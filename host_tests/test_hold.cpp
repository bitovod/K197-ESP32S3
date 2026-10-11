// Host test for hold.ino (v24 DATA HOLD): manual HOLD, AUTO HOLD (stability, tolerance, zero / OL, settling,
// recapture, function change), the indicator and the capture marker + beep - with the real reading_math.ino and
// stand-ins for the display, the marker, the beeper and the log.
#include <Arduino.h>
#include <freertos/task.h>
#include <math.h>
#include "config.h"
#include "display.h"
#include "framebuffer.h"
#include "settings.h"
#include "marker.h"
#include "hold.h"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

// ---- stand-ins ----
static bool gHeld = false;          // display frozen
static int gHoldAtCalls = 0, gHoldCurrentCalls = 0;
static uint32_t gHoldAtMs = 0;
static uint8_t gInd = 0xFF;
void displayHoldAt(uint32_t atMs) { gHeld = true; gHoldAtCalls++; gHoldAtMs = atMs; }
void displayHoldCurrent() { gHeld = true; gHoldCurrentCalls++; }
void displayHoldRelease() { gHeld = false; }
void setHoldIndicator(uint8_t bits) { gInd = bits; }
static int gMarkers = 0;
static uint8_t gMarkerSource = 0xFF;
static uint32_t gMarkerAt = 0;
bool markerAdd(uint32_t atMs, uint8_t source) { gMarkers++; gMarkerSource = source; gMarkerAt = atMs; return true; }
static int gTagShows = 0;
void markerShowTag() { gTagShows++; }
static int gBeeps = 0;
static uint16_t gBeepMs = 0;
void beeperBeepAsync(uint16_t ms) { gBeeps++; gBeepMs = ms; }
static bool gSettling = false;
bool datalogLastReadingSettling() { return gSettling; }
void debugLogf(const char *, ...) {}

#include "reading_math.ino"
#include "hold.ino"

// one reading: 6 characters of digits, decimal point position, unit, minus
static void reading(const char *d6, uint8_t unit = KEITHLEY_UNIT_V, int8_t dp = 1, bool minus = false, uint16_t extra = 0)
{
    fakeMillisOffset() += 300;
    char d[KEITHLEY_DIGIT_COUNT];
    memcpy(d, d6, KEITHLEY_DIGIT_COUNT);
    holdUpdateReading(d, dp, (uint16_t)((minus ? KEITHLEY_BIT_MINUS : 0) | extra), unit);
}
static void readings(int n, const char *d6, uint8_t unit = KEITHLEY_UNIT_V, int8_t dp = 1)
{
    for (int i = 0; i < n; i++)
        reading(d6, unit, dp);
}

