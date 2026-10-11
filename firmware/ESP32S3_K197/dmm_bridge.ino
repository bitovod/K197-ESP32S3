// dmm_bridge.ino
//
// Captures the SPI stream the Keithley 197/197A CPU sends to its LCD
// driver on every display update, and forwards the decoded result to
// display.h's setReadingSegments()/setIndicators()/setUnit() and to
// serial_out.h's serialOutUpdateReading().
//
// Protocol reference: NEC uPD7225 register layout for the Keithley
// 197/197A, reverse engineered by "rastro" (eevblog "Keithley 197 LED
// display Hack" thread) and published in machine-readable form by
// alx2009 (github.com/alx2009/displayBoardTester, file K197_set.h).
// Byte offsets below were cross-checked against that source and
// against the minus-sign handling in the original Nano sketch, which
// reads bit 7 of byte 6 - that only lines up if byte 6 is the full
// mode/indicator byte, not byte 5 as the Nano sketch's own comments
// (but not its code) suggested.

#include "driver/spi_slave.h"
#include "driver/gpio.h"
#include "display.h"
#include "serial_out.h"
#include "datalog.h"
#include "ui.h"
#include "alarm.h"
#include "debuglog.h"
#include "trigger_io.h"

// Longest frame the DMM CPU sends per display update (normal mode).
// Confirmed against a live capture on real hardware - the frame is
// actually 17 bytes, not the 15 documented in the original Nano
// sketch's comments (every offset below is 2 higher than what those
// comments claimed, but internally consistent with each other and
// with the digit values actually observed).
//
// Self-test is NOT a fixed 7-byte frame with a 0x20 marker, as
// originally assumed from the Nano sketch (that assumption turned
// out to be wrong once checked against real captures) - self-test
// frames vary in length instead. What's actually confirmed live:
// every frame we've seen - normal 17-byte readings, and every
// self-test variant (a 10-byte "light one segment everywhere"
// broadcast during the lamp test, plus other lengths while it
// counts/animates through a version-ish "3u5" display) - ends in the
// exact same 9-byte payload: modes, six digits, unit, unit2. Only
// the number of leading NEC uPD7225 setup/instruction bytes differs
// (8 for a normal reading, 1 for the lamp-test broadcast - that
// single leading byte is 0x20, which also happens to be the LAST of
// the 8 setup bytes in a normal frame, so it's plausibly just one
// particular uPD7225 instruction rather than a dedicated "self-test"
// flag). So instead of hardcoding an offset per assumed frame
// "type", the payload is anchored to the END of the frame, whatever
// its total length turns out to be.
#define DMM_FRAME_MAX_BYTES       17
#define DMM_FRAME_PAYLOAD_BYTES   9   // modes(1) + digits(6) + unit(1) + unit2(1)

// Byte offsets *within the trailing 9-byte payload* - i.e. relative
// to (byteCount - DMM_FRAME_PAYLOAD_BYTES), not from the start of the
// frame, since the frame's total length (and therefore how much
// leading setup data precedes the payload) varies. Confirmed against
// live captures for both a normal 17-byte frame (payload offset 8)
// and a 10-byte self-test broadcast (payload offset 1):
//   +0      mode indicators + minus sign
//   +1..+6  six digits, MSD (leftmost) to LSD (rightmost)
//   +7      unit prefix (mV, M, u, V, k, mA)
//   +8      unit base (C, ohm, A, remote) + spare bits
#define DMM_PAYLOAD_MODES   0
#define DMM_PAYLOAD_DIGIT0  1
#define DMM_PAYLOAD_UNITS   7
#define DMM_PAYLOAD_UNIT2   8
#define DMM_DIGIT_COUNT  6

// Bit positions inside the payload's modes byte (DMM_PAYLOAD_MODES)
#define DMM_MODE_BIT_AUTO   0
#define DMM_MODE_BIT_REL    1
#define DMM_MODE_BIT_STO    2
#define DMM_MODE_BIT_DB     3
#define DMM_MODE_BIT_AC     4
#define DMM_MODE_BIT_RCL    5
#define DMM_MODE_BIT_BAT    6
#define DMM_MODE_BIT_MINUS  7

// Bit positions inside the payload's unit-prefix byte (DMM_PAYLOAD_UNITS), at most one set
#define DMM_UNIT_BIT_mV  0
#define DMM_UNIT_BIT_M   1
#define DMM_UNIT_BIT_u   2
#define DMM_UNIT_BIT_V   3
#define DMM_UNIT_BIT_k   4
#define DMM_UNIT_BIT_mA  5

