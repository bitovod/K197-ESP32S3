// Host test for dmm_bridge.ino (v13): SPI slave capture against a simulated K197 that starts sending only
// some time after the ESP32 - the power-up case where the display could stay dark - and the no-data
// watchdog ("Err197", SPI slave restart). The ESP-IDF SPI slave driver is modelled in stubs/driver/spi_slave.h.
#include <Arduino.h>
#include <stdio.h>
#include <vector>
#include <string>
#include <stdarg.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#ifndef bitRead
#define bitRead(v, b) (((v) >> (b)) & 1)
#endif
#include "config.h"
#include "display.h"
#include "keithley_shapes.h"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

// ---- stand-ins for what dmmApplyFrame()/dmmCheckNoData() feed ----
static uint8_t gSeg[6];
static int8_t gDp = -2;
static uint16_t gInd = 0xFFFF;
static uint8_t gUnit = 0xFF;
static int gGoodFrames = 0;  // frames that decoded to the reading the fake DMM sends
static int gBlankFrames = 0; // frames of valid length but all-zero content (the v12 symptom)
static int8_t gLiveDp = -2;
void setReadingSegments(const uint8_t segbits[KEITHLEY_DIGIT_COUNT], int8_t dpPos) { memcpy(gSeg, segbits, 6); gDp = dpPos; }
void setIndicators(uint16_t bits) { gInd = bits; }
void setUnit(uint8_t unit) { gUnit = unit; }
bool serialOutDebugTraceAllowed() { return false; }
void serialOutUpdateReading(const char digits[KEITHLEY_DIGIT_COUNT], int8_t dpPos, uint16_t, uint8_t unit)
{
    if (memcmp(digits, "012345", 6) == 0 && dpPos == 1 && unit == KEITHLEY_UNIT_V) gGoodFrames++;
    else if (memcmp(digits, "      ", 6) == 0) gBlankFrames++;
}
void datalogUpdateReading(const char *, int8_t, uint16_t, uint8_t) {}
void uiUpdateLiveReading(const char *, int8_t dpPos, uint16_t, uint8_t) { gLiveDp = dpPos; }
void alarmUpdateReading(const char *, int8_t, uint16_t, uint8_t) {} // v17
static int gHomeRequests = 0;
void uiRequestHome() { gHomeRequests++; } // v22
static int gHoldStops = 0;
void holdStop() { gHoldStops++; } // v24: oFF197 ends HOLD
void holdUpdateReading(const char *, int8_t, uint16_t, uint8_t) {} // v24
static int gDmmOffNotes = 0;
void datalogNoteDmmOff() { gDmmOffNotes++; } // v22
static std::vector<std::string> gLog;
void debugLogAlways(const char *format, ...) { char b[200]; va_list a; va_start(a, format); vsnprintf(b, sizeof(b), format, a); va_end(a); gLog.push_back(b); }
void debugLogf(const char *, ...) {}
void triggerIoPulseMeteringComplete() {}

#include "dmm_bridge.ino"

// The v12 capture loop, kept here to show the failure it had.
static int oldCaptureFrame()
{
    memset(&dmmTransaction, 0, sizeof(dmmTransaction));
    memset(dmmRxBuf, 0, sizeof(dmmRxBuf));
    dmmTransaction.length = DMM_FRAME_MAX_BYTES * 8;
    dmmTransaction.rx_buffer = dmmRxBuf;
    if (spi_slave_transmit(SPI3_HOST, &dmmTransaction, pdMS_TO_TICKS(DMM_CAPTURE_TIMEOUT_MS)) != ESP_OK) return 0;
    int n = (int)(dmmTransaction.trans_len / 8);
    return n > DMM_FRAME_MAX_BYTES ? 0 : n;
}

// A normal 17-byte frame: 8 setup bytes, then modes, digits 0 1 2 3 4 5, V, unit2. The dp bit of a digit byte
// lights the dot to the LEFT of that digit, so it sits in digit 2 for "01.2345".
static void makeFrame(uint8_t *f)
{
    memset(f, 0, 17);
    for (int i = 0; i < 8; i++) f[i] = 0x20;
    const uint8_t d[6] = {0xEB, 0xC0, 0x7A | 0x04, 0xF8, 0xD1, 0xB9};
    f[8] = 0x01; // AUTO
    memcpy(f + 9, d, 6);
    f[15] = 0x08; // V
    f[16] = 0x00;
}

static void resetAll(uint32_t dmmStartDelayMs)
{
    FakeSpiSlave &s = fakeSpi();
    if (s.initialized) spi_slave_free(SPI3_HOST);
    s = FakeSpiSlave();
    makeFrame(s.frame);
    gGoodFrames = gBlankFrames = 0;
    gLog.clear();
    dmmNoDataShown = false;
    dmmRestartCount = 0;
    dmmCsEdges = 0; // v22: the delivered frames count as CS edges now
    dmmLastCsEdgeMs = 0;
    dmmOff = false;
    gpioLevel[DMM_CS_PIN] = LOW;
    dmmBusInit(DMM_SCK_PIN, DMM_MOSI_PIN, DMM_CS_PIN);
    s.dmmOn = true;
    s.nextFrameMs = fakeNow() + dmmStartDelayMs;
}

