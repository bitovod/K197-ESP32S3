// hold.ino
// v24: DATA HOLD - see hold.h.
//
// Threads: holdStart() / holdStop() come from the UI task (STO) or the SCPI
// task (MEASure:HOLD), holdUpdateReading() from the capture task, holdTick()
// from the esp_timer task. The state is one byte changed under holdMux; the
// AUTO HOLD run belongs to the capture task alone (a start / stop bumps
// holdGeneration, and the capture task resets its run when it sees that). The
// capture task never marks the log (datalog: any task except the capture
// task) - it counts a capture event, and holdTick() marks and beeps.

#include "hold.h"
#include "config.h"
#include "settings.h"
#include "display.h"
#include "framebuffer.h" // FB_HOLD_*
#include "marker.h"
#include "beeper.h"
#include "datalog.h"
#include "reading_math.h"
#include "debuglog.h"
#include <esp_timer.h>

static portMUX_TYPE holdMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t holdSt = HOLD_STATE_OFF;
static volatile uint32_t holdGeneration = 0;

// AUTO HOLD run of stable readings (capture task only)
static uint32_t holdRunGeneration = 0;
static const char *holdRunFunction = NULL; // keithleyFunctionPrefix() - static strings, compared by address
static uint8_t holdRunUnit = 0;
static int8_t holdRunDp = -1;
static long holdRunMin = 0;
static long holdRunMax = 0;
static uint8_t holdRunLen = 0;
static bool holdRunCaptured = false;
// the reading held by AUTO HOLD (capture task only)
static bool holdHeldValid = false;
static const char *holdHeldFunction = NULL;
static uint8_t holdHeldUnit = 0;
static int8_t holdHeldDp = -1;
static long holdHeldCount = 0;

// capture events for holdTick()
static volatile uint32_t holdCaptureEvents = 0;
static uint32_t holdCaptureHandled = 0;
static volatile uint32_t holdCaptureMs = 0;
static volatile uint32_t holdCaptures = 0;

// indicator
static esp_timer_handle_t holdTimer = NULL;
static uint16_t holdBlinkTicks = 0;
static bool holdBlinkOn = true;
static volatile bool holdPreviewOn = false; // manual HOLD preview while STO is held (HOLD lit, state still OFF)

// The displayed digits as a signed whole number of counts of the last digit;
// false for anything that is not a plain number (OL, self-test, blanks only).
static bool holdCounts(const char digits[KEITHLEY_DIGIT_COUNT], int8_t dpPos, uint16_t indicatorBits, long *out)
{
    if (dpPos < 0 || keithleyIsOverload(digits))
    {
        return false;
    }
    long value = 0;
    bool anyDigit = false;
    for (uint8_t i = 0; i < KEITHLEY_DIGIT_COUNT; i++)
    {
        char c = digits[i];
        if (c == ' ')
        {
            value = value * 10;
        }
        else if (c >= '0' && c <= '9')
        {
            value = value * 10 + (c - '0');
            anyDigit = true;
        }
        else
        {
            return false;
        }
    }
    if (!anyDigit)
    {
        return false;
    }
    *out = (indicatorBits & KEITHLEY_BIT_MINUS) ? -value : value;
    return true;
}

// "HOLD" / "AUTO" for the state (the blink phase for AUTO WAIT).
static void holdShowIndicator()
{
    uint8_t st = holdSt;
    uint8_t bits = 0;
    if (st == HOLD_STATE_MANUAL || (st == HOLD_STATE_OFF && holdPreviewOn))
    {
        bits = FB_HOLD_LIT;
    }
    else if (st == HOLD_STATE_AUTO_HELD)
    {
        bits = FB_HOLD_LIT | FB_HOLD_AUTO_LIT;
    }
    else if (st == HOLD_STATE_AUTO_WAIT)
    {
        bits = holdBlinkOn ? FB_HOLD_LIT : 0;
    }
    setHoldIndicator(bits);
}