// Bit positions inside the payload's unit2 byte (DMM_PAYLOAD_UNIT2) -
// the base unit (at most one set), plus the independent
// remote-control indicator.
#define DMM_UNIT2_BIT_OHM  1
#define DMM_UNIT2_BIT_A    2
#define DMM_UNIT2_BIT_RMT  5

// Segment bit layout shared by all six digit bytes, MSB to LSB:
// c, b, d, g, a, dp, e, f
// The leftmost digit (payload offset DMM_PAYLOAD_DIGIT0) has no dp wired - its
// bit 2 is simply always 0 there, so treating it the same way as the
// other five digits is harmless.
#define DMM_SEG_MASK_NO_DP  0xFB   // clears bit 2 (dp) before a decodeDigit() lookup
#define DMM_SEG_DP_BIT      0x04

// The SPI slave DMA engine expects the rx buffer size itself to be a
// multiple of 4 bytes, or it can write past the end of a shorter
// buffer and corrupt whatever static data follows it. 17 is not a
// multiple of 4, so pad the actual allocation up to 20 - the
// transaction below still only asks for DMM_FRAME_MAX_BYTES*8 bits.
#define DMM_RXBUF_BYTES 20

// dmmCaptureFrame() used to block with portMAX_DELAY, i.e. forever,
// until the DMM's next display update. That's fine on its own (a
// working K197 always keeps updating), but loop() only gets back to
// serialOutProcess() - which drains the SCPI command port - after
// dmmCaptureFrame() returns, so an indefinite block here would make
// the SCPI port indefinitely unresponsive too if the DMM ever went
// quiet (powered off, bus unplugged, ...). A bounded wait instead
// means loop() reliably comes back around at least this often even
// with no DMM activity at all - spi_slave_transmit() times out,
// dmmCaptureFrame() returns 0 (already handled the same as any other
// non-ESP_OK result), and loop() falls through to serialOutProcess().
#define DMM_CAPTURE_TIMEOUT_MS 200

static spi_slave_transaction_t dmmTransaction;
static uint8_t dmmRxBuf[DMM_RXBUF_BYTES] __attribute__((aligned(4)));

// v13 fix for "after power-up the display sometimes stays dark (all
// segments ghost) until the DMM is switched off and on" - the suspected
// cause, not confirmed on the instrument (no scope/analyzer at hand):
// up to v12, dmmCaptureFrame() called spi_slave_transmit() with a 200 ms
// timeout. On a timeout ESP-IDF leaves the transaction QUEUED in the
// driver - spi_slave_transmit() is just queue_trans() + get_trans_result(),
// and only the second part timed out. The next call then memset() the
// same transaction struct and receive buffer while the driver still owned
// them, and queued them AGAIN. While the DMM was still silent (the ESP32
// boots together with it) and in fact whenever a frame took longer than
// 200 ms, the driver held up to 4 copies of one transaction whose struct
// and buffer the CPU kept rewriting while the DMA could be filling them -
// undefined behaviour, and a state that never cleared by itself.
// (host_tests/test_dmm.cpp shows the pile-up; it cannot show what the
// real DMA race does to the data.)
//
// Now exactly ONE transaction is ever in the driver: it is queued once,
// and a timeout just means "keep waiting for the same one next time" -
// the struct and the buffer are only touched again after the driver has
// handed the finished transaction back. On top of that, whatever the real
// cause, the capture task restarts the SPI slave if no usable frame
// arrives for CONFIG_DMM_NO_DATA_MS, and the display shows "Err197"
// meanwhile (dmmCheckNoData() below).
static bool dmmTransQueued = false;

// Pins remembered by dmmBusInit() for dmmBusRestart().
static int8_t dmmSckPin = -1;
static int8_t dmmMosiPin = -1;
static int8_t dmmCsPin = -1;

// No-data watchdog (dmmCheckNoData()): millis() of the last valid frame
// (or of the last bus (re)start), whether "Err197" is shown, and how
// many restarts were needed (for the debug output).
static uint32_t dmmLastValidMs = 0;
static uint32_t dmmLastRestartMs = 0;
static bool dmmNoDataShown = false;
static uint32_t dmmRestartCount = 0;
static bool dmmFirstFrameReported = false; // v13 boot report