int main()
{
    // 1. the v12 loop, for the record (no check): it left up to 4 copies of the same transaction in the driver
    //    ("pending" below) and rewrote that struct and buffer while the driver still owned them. This model
    //    delivers a frame atomically, so it cannot reproduce what a DMA transfer racing with the CPU's memset()
    //    does on the real chip - it only shows the pile-up itself.
    {
        resetAll(2000);
        size_t maxPending = 0;
        for (int i = 0; i < 200; i++)
        {
            int n = oldCaptureFrame();
            if (n > 0) dmmApplyFrame(n);
            if (fakeSpi().pending.size() > maxPending) maxPending = fakeSpi().pending.size();
        }
        printf("1. v12 capture, DMM silent for 2 s after boot: up to %zu copies of the one transaction queued at once\n", maxPending);
    }
    // 2. the v13 loop: whatever the start delay, every frame after the DMM starts is captured and correct
    const uint32_t delays[5] = {0, 150, 700, 2000, 9000};
    for (int k = 0; k < 5; k++)
    {
        resetAll(delays[k]);
        for (int i = 0; i < 200; i++) { int n = dmmCaptureFrame(); if (n > 0) dmmApplyFrame(n); dmmCheckNoData(); }
        printf("2. v13 capture, DMM silent for %u ms after boot: %d good frames, %d blank, %d lost of %d sent, %d SPI restarts\n",
               delays[k], gGoodFrames, gBlankFrames, fakeSpi().lost, fakeSpi().sent, (int)dmmRestartCount);
        CHECK(gBlankFrames == 0, "never a blank frame");
        CHECK(gGoodFrames >= fakeSpi().sent - 1 && fakeSpi().lost <= 1, "every frame captured (one may fall into the instant before the first transaction is queued)");
        CHECK(!dmmNoDataShown, "no Err197 while data flows");
        CHECK(fakeSpi().pending.size() <= 1, "never more than one transaction in the driver");
    }
    // 3. watchdog: no DMM at all (switched off: no CS edge, CS low) -> oFF197 after CONFIG_DMM_NO_DATA_MS (v22),
    //    SPI slave restarted every period; CS edges without frames -> Err197; data -> normal again
    {
        resetAll(0);
        fakeSpi().dmmOn = false;
        fakeGpioEdges(DMM_CS_PIN, 0); // (armed)
        uint32_t t0 = fakeNow();
        bool powerDownEdge = false;
        while (!dmmNoDataShown && fakeNow() - t0 < 60000)
        {
            int n = dmmCaptureFrame(); if (n > 0) dmmApplyFrame(n);
            if (!powerDownEdge) { fakeGpioEdges(DMM_CS_PIN, 9); powerDownEdge = true; } // v22: switching off = a few broken frames
            dmmCheckNoData();
        }
        uint32_t after = fakeNow() - t0;
        printf("3. no DMM: oFF197 after %u ms\n", after);
        CHECK(dmmNoDataShown && after >= CONFIG_DMM_NO_DATA_MS && after <= CONFIG_DMM_NO_DATA_MS + 500, "oFF197 after the no-data time");
        const uint8_t off[6] = {0x66, 0x2B, 0x2B, 0x50, 0x5B, 0x51};
        CHECK(memcmp(gSeg, off, 6) == 0 && gDp == -1 && gInd == 0 && gUnit == KEITHLEY_UNIT_NONE && dmmIsOff(), "display shows oFF197, nothing else");
        CHECK(gHomeRequests > 0 && gDmmOffNotes > 0, "oFF197: the UI is sent back to the main display, the log told (long settling next)");
        CHECK(gHoldStops > 0, "v24: oFF197 ends HOLD");
        CHECK(gLiveDp == -1, "statistics live value set to 'not a reading'");
        CHECK(dmmRestartCount == 1 && fakeSpi().inits == 2, "SPI slave restarted once");
        bool reported = false;
        for (auto &m : gLog) if (m.find("CS falling edges 9, SCK edges 0") != std::string::npos) reported = true;
        CHECK(reported, "edge report: only the power-down edges on CS while the DMM is silent");
        gLog.clear();
        fakeGpioEdges(DMM_SCK_PIN, 500);
        // a K197 that sends something the capture does not turn into frames: a CS edge every 200 ms for 12 s
        for (int i = 0; i < 120; i++)
        {
            int n = dmmCaptureFrame(); if (n > 0) dmmApplyFrame(n);
            if (i < 60) fakeGpioEdges(DMM_CS_PIN, 1);
            dmmCheckNoData();
        } // 24 s more
        bool edgesSeen = false;
        for (auto &m : gLog) if (m.find("SCK edges 500,") != std::string::npos && m.find("CS falling edges 0,") == std::string::npos) edgesSeen = true;
        CHECK(edgesSeen, "edge report counts CS and SCK edges of the silent period");
        bool errShown = false, offAgain = false;
        for (auto &m : gLog)
        {
            if (m.find("now showing Err197") != std::string::npos) errShown = true;
            if (errShown && m.find("now showing oFF197") != std::string::npos) offAgain = true;
        }
        CHECK(errShown, "CS edges without frames: Err197 for that period (not off)");
        CHECK(offAgain && memcmp(gSeg, off, 6) == 0 && dmmIsOff(), "quiet again in the next period: oFF197");
        fakeGpioEdges(DMM_SCK_PIN, DMM_SCK_COUNT_CAP + 50);
        CHECK(!fakeGpioIntr(DMM_SCK_PIN).enabled && dmmSckCapped, "SCK counter switches itself off at the cap");
        printf("   restarts after %u ms of silence: %u\n", fakeNow() - t0, (unsigned)dmmRestartCount);
        CHECK(dmmRestartCount >= 3, "restarted again every period");
        FakeSpiSlave &s = fakeSpi();
        s.dmmOn = true;
        s.nextFrameMs = fakeNow() + 100;
        for (int i = 0; i < 20; i++) { int n = dmmCaptureFrame(); if (n > 0) dmmApplyFrame(n); dmmCheckNoData(); }
        CHECK(!dmmNoDataShown && gGoodFrames > 0 && gDp == 1 && !dmmIsOff(), "data again: the reading replaces oFF197");
        CHECK(!fakeGpioIntr(DMM_SCK_PIN).enabled && fakeGpioIntr(DMM_CS_PIN).enabled, "frames flow: SCK counting off, CS counting on");
        unsigned restarts = (unsigned)dmmRestartCount;
        for (int i = 0; i < 200; i++) { int n = dmmCaptureFrame(); if (n > 0) dmmApplyFrame(n); dmmCheckNoData(); }
        CHECK(dmmRestartCount == restarts, "no restarts while data flows");
    }
    // 3b. v22: no edges but CS stuck HIGH (a K197 with power but a stopped CPU) -> Err197, not off
    {
        resetAll(0);
        fakeSpi().dmmOn = false;
        gpioLevel[DMM_CS_PIN] = HIGH;
        uint32_t t0 = fakeNow();
        while (!dmmNoDataShown && fakeNow() - t0 < 60000) { int n = dmmCaptureFrame(); if (n > 0) dmmApplyFrame(n); dmmCheckNoData(); }
        const uint8_t err[6] = {0x2F, 0x22, 0x22, 0x50, 0x5B, 0x51};
        CHECK(memcmp(gSeg, err, 6) == 0 && !dmmIsOff(), "CS high without edges: Err197");
        // light sleep wake-up: the restart request is served on the next pass
        unsigned restarts = (unsigned)dmmRestartCount;
        dmmRequestBusRestart();
        dmmCheckNoData();
        CHECK(dmmRestartCount == restarts + 1, "dmmRequestBusRestart(): restarted on the next pass");
        gpioLevel[DMM_CS_PIN] = LOW;
        printf("3b. CS high without edges: Err197; restart on request\n");
    }
    // 4. frames of valid length but all-zero payload (a capture gone wrong) do not count as data: Err197 + restarts
    {
        resetAll(0);
        memset(fakeSpi().frame, 0, sizeof(fakeSpi().frame));
        uint32_t t0 = fakeNow();
        while (!dmmNoDataShown && fakeNow() - t0 < 60000) { int n = dmmCaptureFrame(); if (n > 0) dmmApplyFrame(n); dmmCheckNoData(); }
        printf("4. all-zero frames: Err197 after %u ms, %d frames received\n", fakeNow() - t0, fakeSpi().sent);
        CHECK(dmmNoDataShown && dmmRestartCount == 1, "all-zero frames: Err197 and an SPI restart");
        for (int i = 0; i < 20; i++) { int n = dmmCaptureFrame(); if (n > 0) dmmApplyFrame(n); dmmCheckNoData(); }
        const uint8_t err[6] = {0x2F, 0x22, 0x22, 0x50, 0x5B, 0x51};
        CHECK(memcmp(gSeg, err, 6) == 0, "Err197 stays (not overwritten by the blank frames)");
        makeFrame(fakeSpi().frame);
        for (int i = 0; i < 20; i++) { int n = dmmCaptureFrame(); if (n > 0) dmmApplyFrame(n); dmmCheckNoData(); }
        CHECK(!dmmNoDataShown && gDp == 1, "a real frame clears it");
    }
    printf(failures ? "FAILURES: %d\n" : "ALL OK\n", failures);
    return failures != 0;
}
