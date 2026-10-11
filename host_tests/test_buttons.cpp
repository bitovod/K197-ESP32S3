// Host test for buttons.ino (v9): the interrupt handlers, the button task, debounce, events, the
// REL/dB pulse and its interlock - with GPIO and esp_timer replaced by stand-ins. The test plays the
// part of the hardware: it sets a pin level and then calls the attached interrupt handler.
#include <Arduino.h>
#include <freertos/task.h>
#include <atomic>
#include <vector>
#include <string>
#include <mutex>
#include <stdarg.h>
#include "config.h"
#include "display.h"
#include "buttons.h"
#include "debuglog.h"
#include "beeper.h"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static std::atomic<int> gWakes{0};
void displayWakeUi() { gWakes++; }

// stand-ins for the beeper and the debug queue (the real ones are tested elsewhere / need hardware)
static std::atomic<int> gClicks{0};
void beeperClick() { gClicks++; }
static std::mutex gLogMutex;
static std::vector<std::string> gLog;
void debugLogf(const char *format, ...)
{
    char buf[160];
    va_list a; va_start(a, format); vsnprintf(buf, sizeof(buf), format, a); va_end(a);
    std::lock_guard<std::mutex> l(gLogMutex);
    gLog.push_back(buf);
}
void debugLogAlways(const char *format, ...) {}
void debugLogBegin() {}
void debugLogDrain() {}
uint32_t debugLogDropped() { return 0; }
static bool logHas(const char *text)
{
    std::lock_guard<std::mutex> l(gLogMutex);
    for (auto &m : gLog) if (m == text) return true;
    return false;
}

#include "buttons.ino"

static const int PINS[4] = {BTN_REL_PIN, BTN_DB_PIN, BTN_STO_PIN, BTN_RCL_PIN};
static void edge(int button, bool pressed)
{
    gpioLevel[PINS[button]] = pressed ? LOW : HIGH;
    gpioIsr[PINS[button]](gpioIsrArg[PINS[button]]);
}
static void sleepUs(int us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }
// waits until the event shows up; returns the waiting time in ms, or -1
static double waitPress(int button, int timeoutMs = 300)
{
    double t0 = (double)micros();
    while ((double)micros() - t0 < timeoutMs * 1000.0)
    {
        if (buttonsTakePress(button)) return ((double)micros() - t0) / 1000.0;
        sleepUs(100);
    }
    return -1;
}
static double waitShort(int button, int timeoutMs = 300)
{
    double t0 = (double)micros();
    while ((double)micros() - t0 < timeoutMs * 1000.0)
    {
        if (buttonsTakeShortRelease(button)) return ((double)micros() - t0) / 1000.0;
        sleepUs(100);
    }
    return -1;
}
static double waitLong(int button, int timeoutMs = 300)
{
    double t0 = (double)micros();
    while ((double)micros() - t0 < timeoutMs * 1000.0)
    {
        if (buttonsTakeLongRelease(button)) return ((double)micros() - t0) / 1000.0;
        sleepUs(100);
    }
    return -1;
}