// v13 edge counters - tell "the K197 sends nothing" apart from "it sends,
// but nothing usable arrives". They run on GPIO interrupts of the SAME pins
// the SPI slave uses (an input can feed the peripheral and the GPIO
// interrupt at once), independent of the SPI slave.
//   CS falling edges: always counted (about 3.5 per second, nothing).
//   SCK rising edges: 8 per byte at the K197's SPI clock, too many to count
//   all the time - counted only until the first usable frame and while the
//   no-data state lasts, and the counter switches itself off at
//   DMM_SCK_COUNT_CAP edges per window (a noisy line cannot flood core 0).
// dmmCheckNoData() reports and clears both per window.
#define DMM_SCK_COUNT_CAP 20000
static volatile uint32_t dmmCsEdges = 0;
// v22: millis() of the last CS falling edge (DMM off = none for CONFIG_DMM_OFF_QUIET_MS)
static volatile uint32_t dmmLastCsEdgeMs = 0;
// v22: the DMM is switched off (no CS edge for a whole no-data period and CS low); read by power.ino
static volatile bool dmmOff = false;
// v22: power.ino asks for an SPI slave restart right after a wake-up from light sleep
static volatile bool dmmRestartRequested = false;
static volatile uint32_t dmmSckEdges = 0;
static volatile bool dmmSckCapped = false;
static bool dmmSckCounting = false;

static void dmmCsEdgeIsr(void *arg)
{
    (void)arg;
    dmmCsEdges = dmmCsEdges + 1; // single writer; no ++ on a volatile (C++20)
    dmmLastCsEdgeMs = (uint32_t)millis();
}

static void dmmSckEdgeIsr(void *arg)
{
    (void)arg;
    uint32_t n = dmmSckEdges + 1;
    dmmSckEdges = n;
    if (n >= DMM_SCK_COUNT_CAP)
    {
        gpio_intr_disable((gpio_num_t)dmmSckPin);
        dmmSckCapped = true;
    }
}

// (Re)arms the edge interrupts - after every SPI slave (re)start, since the
// driver reconfigures the pins. Only the interrupt type/handler is touched,
// never the pin's direction or routing. The GPIO interrupt service is
// normally already installed (buttons, trigger input); "already installed"
// is fine.
static void dmmEdgeCountersArm()
{
    gpio_install_isr_service(0);
    gpio_set_intr_type((gpio_num_t)dmmCsPin, GPIO_INTR_NEGEDGE);
    gpio_isr_handler_add((gpio_num_t)dmmCsPin, &dmmCsEdgeIsr, NULL);
    gpio_intr_enable((gpio_num_t)dmmCsPin);
    gpio_set_intr_type((gpio_num_t)dmmSckPin, GPIO_INTR_POSEDGE);
    gpio_isr_handler_add((gpio_num_t)dmmSckPin, &dmmSckEdgeIsr, NULL);
    if (dmmSckCounting)
    {
        gpio_intr_enable((gpio_num_t)dmmSckPin);
    }
    else
    {
        gpio_intr_disable((gpio_num_t)dmmSckPin);
    }
}

static void dmmSetSckCounting(bool on)
{
    dmmSckCounting = on;
    dmmSckEdges = 0;
    dmmSckCapped = false;
    if (on)
    {
        gpio_intr_enable((gpio_num_t)dmmSckPin);
    }
    else
    {
        gpio_intr_disable((gpio_num_t)dmmSckPin);
    }
}

// One report line: edges since the last report, then the counters restart.
static void dmmReportEdges(const char *when)
{
    uint32_t cs = dmmCsEdges;
    uint32_t sck = dmmSckEdges;
    bool capped = dmmSckCapped;
    dmmCsEdges = 0;
    dmmSckEdges = 0;
    dmmSckCapped = false;
    if (dmmSckCounting)
    {
        gpio_intr_enable((gpio_num_t)dmmSckPin); // re-enable after a cap
    }
    debugLogAlways("DMM: %s: CS falling edges %lu, SCK edges %lu%s, CS now %s", when, (unsigned long)cs,
                   (unsigned long)sck, dmmSckCounting ? (capped ? " (capped)" : "") : " (not counted)",
                   digitalRead(dmmCsPin) ? "HIGH" : "LOW");
}

// Initializes the SPI slave on the remembered pins; returns the ESP-IDF
// result (the callers report it - setup() over Serial, the capture task
// through the debug queue).
static esp_err_t dmmBusStart()
{
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = dmmMosiPin;
    buscfg.miso_io_num = -1;
    buscfg.sclk_io_num = dmmSckPin;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;

    spi_slave_interface_config_t slvcfg = {};
    slvcfg.mode = 0; // CPOL=0, CPHA=0, matching the original AVR SPI slave setup
    slvcfg.spics_io_num = dmmCsPin;
    slvcfg.queue_size = 3;
    slvcfg.flags = 0;
    slvcfg.post_setup_cb = NULL;
    slvcfg.post_trans_cb = NULL;

    dmmTransQueued = false;
    esp_err_t err = spi_slave_initialize(SPI3_HOST, &buscfg, &slvcfg, SPI_DMA_CH_AUTO);
    dmmLastRestartMs = (uint32_t)millis();
    return err;
}