void holdStart(uint8_t mode, uint32_t atMs, bool showTag)
{
    bool manual = (mode != SETTINGS_HOLD_AUTO);
    portENTER_CRITICAL(&holdMux);
    holdGeneration = holdGeneration + 1;
    holdSt = manual ? HOLD_STATE_MANUAL : HOLD_STATE_AUTO_WAIT;
    portEXIT_CRITICAL(&holdMux);
    holdBlinkTicks = 0;
    holdBlinkOn = true;
    holdPreviewOn = false; // a preview is taken over
    if (manual)
    {
        displayHoldAt(atMs);                      // what was shown at the first edge of the press
        markerAdd(atMs, MARKER_SOURCE_HOLD);      // ... and the same record in the log (silent)
        if (showTag)
        {
            markerShowTag();
        }
        debugLogf("HOLD: manual");
    }
    else
    {
        displayHoldRelease(); // live until the first stable reading
        debugLogf("HOLD: AUTO, waiting for a stable reading");
    }
    holdShowIndicator();
}

void holdStop()
{
    portENTER_CRITICAL(&holdMux);
    bool wasOn = (holdSt != HOLD_STATE_OFF);
    holdGeneration = holdGeneration + 1;
    holdSt = HOLD_STATE_OFF;
    portEXIT_CRITICAL(&holdMux);
    holdPreviewOn = false;
    displayHoldRelease();
    setHoldIndicator(0);
    if (wasOn)
    {
        debugLogf("HOLD: off");
    }
}

void holdPreview(uint32_t atMs)
{
    if (holdSt != HOLD_STATE_OFF)
    {
        return;
    }
    displayHoldAt(atMs);
    holdPreviewOn = true;
    holdShowIndicator();
}

void holdCancelPreview()
{
    if (!holdPreviewOn)
    {
        return;
    }
    holdPreviewOn = false;
    if (holdSt == HOLD_STATE_OFF)
    {
        displayHoldRelease();
    }
    holdShowIndicator();
}

uint8_t holdState()
{
    return holdSt;
}

uint32_t holdCaptureCount()
{
    return holdCaptures;
}