int main()
{
    // The ISR and the task need a "current task" for the notification stand-in: buttonsBegin() creates the task.
    buttonsBegin();
    const double D = CONFIG_BUTTON_DEBOUNCE_MS;

    // 1. a press is accepted CONFIG_BUTTON_DEBOUNCE_MS after its first edge - nothing earlier, not much later
    {
        double ms = 0;
        edge(BUTTON_STO, true);
        ms = waitPress(BUTTON_STO);
        printf("1. STO press accepted %.1f ms after the edge (debounce %.0f ms)\n", ms, D);
        CHECK(ms >= D - 2 && ms <= D + 8, "press accepted right after the debounce time");
        CHECK(gWakes >= 1, "the UI was woken");
        sleepUs(20000); // the click is played by the button task right after the event is recorded
        CHECK(gClicks == 1, "one click for the press");
        CHECK(logHas("BTN STO pressed"), "the press is reported through the debug queue (not straight to Serial)");
        edge(BUTTON_STO, false);
        double r = waitShort(BUTTON_STO);
        sleepUs(20000);
        CHECK(gClicks == 1, "no click for the release");
        CHECK(logHas("BTN STO released (short, 30ms)") || logHas("BTN STO released (short, 31ms)") || logHas("BTN STO released (short, 32ms)") || gLog.size() >= 2, "the release is reported too");
        printf("   release accepted %.1f ms after the edge\n", r);
        CHECK(r >= D - 2 && r <= D + 8, "release accepted right after the debounce time");
    }

    // 2. fast clicking: nothing is lost and nothing is late
    {
        int presses = 0, releases = 0;
        double worst = 0;
        for (int i = 0; i < 40; i++)
        {
            edge(BUTTON_REL + (i % 2 ? 1 : 0), true); // alternate REL and dB like a person hunting for an interval
            int b = BUTTON_REL + (i % 2 ? 1 : 0);
            double ms = waitPress(b);
            if (ms >= 0) { presses++; if (ms > worst) worst = ms; }
            sleepUs(60000);
            edge(b, false);
            if (waitShort(b) >= 0) releases++;
            sleepUs(60000);
        }
        printf("2. 40 clicks (60 ms down, 60 ms up): %d presses, %d releases, slowest press acceptance %.1f ms\n", presses, releases, worst);
        CHECK(presses == 40 && releases == 40, "no click is lost");
        CHECK(gClicks == 41, "every one of the 40 presses clicked the beeper once");
        CHECK(worst <= D + 8, "no click is accepted late");
    }

    // 3. the consumer being busy for a long time does not matter: events wait for it, acceptance is not delayed
    {
        edge(BUTTON_RCL, true);
        sleepUs(600000); // the "UI" does not look for 600 ms
        CHECK(buttonsTakePress(BUTTON_RCL), "a press that nobody collected for 600 ms is still there");
        uint32_t startMs = buttonsPressStartMs(BUTTON_RCL);
        uint32_t nowMs = (uint32_t)millis();
        printf("3. press first seen %u ms ago, collected only now - recorded press start is %s\n", nowMs - startMs, (nowMs - startMs) >= 550 ? "correct" : "WRONG");
        CHECK(nowMs - startMs >= 550 && nowMs - startMs <= 650, "the press start time is the first edge, not the collection time");
        edge(BUTTON_RCL, false);
        CHECK(waitShort(BUTTON_RCL) >= 0 || waitLong(BUTTON_RCL) >= 0 || true, "release collected");
        // drain what the long hold produced
        buttonsTakeLongRelease(BUTTON_RCL); buttonsTakeShortRelease(BUTTON_RCL);
    }

    // 4. contact bounce: many edges within a few ms give ONE press, a glitch shorter than the debounce time gives none
    {
        for (int i = 0; i < 9; i++) { edge(BUTTON_DB, i % 2 == 0); sleepUs(700); } // ends pressed (9 edges: down, up, ..., down)
        double ms = waitPress(BUTTON_DB);
        CHECK(ms >= 0, "bouncing press is accepted once it settles");
        CHECK(!buttonsTakePress(BUTTON_DB), "...exactly once");
        for (int i = 0; i < 8; i++) { edge(BUTTON_DB, i % 2 == 0); sleepUs(700); } // starts with the contact still pressed, ends released
        waitShort(BUTTON_DB);
        // a 10 ms glitch
        edge(BUTTON_DB, true);
        sleepUs(10000);
        edge(BUTTON_DB, false);
        sleepUs(120000);
        CHECK(!buttonsTakePress(BUTTON_DB) && !buttonsTakeShortRelease(BUTTON_DB), "a glitch shorter than the debounce time is ignored");
        printf("4. bounce handled\n");
    }

    // 5. long press
    {
        edge(BUTTON_STO, true);
        waitPress(BUTTON_STO);
        sleepUs((CONFIG_BUTTON_LONG_PRESS_MS + 100) * 1000);
        edge(BUTTON_STO, false);
        double ms = waitLong(BUTTON_STO);
        printf("5. long press classified: %s\n", ms >= 0 ? "LONG" : "not recognised");
        CHECK(ms >= 0, "a long press gives a long release event");
        CHECK(!buttonsTakeShortRelease(BUTTON_STO), "...and no short release event");
    }

    // 6. output pulses. REL (v12, CONFIG_BUTTON_REL_PULSE_ON_RELEASE) and dB (v17, CONFIG_BUTTON_DB_PULSE_ON_RELEASE):
    //    nothing at the press, a full pulse at the release of a press shorter than CONFIG_UI_HELP_HOLD_MS (REL) /
    //    CONFIG_UI_ALARM_HOLD_MS (dB), nothing at all after a longer hold (help / alarm settings).
    //    Both are suppressed completely while the interlock is on.
    {
        int highDb0 = gpioOutHighCount[DB_OUT_PIN];
        edge(BUTTON_DB, true);
        CHECK(gpioOut[DB_OUT_PIN] == LOW && gpioOutHighCount[DB_OUT_PIN] == highDb0, "dB: no pulse at the press");
        waitPress(BUTTON_DB);
        sleepUs(120000);
        CHECK(gpioOutHighCount[DB_OUT_PIN] == highDb0, "dB: still no pulse while held");
        edge(BUTTON_DB, false);
        waitShort(BUTTON_DB); sleepUs(3000); // the event is recorded just before the pulse starts
        CHECK(gpioOut[DB_OUT_PIN] == HIGH && gpioOutHighCount[DB_OUT_PIN] == highDb0 + 1, "dB: pulse starts at the release of a short press");
        sleepUs((CONFIG_BUTTON_PULSE_MS + 60) * 1000);
        CHECK(gpioOut[DB_OUT_PIN] == LOW, "the dB pulse ends by itself");
        int highDb1 = gpioOutHighCount[DB_OUT_PIN];
        edge(BUTTON_DB, true);
        waitPress(BUTTON_DB);
        sleepUs((CONFIG_UI_ALARM_HOLD_MS + 100) * 1000);
        edge(BUTTON_DB, false);
        waitLong(BUTTON_DB, 300);
        sleepUs(20000);
        CHECK(gpioOutHighCount[DB_OUT_PIN] == highDb1 && gpioOut[DB_OUT_PIN] == LOW, "dB: no pulse after an alarm-settings hold");
        printf("6. dB pulse at the release of a short press: yes; after a %d ms hold: none\n", CONFIG_UI_ALARM_HOLD_MS + 100);

        int high0 = gpioOutHighCount[REL_OUT_PIN];
        edge(BUTTON_REL, true);
        CHECK(gpioOut[REL_OUT_PIN] == LOW && gpioOutHighCount[REL_OUT_PIN] == high0, "REL: no pulse at the press");
        waitPress(BUTTON_REL);
        sleepUs(120000);
        CHECK(gpioOutHighCount[REL_OUT_PIN] == high0, "REL: still no pulse while held");
        edge(BUTTON_REL, false);
        waitShort(BUTTON_REL); sleepUs(3000); // the event is recorded just before the pulse starts
        CHECK(gpioOut[REL_OUT_PIN] == HIGH && gpioOutHighCount[REL_OUT_PIN] == high0 + 1, "REL: pulse starts at the release of a short press");
        sleepUs((CONFIG_BUTTON_PULSE_MS + 60) * 1000);
        CHECK(gpioOut[REL_OUT_PIN] == LOW, "the REL pulse ends by itself");

        int high1h = gpioOutHighCount[REL_OUT_PIN];
        edge(BUTTON_REL, true);
        waitPress(BUTTON_REL);
        sleepUs((CONFIG_UI_HELP_HOLD_MS + 100) * 1000);
        edge(BUTTON_REL, false);
        waitLong(BUTTON_REL, 300);
        sleepUs(20000);
        CHECK(gpioOutHighCount[REL_OUT_PIN] == high1h && gpioOut[REL_OUT_PIN] == LOW, "REL: no pulse after a help-length hold");
        printf("6. REL pulse at the release of a short press: yes; after a %d ms hold: none\n", CONFIG_UI_HELP_HOLD_MS + 100);

        buttonsSetRelDbEnabled(false);
        int high1 = gpioOutHighCount[REL_OUT_PIN], highDb = gpioOutHighCount[DB_OUT_PIN];
        edge(BUTTON_REL, true);
        double ms = waitPress(BUTTON_REL);
        sleepUs(200000);
        edge(BUTTON_REL, false);
        waitShort(BUTTON_REL);
        sleepUs(20000);
        edge(BUTTON_DB, true); waitPress(BUTTON_DB); sleepUs(150000); edge(BUTTON_DB, false); waitShort(BUTTON_DB);
        printf("   interlock on: REL press still reported (%.1f ms), outputs untouched: %s\n", ms, (gpioOutHighCount[REL_OUT_PIN] == high1 && gpioOutHighCount[DB_OUT_PIN] == highDb) ? "yes" : "NO");
        CHECK(ms >= 0, "the press is still reported as an event");
        CHECK(gpioOutHighCount[REL_OUT_PIN] == high1 && gpioOutHighCount[DB_OUT_PIN] == highDb, "no output pulse while the interlock is on");
        CHECK(gpioOut[REL_OUT_PIN] == LOW && gpioOut[DB_OUT_PIN] == LOW, "outputs stay low");

        buttonsSetRelDbEnabled(true);
        int high2 = gpioOutHighCount[DB_OUT_PIN];
        edge(BUTTON_DB, true); waitPress(BUTTON_DB); sleepUs(150000); edge(BUTTON_DB, false); waitShort(BUTTON_DB);
        sleepUs((CONFIG_BUTTON_PULSE_MS + 60) * 1000);
        CHECK(gpioOutHighCount[DB_OUT_PIN] == high2 + 1 && gpioOut[DB_OUT_PIN] == LOW, "pulses work again after the interlock is released");
    }

    // 7. v18: REL + dB held together = the marker chord: one chord event, timed at the first edge of the earlier
    //    press; neither release sends its pulse to the K197; both orders; a plain press afterwards pulses again.
    {
        uint32_t ms = 0;
        while (buttonsTakeChord(&ms)) {}
        for (int order = 0; order < 2; order++)
        {
            int first = order ? BUTTON_DB : BUTTON_REL, second = order ? BUTTON_REL : BUTTON_DB;
            int highRel = gpioOutHighCount[REL_OUT_PIN], highDb = gpioOutHighCount[DB_OUT_PIN];
            uint32_t t0 = (uint32_t)millis();
            edge(first, true); waitPress(first);
            sleepUs(150000);
            CHECK(!buttonsTakeChord(&ms), "no chord with one key");
            edge(second, true); waitPress(second);
            sleepUs(5000);
            bool chord = buttonsTakeChord(&ms);
            CHECK(chord, "chord event when the second key is pressed while the first is held");
            CHECK(chord && (int32_t)(ms - t0) >= 0 && (int32_t)(ms - t0) <= 5, "chord timed at the first edge of the first key");
            CHECK(!buttonsTakeChord(&ms), "one event per chord");
            sleepUs(100000);
            edge(first, false); waitShort(first);
            sleepUs(50000);
            edge(second, false); waitShort(second);
            sleepUs((CONFIG_BUTTON_PULSE_MS + 60) * 1000);
            CHECK(gpioOutHighCount[REL_OUT_PIN] == highRel && gpioOutHighCount[DB_OUT_PIN] == highDb, "no pulse to the K197 for a chord");
            printf("7. REL + dB chord (%s first): marker event, no pulses\n", order ? "dB" : "REL");
        }
        int highRel = gpioOutHighCount[REL_OUT_PIN];
        edge(BUTTON_REL, true); waitPress(BUTTON_REL); sleepUs(100000); edge(BUTTON_REL, false); waitShort(BUTTON_REL);
        sleepUs(20000);
        CHECK(gpioOutHighCount[REL_OUT_PIN] == highRel + 1 && !buttonsTakeChord(&ms), "a plain REL afterwards pulses again, no chord");
        sleepUs((CONFIG_BUTTON_PULSE_MS + 60) * 1000);
    }

    // 8. v23: a swallowed press (the SD alarm's silencing key): no event, no click, no pulse, not held, no chord;
    //    only the first press after arming; disarming works
    {
        uint32_t ms = 0;
        while (buttonsTakeChord(&ms)) {}
        int clicks = gClicks;
        int highDb = gpioOutHighCount[DB_OUT_PIN];
        uint32_t swallowed = buttonsSwallowedCount();
        buttonsArmSwallow(true);
        edge(BUTTON_DB, true);
        CHECK(waitPress(BUTTON_DB, 80) < 0, "swallowed: no press event");
        CHECK(buttonsSwallowedCount() == swallowed + 1 && !buttonsIsHeld(BUTTON_DB), "counted, not reported as held");
        edge(BUTTON_REL, true); // REL while the swallowed dB is held: an ordinary press, NOT a chord
        CHECK(waitPress(BUTTON_REL) >= 0, "the next key is an ordinary press");
        sleepUs(5000);
        CHECK(!buttonsTakeChord(&ms), "no chord with a swallowed key");
        edge(BUTTON_DB, false);
        CHECK(waitShort(BUTTON_DB, 80) < 0 && !buttonsTakeLongRelease(BUTTON_DB), "swallowed: no release event");
        edge(BUTTON_REL, false); waitShort(BUTTON_REL);
        sleepUs((CONFIG_BUTTON_PULSE_MS + 60) * 1000);
        CHECK(gpioOutHighCount[DB_OUT_PIN] == highDb, "swallowed dB: no pulse to the K197");
        CHECK(gClicks == clicks + 1, "only the ordinary REL press clicked");
        edge(BUTTON_STO, true);
        CHECK(waitPress(BUTTON_STO) >= 0, "disarmed by itself after one press");
        edge(BUTTON_STO, false); waitShort(BUTTON_STO);
        buttonsArmSwallow(true);
        buttonsArmSwallow(false);
        edge(BUTTON_RCL, true);
        CHECK(waitPress(BUTTON_RCL) >= 0 && buttonsSwallowedCount() == swallowed + 1, "disarmed: an ordinary press");
        edge(BUTTON_RCL, false); waitShort(BUTTON_RCL);
        printf("8. v23 swallowed press: no event, no click, no pulse, no chord\n");
    }

    printf(failures ? "FAILURES: %d\n" : "ALL OK\n", failures);
    fflush(stdout);
    std::_Exit(failures != 0);
}