// Called once from setup().
void dmmBusInit(int8_t sckPin, int8_t mosiPin, int8_t csPin)
{
    dmmSckPin = sckPin;
    dmmMosiPin = mosiPin;
    dmmCsPin = csPin;
    // v13 boot report: the CS line level right now (high = the DMM CPU is
    // idle between frames, low = selected/unpowered/mid-frame).
    pinMode(csPin, INPUT);
    debugLogAlways("boot: DMM CS line %s at SPI init (%lu ms after reset)", digitalRead(csPin) ? "HIGH" : "LOW",
                   (unsigned long)millis());
    esp_err_t err = dmmBusStart();
    if (err != ESP_OK)
    {
        debugLogAlways("boot: SPI3 slave init failed, esp_err_t = %d", (int)err);
    }
    dmmSckCounting = true; // until the first usable frame
    dmmEdgeCountersArm();
    dmmLastValidMs = (uint32_t)millis(); // the no-data watchdog counts from here
}

// Frees and re-initializes the SPI slave (drops a transaction that may be
// stuck in the driver). Only called from the capture task, when nothing
// has arrived for a long time, so no transfer is cut short in practice.
static void dmmBusRestart()
{
    spi_slave_free(SPI3_HOST);
    dmmRestartCount++;
    esp_err_t err = dmmBusStart();
    if (err != ESP_OK)
    {
        debugLogAlways("DMM: SPI3 slave re-init failed, esp_err_t = %d", (int)err);
    }
    dmmEdgeCountersArm();
}

// Queues the one receive transaction. Struct and buffer are cleared here,
// while the driver does NOT own them.
static bool dmmQueueTransaction()
{
    memset(&dmmTransaction, 0, sizeof(dmmTransaction));
    memset(dmmRxBuf, 0, sizeof(dmmRxBuf));
    dmmTransaction.length = DMM_FRAME_MAX_BYTES * 8; // the longest frame; shorter ones end with CS
    dmmTransaction.tx_buffer = NULL;
    dmmTransaction.rx_buffer = dmmRxBuf;
    dmmTransQueued = (spi_slave_queue_trans(SPI3_HOST, &dmmTransaction, 0) == ESP_OK);
    return dmmTransQueued;
}

void dmmDumpFrame(int byteCount); // defined below

// Waits up to DMM_CAPTURE_TIMEOUT_MS for the DMM to finish one
// CS-framed transfer, then returns the number of bytes actually
// captured (0 on timeout - not an error, just "nothing this pass",
// the same transaction stays queued for the next call - or something
// else if a frame got cut short; the caller is expected to sanity-check
// the count before trusting it). After a non-zero return the bytes are
// in dmmRxBuf until the next call.
int dmmCaptureFrame()
{
    if (!dmmTransQueued && !dmmQueueTransaction())
    {
        // Cannot queue (should not happen with one transaction and a
        // queue of 3) - wait like a timeout so the loop does not spin.
        vTaskDelay(pdMS_TO_TICKS(DMM_CAPTURE_TIMEOUT_MS));
        return 0;
    }

    spi_slave_transaction_t *done = NULL;
    esp_err_t err = spi_slave_get_trans_result(SPI3_HOST, &done, pdMS_TO_TICKS(DMM_CAPTURE_TIMEOUT_MS));
    if (err != ESP_OK)
    {
        return 0; // timeout - the transaction is still queued, wait for it next time
    }
    dmmTransQueued = false; // ours again; the next call queues a fresh one

    int byteCount = (int)(dmmTransaction.trans_len / 8);
    if (byteCount > DMM_FRAME_MAX_BYTES)
    {
        // Physically impossible given length above - something is
        // wrong (extra clock edges from noise/missing pull-up, or CS
        // not framing what we think it frames). Print whatever
        // landed in the buffer, capped to its real capacity, instead
        // of discarding it - useful while bringing the wiring up.
        debugLogAlways("dmmCaptureFrame: implausible byte count %d", byteCount);
        dmmDumpFrame(DMM_RXBUF_BYTES);
        return 0;
    }
    return byteCount;
}

// "Err197" in the K197's own 7-segment digits, display.h segment bits
// (bit0=a, bit1=g, bit2=d, bit3=f, bit4=b, bit5=e, bit6=c): E, r, r, 1,
// open 9 (as the K197 draws it), 7.
static const uint8_t dmmErrSegbits[DMM_DIGIT_COUNT] = {
    0x2F, // E: a d e f g
    0x22, // r: e g
    0x22, // r
    0x50, // 1: b c
    0x5B, // 9: a b c f g
    0x51  // 7: a b c
};