int main()
{
    const int N = CONFIG_HOLD_AUTO_READINGS;

    // 1. manual HOLD: the reading shown at the press, a marker on it, HOLD lit; stop = live, dark
    {
        holdStart(SETTINGS_HOLD_MANUAL, 12345, true);
        CHECK(holdState() == HOLD_STATE_MANUAL && gHeld && gHoldAtMs == 12345, "manual: frozen at the press");
        CHECK(gMarkers == 1 && gMarkerSource == MARKER_SOURCE_HOLD && gMarkerAt == 12345 && gTagShows == 1, "manual: marker on the same moment, TAG (SCPI: at once)");
        CHECK(gInd == FB_HOLD_LIT, "manual: HOLD lit");
        readings(10, " 12345");
        CHECK(gHoldCurrentCalls == 0 && holdState() == HOLD_STATE_MANUAL, "manual: readings change nothing");
        holdTick();
        CHECK(gInd == FB_HOLD_LIT && gBeeps == 0, "manual: tick keeps HOLD lit, no beep");
        holdStop();
        CHECK(holdState() == HOLD_STATE_OFF && !gHeld && gInd == 0, "stop: live, dark");
        // the preview at the press: frozen + HOLD lit, no marker; a click undoes it; a hold takes it over
        int markers = gMarkers;
        holdPreview(500);
        CHECK(holdState() == HOLD_STATE_OFF && gHeld && gHoldAtMs == 500 && gInd == FB_HOLD_LIT && gMarkers == markers, "preview: frozen, HOLD lit, no marker");
        holdTick();
        CHECK(gInd == FB_HOLD_LIT, "the tick keeps the preview lit");
        holdCancelPreview();
        CHECK(!gHeld && gInd == 0, "click: preview undone");
        holdPreview(600);
        holdStart(SETTINGS_HOLD_MANUAL, 600, true);
        holdTick();
        CHECK(holdState() == HOLD_STATE_MANUAL && gHeld && gInd == FB_HOLD_LIT && gMarkers == markers + 1 && gTagShows == 2,
              "hold: preview taken over, marked, TAG");
        holdCancelPreview();
        CHECK(gHeld, "a stale cancel does not release a confirmed HOLD");
        holdPreview(700);
        CHECK(gHoldAtMs == 600, "no preview while HOLD is on");
        holdStop();
        printf("1. manual HOLD (+ preview) ok\n");
    }

    // 2. AUTO HOLD: waits (live, HOLD blinks), captures the first stable reading (HOLD + AUTO, beep, marker)
    {
        int markers = gMarkers;
        holdStart(SETTINGS_HOLD_AUTO, 0, false);
        CHECK(holdState() == HOLD_STATE_AUTO_WAIT && !gHeld && gInd == FB_HOLD_LIT, "AUTO: waiting, live, HOLD on");
        int toggles = 0;
        uint8_t last = gInd;
        for (int i = 0; i < 2 * CONFIG_HOLD_BLINK_MS / CONFIG_HOLD_TICK_MS; i++)
        {
            holdTick();
            if (gInd != last) toggles++;
            last = gInd;
        }
        CHECK(toggles == 2 && (gInd & FB_HOLD_AUTO_LIT) == 0, "AUTO waiting: HOLD blinks, AUTO dark");
        readings(N - 1, " 12345");
        CHECK(holdState() == HOLD_STATE_AUTO_WAIT && gHoldCurrentCalls == 0, "not stable before N readings");
        reading(" 12346"); // within the tolerance
        CHECK(holdState() == HOLD_STATE_AUTO_HELD && gHeld && gHoldCurrentCalls == 1, "N stable readings: captured");
        holdTick();
        CHECK(gInd == (FB_HOLD_LIT | FB_HOLD_AUTO_LIT) && gBeeps == 1 && gBeepMs == CONFIG_HOLD_CAPTURE_BEEP_MS &&
              gMarkers == markers + 1 && gMarkerSource == MARKER_SOURCE_HOLD && gTagShows == 3, "captured: HOLD + AUTO, beep, marker, TAG");
        readings(20, " 12345");
        holdTick();
        CHECK(gHoldCurrentCalls == 1 && gBeeps == 1, "the same point: no recapture");
        printf("2. AUTO HOLD: blinking while waiting, first capture ok\n");
    }

    // 3. a new stable value is captured; noise, zero, OL and settling are not
    {
        readings(N, " 23456");
        holdTick();
        CHECK(gHoldCurrentCalls == 2 && gBeeps == 2, "a new stable value: captured");
        for (int i = 0; i < 12; i++) reading((i % 2) ? " 30000" : " 30010"); // jumps of 10 counts: never stable
        CHECK(gHoldCurrentCalls == 2, "noise above the tolerance: nothing");
        readings(10, " 00050");
        CHECK(gHoldCurrentCalls == 2 && holdState() == HOLD_STATE_AUTO_HELD, "probes lifted (50 counts): not captured, held stays");
        readings(10, "  OL  ");
        CHECK(gHoldCurrentCalls == 2, "OL: not captured");
        readings(10, " 00099");
        CHECK(gHoldCurrentCalls == 2, "99 counts: below CONFIG_HOLD_AUTO_MIN_COUNTS");
        reading(" 00100");
        reading(" 00100");
        reading(" 00101");
        reading(" 00100");
        CHECK(gHoldCurrentCalls == 3, "100 counts: captured");
        // settling breaks the run
        readings(N - 1, " 45678");
        gSettling = true;
        reading(" 45678");
        gSettling = false;
        CHECK(gHoldCurrentCalls == 3, "a settling reading breaks the run");
        readings(N - 1, " 45678");
        CHECK(gHoldCurrentCalls == 3, "... N more are needed");
        reading(" 45678");
        CHECK(gHoldCurrentCalls == 4, "... and then it is captured");
        // negative, other range (decimal point) of the same function = different
        readings(N, " 45678", KEITHLEY_UNIT_V, 2);
        CHECK(gHoldCurrentCalls == 5, "other decimal point (range): captured");
        for (int i = 0; i < N; i++) reading(" 45678", KEITHLEY_UNIT_V, 2, true);
        CHECK(gHoldCurrentCalls == 6, "negative value: captured");
        holdTick();
        printf("3. recapture, noise, zero, OL, threshold, settling, range, sign ok\n");
    }

    // 4. a change of the function: the held reading is dropped, waiting again (live); then captured in OHM
    {
        int captures = gHoldCurrentCalls;
        reading(" 12345", KEITHLEY_UNIT_KOHM);
        CHECK(holdState() == HOLD_STATE_AUTO_WAIT && !gHeld, "function change: waiting again, live");
        holdTick();
        CHECK((gInd & FB_HOLD_AUTO_LIT) == 0, "AUTO dark again");
        readings(N - 1, " 12345", KEITHLEY_UNIT_KOHM);
        CHECK(gHoldCurrentCalls == captures + 1 && holdState() == HOLD_STATE_AUTO_HELD, "OHM captured after N readings");
        // a range change within OHM is not a function change
        reading(" 12345", KEITHLEY_UNIT_MOHM);
        CHECK(holdState() == HOLD_STATE_AUTO_HELD && gHeld, "kOhm -> MOhm: same function, held stays");
        // AC on the same unit is another function (DCV -> ACV)
        readings(N, " 12345");
        holdTick();
        CHECK(holdState() == HOLD_STATE_AUTO_HELD, "DCV captured");
        reading(" 12345", KEITHLEY_UNIT_V, 1, false, KEITHLEY_BIT_AC);
        CHECK(holdState() == HOLD_STATE_AUTO_WAIT, "DCV -> ACV: waiting again");
        printf("4. function change ok\n");
    }

    // 5. stop and restart: nothing of the old run counts; the same value is captured again after a restart
    {
        holdStop();
        int captures = gHoldCurrentCalls;
        readings(N, " 77777");
        CHECK(gHoldCurrentCalls == captures && holdState() == HOLD_STATE_OFF, "off: readings ignored");
        holdStart(SETTINGS_HOLD_AUTO, 0, false);
        reading(" 77777");
        CHECK(holdState() == HOLD_STATE_AUTO_WAIT, "restart: the old run does not count");
        readings(N - 1, " 77777");
        CHECK(gHoldCurrentCalls == captures + 1, "restart: captured after N new readings");
        holdStart(SETTINGS_HOLD_AUTO, 0, false); // STO held again: starts over
        readings(N, " 77777");
        CHECK(gHoldCurrentCalls == captures + 2, "started over: the same value captured again");
        holdStart(SETTINGS_HOLD_MANUAL, 999, false);
        CHECK(holdState() == HOLD_STATE_MANUAL && gHoldAtMs == 999, "from AUTO to manual");
        readings(N, " 88888");
        CHECK(gHoldCurrentCalls == captures + 2, "manual: no AUTO capture");
        holdStop();
        printf("5. stop / restart ok (%u captures in all)\n", (unsigned)holdCaptureCount());
    }

    printf(failures ? "FAILURES: %d\n" : "ALL OK\n", failures);
    return failures ? 1 : 0;
}