void holdUpdateReading(const char digits[KEITHLEY_DIGIT_COUNT], int8_t dpPos, uint16_t indicatorBits, uint8_t unit)
{
    uint8_t st = holdSt;
    if (st != HOLD_STATE_AUTO_WAIT && st != HOLD_STATE_AUTO_HELD)
    {
        return;
    }
    uint32_t gen = holdGeneration;
    if (gen != holdRunGeneration)
    {
        // started (again) / stopped since the last reading: nothing of the old run or held reading counts
        holdRunGeneration = gen;
        holdRunLen = 0;
        holdHeldValid = false;
    }
    const char *function = keithleyFunctionPrefix(unit, indicatorBits);

    // a change of the function drops the held reading: waiting again, live display
    if (st == HOLD_STATE_AUTO_HELD && holdHeldValid && function[0] != '\0' && function != holdHeldFunction)
    {
        bool changed = false;
        portENTER_CRITICAL(&holdMux);
        if (holdSt == HOLD_STATE_AUTO_HELD && holdGeneration == gen)
        {
            holdSt = HOLD_STATE_AUTO_WAIT;
            changed = true;
        }
        portEXIT_CRITICAL(&holdMux);
        if (changed)
        {
            holdHeldValid = false;
            displayHoldRelease();
        }
    }

    long count = 0;
    if (!holdCounts(digits, dpPos, indicatorBits, &count) || datalogLastReadingSettling())
    {
        holdRunLen = 0; // not a plain reading, or still settling: the run is broken
        return;
    }
    bool sameKind = (holdRunLen > 0 && function == holdRunFunction && unit == holdRunUnit && dpPos == holdRunDp);
    long newMin = (count < holdRunMin) ? count : holdRunMin;
    long newMax = (count > holdRunMax) ? count : holdRunMax;
    if (sameKind && newMax - newMin <= 2L * (long)CONFIG_HOLD_AUTO_TOLERANCE_COUNTS)
    {
        holdRunMin = newMin;
        holdRunMax = newMax;
        if (holdRunLen < 255)
        {
            holdRunLen++;
        }
    }
    else
    {
        // a new run starts with this reading
        holdRunFunction = function;
        holdRunUnit = unit;
        holdRunDp = dpPos;
        holdRunMin = count;
        holdRunMax = count;
        holdRunLen = 1;
        holdRunCaptured = false;
    }
    if (holdRunLen < CONFIG_HOLD_AUTO_READINGS || holdRunCaptured)
    {
        return;
    }
    long magnitude = (count < 0) ? -count : count;
    if (magnitude < (long)CONFIG_HOLD_AUTO_MIN_COUNTS)
    {
        return; // near zero (probes lifted off): never captured - a later reading of the run may still be
    }
    holdRunCaptured = true; // one run captures at most once
    long diff = count - holdHeldCount;
    if (diff < 0)
    {
        diff = -diff;
    }
    bool differs = !holdHeldValid || function != holdHeldFunction || unit != holdHeldUnit || dpPos != holdHeldDp ||
                   diff > (long)CONFIG_HOLD_AUTO_TOLERANCE_COUNTS;
    if (!differs)
    {
        return; // the same point again: kept, no new beep
    }
    bool captured = false;
    portENTER_CRITICAL(&holdMux);
    if ((holdSt == HOLD_STATE_AUTO_WAIT || holdSt == HOLD_STATE_AUTO_HELD) && holdGeneration == gen)
    {
        holdSt = HOLD_STATE_AUTO_HELD;
        holdCaptureMs = (uint32_t)millis();
        holdCaptureEvents = holdCaptureEvents + 1;
        captured = true;
    }
    portEXIT_CRITICAL(&holdMux);
    if (!captured)
    {
        return;
    }
    displayHoldCurrent(); // this frame (its display setters ran before this call)
    if (holdSt != HOLD_STATE_AUTO_HELD)
    {
        displayHoldRelease(); // stopped in the meantime
        return;
    }
    holdHeldValid = true;
    holdHeldFunction = function;
    holdHeldUnit = unit;
    holdHeldDp = dpPos;
    holdHeldCount = count;
}

void holdTick()
{
    uint32_t events = holdCaptureEvents;
    if (events != holdCaptureHandled)
    {
        holdCaptureHandled = events;
        holdCaptures = holdCaptures + 1;
        markerAdd(holdCaptureMs, MARKER_SOURCE_HOLD); // the captured reading marked in the log ...
        markerShowTag();                              // ... and TAG lit
        beeperBeepAsync(CONFIG_HOLD_CAPTURE_BEEP_MS);
        debugLogf("HOLD: AUTO captured a reading");
    }
    if (holdSt == HOLD_STATE_AUTO_WAIT)
    {
        holdBlinkTicks++;
        if ((uint32_t)holdBlinkTicks * (uint32_t)CONFIG_HOLD_TICK_MS >= (uint32_t)CONFIG_HOLD_BLINK_MS)
        {
            holdBlinkTicks = 0;
            holdBlinkOn = !holdBlinkOn;
        }
    }
    else
    {
        holdBlinkTicks = 0;
        holdBlinkOn = true;
    }
    holdShowIndicator();
}

static void holdTimerCallback(void *arg)
{
    (void)arg;
    holdTick();
}

void holdBegin()
{
    esp_timer_create_args_t timerArgs = {};
    timerArgs.callback = &holdTimerCallback;
    timerArgs.arg = NULL;
    timerArgs.dispatch_method = ESP_TIMER_TASK;
    timerArgs.name = "hold";
    if (esp_timer_create(&timerArgs, &holdTimer) == 0)
    {
        esp_timer_start_periodic(holdTimer, (uint64_t)CONFIG_HOLD_TICK_MS * 1000ULL);
    }
    setHoldIndicator(0);
}