// v22: "oFF197" - the K197 is switched off (not a single CS edge for a whole
// no-data period, CS low through the input divider). Small o: a capital O
// would read as a zero.
static const uint8_t dmmOffSegbits[DMM_DIGIT_COUNT] = {
    0x66, // o: c d e g
    0x2B, // F: a e f g
    0x2B, // F
    0x50, // 1
    0x5B, // 9 (open)
    0x51  // 7
};

bool dmmIsOff()
{
    return dmmOff;
}

void dmmRequestBusRestart()
{
    dmmRestartRequested = true;
}

// v22: the K197 looks switched off - CS low now, and no CS edge for the
// last CONFIG_DMM_OFF_QUIET_MS. Not "none in the whole period": while its
// supply decays the K197 still sends a few broken frames, and the restart of
// the SPI slave itself puts a glitch or two on the pin. A running K197 (or one
// that sends frames the capture cannot use) gives a CS edge every ~285 ms.
static bool dmmLooksOff()
{
    return ((uint32_t)((uint32_t)millis() - dmmLastCsEdgeMs) >= (uint32_t)CONFIG_DMM_OFF_QUIET_MS) &&
           (digitalRead(dmmCsPin) == LOW);
}

// v22: Err197 or oFF197 on the LCD replica, from what the last period showed.
static void dmmShowNoData(bool off)
{
    setReadingSegments(off ? dmmOffSegbits : dmmErrSegbits, -1);
    setIndicators(0);
    setUnit(KEITHLEY_UNIT_NONE);
    if (off)
    {
        uiRequestHome();     // whatever screen was open: back to the main display, which says oFF197
        datalogNoteDmmOff(); // v22: the readings after it is switched on again settle for longer
    }
}

// Called by the capture task after every dmmCaptureFrame() (at least every
// DMM_CAPTURE_TIMEOUT_MS). If no valid frame has arrived for
// CONFIG_DMM_NO_DATA_MS, shows "Err197" on the LCD replica (no indicators,
// no unit; the statistics screen shows "--.----" as its live value) and
// restarts the SPI slave - again every CONFIG_DMM_NO_DATA_MS while the
// silence lasts. The next valid frame simply overwrites the display.
void dmmCheckNoData()
{
    uint32_t nowMs = (uint32_t)millis();
    if (dmmRestartRequested)
    {
        // v22: just woken from light sleep - the peripheral may not have survived it, start it afresh
        dmmRestartRequested = false;
        dmmBusRestart();
    }
    if ((uint32_t)(nowMs - dmmLastValidMs) < (uint32_t)CONFIG_DMM_NO_DATA_MS)
    {
        return;
    }
    bool decidedNow = false;
    if (!dmmNoDataShown)
    {
        dmmNoDataShown = true;
        decidedNow = true;
        // v22: CS quiet for a while and low = the K197 is switched off
        dmmOff = dmmLooksOff();
        dmmShowNoData(dmmOff);
        const char blank[DMM_DIGIT_COUNT] = {' ', ' ', ' ', ' ', ' ', ' '};
        uiUpdateLiveReading(blank, -1, 0, KEITHLEY_UNIT_NONE); // not a reading -> placeholder
        alarmUpdateReading(blank, -1, 0, KEITHLEY_UNIT_NONE); // v17: no reading -> no alarm
        datalogUpdateReading(blank, -1, 0, KEITHLEY_UNIT_NONE); // v20: not logged, but the next readings settle again
        debugLogAlways("DMM: no valid frame for %u ms - showing %s", (unsigned)CONFIG_DMM_NO_DATA_MS, dmmOff ? "oFF197" : "Err197");
    }
    if ((uint32_t)(nowMs - dmmLastRestartMs) >= (uint32_t)CONFIG_DMM_NO_DATA_MS)
    {
        // v22: every further silent period decides again (the K197 may be switched off later, or start sending garbage)
        bool off = dmmLooksOff();
        if (decidedNow)
        {
            off = dmmOff; // decided a moment ago above, over the same period
        }
        if (off != dmmOff)
        {
            dmmOff = off;
            dmmShowNoData(off);
            debugLogAlways("DMM: now showing %s", off ? "oFF197" : "Err197");
        }
        // Edges seen during the silent period just ended: 0 CS edges = the
        // K197 sent nothing at all (its CPU is not running); edges but no
        // usable frame = something arrives and the capture side is suspect.
        dmmReportEdges("last period without a usable frame");
        if (!dmmSckCounting)
        {
            dmmSetSckCounting(true); // count SCK too while the silence lasts
        }
        dmmBusRestart(); // also resets dmmLastRestartMs
        debugLogAlways("DMM: SPI slave restarted (%u so far)", (unsigned)dmmRestartCount);
    }
}

// Prints the raw captured bytes as hex - "frame (N bytes): XX XX ...".
// Called from two places: dmmCaptureFrame()'s "implausible byte
// count" branch above (unconditionally, since that's already an error
// worth always seeing), and dmmApplyFrame() below, gated behind
// SYSTem:DEBUG (serialOutDebugTraceAllowed()) for every ordinarily
// valid frame - useful while chasing a decode problem, to see exactly
// what came off the wire rather than only what it decoded to.
void dmmDumpFrame(int byteCount)
{
    // One message, queued without waiting (debuglog.h): this runs in the DMM
    // capture task, which must never wait for the USB link.
    char line[DEBUGLOG_MESSAGE_CHARS + 1];
    int length = snprintf(line, sizeof(line), "frame (%d bytes): ", byteCount);
    for (int i = 0; i < byteCount && length + 3 < (int)sizeof(line); i++)
    {
        length += snprintf(line + length, sizeof(line) - (size_t)length, "%02X ", dmmRxBuf[i]);
    }
    debugLogAlways("%s", line);
}

// Maps a digit byte (with the dp bit already masked out) to the
// character it represents. Returns ' ' for blank and for any pattern
// that is not a valid digit - the leftmost digit only ever shows
// blank or '1', used for +-1 overflow.
//
// Not on the pixel-rendering path (setReadingSegments() renders
// straight from remapSegBits() so hardware-accurate shapes like the
// open '9' are never round-tripped through an ASCII digit), but IS
// used by dmmApplyFrame() to build the digits[] array
// serialOutUpdateReading() (serial_out.ino) caches for the READ?
// text transcript.
//
// Note on '9': confirmed against a live capture that this display
// draws 9 as an "open" digit without the d (bottom) segment, i.e.
// segments a,b,c,f,g only - not the more common closed variant with
// d also lit. 0xD9, not 0xF9.
//
// 'L' (segments d,e,f) is not a measurement digit - it only appears
// as the second half of the "OL" overload indication, confirmed from
// a live capture during a resistance-range overload. The 'O' half is
// bit-identical to '0' on a 7-segment display, so no separate case is
// needed for it.
static char decodeDigit(uint8_t segments)
{
    switch (segments)
    {
        case 0xEB: return '0';
        case 0xC0: return '1';
        case 0x7A: return '2';
        case 0xF8: return '3';
        case 0xD1: return '4';
        case 0xB9: return '5';
        case 0xBB: return '6';
        case 0xC8: return '7';
        case 0xFB: return '8';
        case 0xD9: return '9';
        case 0x23: return 'L';
        default:   return ' ';
    }
}

// Note: this project tried leading-zero blanking + a "floating"
// minus sign here (making a reading like "00.1234" show as "0.1234",
// and "-0000.02" show as "-0.02" with the sign right next to the
// first significant digit) and then reverted it - the author found
// the sign "jumping" between positions depending on the reading hurt
// readability more than the extra padding zeros ever did. Kept only
// as a note in case it comes up again; see docs/DEVELOPMENT.md for the
// full writeup of what was tried.

// Converts one raw K197 digit byte's segment bits (bit7=c, bit6=b,
// bit5=d, bit4=g, bit3=a, bit1=e, bit0=f - bit2/dp is handled
// separately in dmmApplyFrame() and simply ignored here) into
// display.h's own segment bit order (bit0=a, bit1=g, bit2=d, bit3=f,
// bit4=b, bit5=e, bit6=c - the scheme keithley_char_segbits already
// uses). This is what lets setReadingSegments() take the DMM's own
// bits directly instead of round-tripping through decodeDigit()'s
// ASCII mapping first.
//
// That round trip is not just an extra step - it silently changes
// digit 9's shape. The K197 draws 9 "open" (segments a,b,c,f,g; d
// and e off), confirmed live, but keithley_char_segbits[9] encodes
// the conventional "closed" 9 (d on) that the artwork/table was
// built around for the character path. Feeding raw bits straight
// through this function reproduces the real hardware's shape;
// decodeDigit()+setReading() would not.
static uint8_t remapSegBits(uint8_t k197raw)
{
    uint8_t out = 0;
    if (bitRead(k197raw, 7)) out |= (1 << 6); // c
    if (bitRead(k197raw, 6)) out |= (1 << 4); // b
    if (bitRead(k197raw, 5)) out |= (1 << 2); // d
    if (bitRead(k197raw, 4)) out |= (1 << 1); // g
    if (bitRead(k197raw, 3)) out |= (1 << 0); // a
    if (bitRead(k197raw, 1)) out |= (1 << 5); // e
    if (bitRead(k197raw, 0)) out |= (1 << 3); // f
    return out;
}

// Combines the prefix byte and base-unit byte into one of setUnit()'s
// codes (0 = hidden, 1..8 = mV, V, uA, mA, A, Ohm, kOhm, MOhm, 10 =
// dB). dB is an override, not an additional bit to combine: on real
// hardware the normal prefix/base bytes go blank while dB mode is
// active, confirmed against a live capture, so when dbMode is set we
// return the dB unit immediately regardless of what those bytes say.
//
// The original LCD renders each unit as two independent glyphs - a
// base symbol (V, A, Ohm) plus, where relevant, a separate prefix
// glyph (m, u, k, M) next to it - rather than one combined symbol.
// So every prefix bit below only means anything when ANDed with its
// matching base bit; confirmed on real hardware for mV (m+V both set
// together) and for mA/uA (m or u alongside the A base bit).
static uint8_t decodeUnit(uint8_t unitsByte, uint8_t unit2Byte, bool dbMode)
{
    if (dbMode)
        return KEITHLEY_UNIT_DB;

    bool mPrefixV = bitRead(unitsByte, DMM_UNIT_BIT_mV); // "m" glyph next to V
    bool M        = bitRead(unitsByte, DMM_UNIT_BIT_M);  // "M" glyph next to Ohm
    bool u        = bitRead(unitsByte, DMM_UNIT_BIT_u);  // "u" glyph next to A
    bool V        = bitRead(unitsByte, DMM_UNIT_BIT_V);
    bool k        = bitRead(unitsByte, DMM_UNIT_BIT_k);  // "k" glyph next to Ohm
    bool mPrefixA = bitRead(unitsByte, DMM_UNIT_BIT_mA); // "m" glyph next to A
    bool ohm      = bitRead(unit2Byte, DMM_UNIT2_BIT_OHM);
    bool amp      = bitRead(unit2Byte, DMM_UNIT2_BIT_A);

    if (mPrefixV && V)  return KEITHLEY_UNIT_MV;
    if (V)              return KEITHLEY_UNIT_V;
    if (u && amp)        return KEITHLEY_UNIT_UA;
    if (mPrefixA && amp) return KEITHLEY_UNIT_MA;
    if (amp)             return KEITHLEY_UNIT_A;
    if (ohm && k)        return KEITHLEY_UNIT_KOHM;
    if (ohm && M)        return KEITHLEY_UNIT_MOHM;
    if (ohm)             return KEITHLEY_UNIT_OHM;
    return KEITHLEY_UNIT_NONE;
}

// Combines the mode byte and the remote bit (which lives in the
// second unit byte, not with the other mode bits) into the bitfield
// setIndicators() expects. dB is intentionally excluded - it now
// drives the unit display instead (see decodeUnit()), matching how
// the original LCD blanks the normal unit and shows only "dB".
static uint16_t decodeIndicators(uint8_t modesByte, uint8_t unit2Byte)
{
    uint16_t bits = 0;

    if (bitRead(modesByte, DMM_MODE_BIT_AUTO))  bits |= KEITHLEY_BIT_AUT;
    if (bitRead(modesByte, DMM_MODE_BIT_REL))   bits |= KEITHLEY_BIT_REL;
    if (bitRead(modesByte, DMM_MODE_BIT_STO))   bits |= KEITHLEY_BIT_STO;
    if (bitRead(modesByte, DMM_MODE_BIT_RCL))   bits |= KEITHLEY_BIT_RCL;
    if (bitRead(modesByte, DMM_MODE_BIT_AC))    bits |= KEITHLEY_BIT_AC;
    if (bitRead(modesByte, DMM_MODE_BIT_BAT))   bits |= KEITHLEY_BIT_BAT;
    if (bitRead(modesByte, DMM_MODE_BIT_MINUS)) bits |= KEITHLEY_BIT_MINUS;
    if (bitRead(unit2Byte, DMM_UNIT2_BIT_RMT))  bits |= KEITHLEY_BIT_REM;

    return bits;
}

// Decodes one captured frame and pushes it to the display, and
// caches it via serialOutUpdateReading() so a subsequent SCPI READ?
// answers with it. Frames too short to even
// contain the 9-byte payload, or longer than the longest frame we've
// ever seen (a full normal reading), are dropped rather than risking
// garbage on screen from a partial/glitched capture - anything in
// between is accepted, whatever its exact length, since the payload
// is anchored to the end of the frame (see the comment on
// DMM_FRAME_PAYLOAD_BYTES above).
void dmmApplyFrame(int byteCount)
{
    if (byteCount < DMM_FRAME_PAYLOAD_BYTES || byteCount > DMM_FRAME_MAX_BYTES)
    {
        return;
    }

    // No-data watchdog: a frame of plausible length whose payload is not all
    // zero means the DMM is talking. An all-zero payload (nothing lit at all -
    // never seen from a working K197, but exactly what a capture that went
    // wrong looks like: the "all segments ghost" state) is still shown, but
    // does not count, so the watchdog restarts the SPI slave if it persists.
    {
        int payloadStart = byteCount - DMM_FRAME_PAYLOAD_BYTES;
        bool anyBit = false;
        for (int i = 0; i < DMM_FRAME_PAYLOAD_BYTES; i++)
        {
            if (dmmRxBuf[payloadStart + i] != 0)
            {
                anyBit = true;
                break;
            }
        }
        if (anyBit)
        {
            dmmLastValidMs = (uint32_t)millis();
            dmmOff = false;
            if (!dmmFirstFrameReported)
            {
                dmmFirstFrameReported = true;
                debugLogAlways("DMM: first valid frame %lu ms after reset (%d bytes)", (unsigned long)dmmLastValidMs, byteCount);
                dmmReportEdges("edges up to the first frame");
            }
            if (dmmNoDataShown)
            {
                dmmNoDataShown = false;
                debugLogAlways("DMM: frames again after %u SPI slave restart(s)", (unsigned)dmmRestartCount);
                dmmReportEdges("edges until frames came back");
            }
            if (dmmSckCounting)
            {
                dmmSetSckCounting(false); // frames flow: no SCK interrupts in normal operation
            }
        }
        else if (dmmNoDataShown)
        {
            return; // keep "Err197" instead of a blank picture
        }
    }

    // SYSTem:DEBUG's raw half of the trace - dumps exactly the bytes
    // this call is about to decode, same hex format as the
    // "implausible byte count" error path above uses. The decoded
    // half (what those bytes turned into) prints separately, from
    // serialOutUpdateReading() near the end of this function - with
    // debug on, a full pass looks like "frame (17 bytes): 20 30 4A
    // ..." immediately followed by the human-readable reading it
    // decoded to, which is what was actually asked for here: being
    // able to see the raw packet again, not just the display content,
    // when hunting a decode problem.
    if (serialOutDebugTraceAllowed())
        dmmDumpFrame(byteCount);

    int payloadOffset = byteCount - DMM_FRAME_PAYLOAD_BYTES;

    char digits[DMM_DIGIT_COUNT];
    uint8_t segbits[DMM_DIGIT_COUNT];
    int8_t dpPos = -1;

    for (int i = 0; i < DMM_DIGIT_COUNT; i++)
    {
        uint8_t raw = dmmRxBuf[payloadOffset + DMM_PAYLOAD_DIGIT0 + i];
        // digits[i] isn't used for rendering (setReadingSegments()
        // below draws straight from segbits[i], so hardware-accurate
        // shapes like the open '9' are preserved untouched) - it's
        // only for serialOutUpdateReading()'s cached text transcript.
        digits[i] = decodeDigit(raw & DMM_SEG_MASK_NO_DP);
        segbits[i] = remapSegBits(raw);
        if (raw & DMM_SEG_DP_BIT)
        {
            // The dp bit stored alongside digit i actually lights the
            // dot to the LEFT of digit i on the K197's LCD glass, not
            // to its right - confirmed by comparing range switches
            // against the original display. setReadingSegments()'s
            // dpPos places the dot to the right of the given index,
            // so the digit to the left of i is what we want here.
            // This also matches the original Nano sketch's own "move
            // dp to next higher digit" remap, which turns out not to
            // have been a quirk of that project's LED module after
            // all.
            dpPos = i - 1;
        }
    }

    setReadingSegments(segbits, dpPos);

    uint8_t modesByte = dmmRxBuf[payloadOffset + DMM_PAYLOAD_MODES];
    uint8_t unit2Byte = dmmRxBuf[payloadOffset + DMM_PAYLOAD_UNIT2];
    uint16_t indicatorBits = decodeIndicators(modesByte, unit2Byte);
    uint8_t unit = decodeUnit(dmmRxBuf[payloadOffset + DMM_PAYLOAD_UNITS], unit2Byte,
                               bitRead(modesByte, DMM_MODE_BIT_DB));
    setIndicators(indicatorBits);
    setUnit(unit);

    serialOutUpdateReading(digits, dpPos, indicatorBits, unit);
    datalogUpdateReading(digits, dpPos, indicatorBits, unit);
    uiUpdateLiveReading(digits, dpPos, indicatorBits, unit); // v8: for the quick statistics screen, see ui.ino
    alarmUpdateReading(digits, dpPos, indicatorBits, unit);  // v17: limit alarm (triangle above the minus, beep)
    triggerIoPulseMeteringComplete(); // v13: METERING COMPLETE OUTPUT, one pulse per complete frame
}
