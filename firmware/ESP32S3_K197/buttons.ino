// buttons.ino
// See buttons.h.

#include "buttons.h"
#include "config.h"
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "display.h" // displayWakeUi()
#include "debuglog.h"
#include "beeper.h"

// One button's full state. Fields the interrupt handler (buttonISR())
// writes are `volatile`; the ones it never touches (pin/outputPin/
// name/pulseTimer, all set once in buttonsBegin() and read-only after
// that) aren't.
//
// Debounce here is "candidate, then confirm after a quiet period",
// not the simpler "ignore any edge within X ms of the last accepted
// one" this file started with - that simpler version trusted whatever
// digitalRead() happened to return at the single instant an edge
// escaped its timing window, with no way to notice or correct a
// misread if that instant landed mid-bounce. On real hardware that
// occasionally left `confirmedPressed` permanently out of sync with
// the actual switch (a short click silently merging into the next
// one, reported as one long press - the author's own real-hardware
// testing caught this). The fix: any edge is only a CANDIDATE new
// state; the button task only accepts it once
// CONFIG_BUTTON_DEBOUNCE_MS has passed with the line still agreeing
// (re-checked with a fresh digitalRead(), not the original edge's
// reading), and any edge that returns to the already-confirmed state
// cancels a pending candidate outright, rather than just resetting a
// timer that trusted it anyway.
struct ButtonState
{
    uint8_t pin;
    uint8_t outputPin; // 0 = this button drives no REL/dB-style pulse
    const char *name;
    esp_timer_handle_t pulseTimer; // unused/NULL when outputPin == 0

    volatile bool confirmedPressed;
    volatile uint32_t pressStartMs; // set when a press is CONFIRMED - see candidateFirstMs below for why that's not the same moment the pulse actually started

    volatile bool candidatePending;
    volatile bool candidatePressed;   // direction of the pending candidate
    volatile uint32_t candidateFirstMs; // when this candidate direction was first seen - used for pulse-timing/duration accuracy, since confirmation happens later
    volatile uint32_t candidateSinceMs; // when it was LAST seen - the button task waits for this to be CONFIG_BUTTON_DEBOUNCE_MS in the past

    // Whether the REL/dB pulse has already been optimistically started
    // for the current not-yet-confirmed press candidate (so a run of
    // same-direction bounce edges before confirmation doesn't restart
    // it repeatedly). Reset once the press is confirmed (no further
    // restarts possible) or the candidate is cancelled (see
    // buttonISR()'s cancellation branch, which also turns the output
    // back off in that case - a candidate that never confirms must not
    // leave the pin stuck HIGH with no pulse-end timer ever armed).
    volatile bool pulseKicked;

    // v12: true for a button whose output pulse is sent at the RELEASE of
    // a short press instead of at the first edge (REL, see
    // CONFIG_BUTTON_REL_PULSE_ON_RELEASE): a hold of CONFIG_UI_HELP_HOLD_MS
    // opens the help screen instead, and a REL already sent to the K197
    // could not be taken back. Set once, read-only afterwards.
    bool pulseOnRelease;
    // For such a button: whether the outputs were enabled when the press
    // was confirmed (the release only sends the pulse if they were then
    // AND still are).
    volatile bool pulseArmed;
    // v17: for such a button, the longest press that still sends the pulse at
    // the release (REL: CONFIG_UI_HELP_HOLD_MS, the help hold; dB:
    // CONFIG_UI_ALARM_HOLD_MS, the alarm settings hold).
    uint16_t releasePulseMaxMs;
    // v18: REL and dB were held together during this press (a marker chord):
    // its release sends no pulse. Button task only.
    bool chord;
    // v23: this press was swallowed (buttonsArmSwallow()): no events, no pulse,
    // not reported as held. Button task writes, buttonsIsHeld() reads.
    volatile bool swallowed;
};

static ButtonState relButton = {BTN_REL_PIN, REL_OUT_PIN, "REL", NULL, false, 0, false, false, 0, 0, false,
                                 CONFIG_BUTTON_REL_PULSE_ON_RELEASE != 0, false, CONFIG_UI_HELP_HOLD_MS, false, false};
static ButtonState dbButton = {BTN_DB_PIN, DB_OUT_PIN, "dB", NULL, false, 0, false, false, 0, 0, false,
                                CONFIG_BUTTON_DB_PULSE_ON_RELEASE != 0, false, CONFIG_UI_ALARM_HOLD_MS, false, false};
static ButtonState stoButton = {BTN_STO_PIN, 0, "STO", NULL, false, 0, false, false, 0, 0, false, false, false, 0, false, false};
static ButtonState rclButton = {BTN_RCL_PIN, 0, "RCL", NULL, false, 0, false, false, 0, 0, false, false, false, 0, false, false};

// The button task (buttonsTaskEntry() below) and the helper that wakes it
// from the interrupt handler.
static volatile TaskHandle_t buttonsTaskHandle = NULL;
static void buttonsTaskEntry(void *pvParameters); // defined below, after the debounce code it calls (buttonsBegin() needs it earlier)

static void wakeButtonsTaskFromIsr()
{
    TaskHandle_t h = buttonsTaskHandle;
    if (h != NULL)
    {
        BaseType_t woken = pdFALSE;
        vTaskNotifyGiveFromISR(h, &woken);
        // No yield here: portYIELD_FROM_ISR() has a different form in
        // different ESP-IDF versions. The task then runs at the next tick
        // (1 ms) at the latest, far below the debounce time.
    }
}

// UI event bitmasks, one bit per button (index = position in
// allButtons[] below, which matches the BUTTON_* constants in
// buttons.h). Written only by the button task (via
// confirmButtonIfSettled()), consumed by the buttonsTake...()
// functions at the bottom of this file - both from the same task,
// see buttons.h.
static volatile uint8_t pressEvents = 0;
static volatile uint32_t pressStartTimes[4] = {0, 0, 0, 0};
static volatile uint8_t shortReleaseEvents = 0;
static volatile uint8_t longReleaseEvents = 0;
// v18: REL + dB held together (buttonsTakeChord()) and the first edge of the
// press that started it.
static volatile bool chordEvent = false;
static volatile uint32_t chordStartMs = 0;

// Whether a REL/dB press may start the output pulse. Read from the
// GPIO interrupt handler, hence volatile.
static volatile bool relDbOutputsEnabled = true;

// v23: the next confirmed press is swallowed (buttonsArmSwallow()).
static volatile bool swallowArmed = false;
static volatile uint32_t swallowedPresses = 0;

// Iterated by buttonsBegin() and the button task - the
// interrupt handlers themselves are each wired to one specific
// button's struct directly (via attachInterruptArg()'s arg
// parameter), they never walk this array.
static ButtonState *allButtons[4] = {&relButton, &dbButton, &stoButton, &rclButton};

// esp_timer callback (runs in the esp_timer service task, NOT
// interrupt context - safe to touch here, unlike inside buttonISR()
// below) that ends one REL/dB button's output pulse. `arg` is the
// same ButtonState* the timer was armed with - deliberately typed
// `void*`, not `ButtonState*`, matching esp_timer_cb_t's own required
// signature: giving this a custom-struct-typed parameter is exactly
// what broke Arduino's automatic prototype generation the first time
// this file was written (the generated prototype landed before this
// file's struct definition, in the merged sketch, and couldn't
// resolve the type) - void* needs no such forward knowledge, so it
// can't hit that problem again.
static void pulseEndCallback(void *arg)
{
    ButtonState *btn = (ButtonState *)arg;
    digitalWrite(btn->outputPin, LOW);
}

// GPIO interrupt handler, shared by all four buttons via
// attachInterruptArg() - `arg` is that button's own ButtonState*, for
// the same void* reason pulseEndCallback() above explains. Runs in
// true interrupt context: kept as short as possible, and touches only
// digitalRead()/digitalWrite()/millis() (all standard practice to
// call from an ESP32 Arduino ISR) - explicitly does NOT call
// Serial.print() or any esp_timer_*() function, neither of which is
// safe here; both are deferred to the button task instead.
static void buttonISR(void *arg)
{
    ButtonState *btn = (ButtonState *)arg;
    uint32_t nowMs = millis();
    bool level = (digitalRead(btn->pin) == LOW); // active LOW, internal pull-up - true means physically pressed right now

    if (level == btn->confirmedPressed)
    {
        // The line is back at whatever we last confirmed - either
        // nothing is actually happening, or a pending candidate for
        // the OTHER direction just bounced back without ever being
        // confirmed. Cancel it.
        if (btn->candidatePending && btn->pulseKicked && !btn->confirmedPressed)
        {
            // That cancelled candidate was a press we'd already
            // optimistically started the pulse for (see below) - it
            // never got confirmed, so no pulse-end timer was ever
            // armed for it either. Undo the pulse right here, or it
            // would stay stuck HIGH forever.
            if (btn->outputPin)
                digitalWrite(btn->outputPin, LOW);
            btn->pulseKicked = false;
        }
        btn->candidatePending = false;
        return;
    }

    if (!btn->candidatePending || btn->candidatePressed != level)
    {
        // A fresh candidate direction - start (or restart, if it
        // flipped mid-bounce) its debounce window.
        btn->candidatePressed = level;
        btn->candidateFirstMs = nowMs;
        btn->candidateSinceMs = nowMs;
        btn->candidatePending = true;
        wakeButtonsTaskFromIsr(); // the button task now debounces this press/release

        if (level && btn->outputPin && !btn->pulseOnRelease && !btn->pulseKicked && relDbOutputsEnabled)
        {
            // Fastest possible response: start the REL/dB pulse the
            // instant we see the FIRST hint of a press, without
            // waiting for debounce to confirm it - deliberately
            // optimistic. The small risk this trades for that speed
            // (a pure noise blip that never confirms) is handled
            // above, in the cancellation branch.
            digitalWrite(btn->outputPin, HIGH);
            btn->pulseKicked = true;
        }
        // (v8: with relDbOutputsEnabled false nothing is kicked here, so
        // no pulse starts, and confirmButtonIfSettled() below does not
        // arm a pulse-end timer for it either.)
    }
    else
    {
        // Same candidate direction seen again before it was
        // confirmed - still bouncing that way; keep extending the
        // quiet-period requirement rather than confirming early.
        btn->candidateSinceMs = nowMs;
    }
}

// v16 (SYSTem:DIAGnostic?): the button task, for its stack high-water mark.
TaskHandle_t buttonsGetTask()
{
    return buttonsTaskHandle;
}

void buttonsBegin()
{
    // The task first, so the handle exists before the first interrupt.
    TaskHandle_t taskHandle = NULL;
    xTaskCreatePinnedToCore(buttonsTaskEntry, "buttons", CONFIG_BUTTONS_TASK_STACK_BYTES, NULL,
                             CONFIG_BUTTONS_TASK_PRIORITY, &taskHandle, 1);
    buttonsTaskHandle = taskHandle;

    for (uint8_t i = 0; i < 4; i++)
    {
        ButtonState *btn = allButtons[i];
        pinMode(btn->pin, INPUT_PULLUP);

        if (btn->outputPin)
        {
            pinMode(btn->outputPin, OUTPUT);
            digitalWrite(btn->outputPin, LOW);

            esp_timer_create_args_t timerArgs = {};
            timerArgs.callback = &pulseEndCallback;
            timerArgs.arg = btn;
            timerArgs.dispatch_method = ESP_TIMER_TASK;
            timerArgs.name = btn->name;
            esp_timer_create(&timerArgs, &btn->pulseTimer);
        }

        attachInterruptArg(digitalPinToInterrupt(btn->pin), &buttonISR, btn, CHANGE);
    }
}

// Confirms one button's pending candidate, if it's been quiet for
// CONFIG_BUTTON_DEBOUNCE_MS - re-checking the live pin level rather
// than trusting whatever buttonISR() read when the candidate started,
// which is the actual fix for the bug this replaced (see the struct's
// own comment above).
//
// Takes `void *arg`, not `ButtonState *`, even though this is never
// called from interrupt context and so doesn't NEED the void* calling
// convention the way buttonISR()/pulseEndCallback() above do - it's
// for the same Arduino auto-prototype reason those two explain: ANY
// function in this file whose signature names the ButtonState struct
// is vulnerable to the exact same "declared void" / "was not declared
// in this scope" compile error, regardless of whether it's an ISR, a
// timer callback, or (as this one turned out to be, the hard way,
// on the author's own real hardware) a perfectly ordinary static
// helper - the auto-generated prototype lands before this struct's
// definition either way. void* sidesteps it categorically, so every
// function here uses it now, not just the two that technically had to.
static uint8_t buttonIndexOf(void *arg)
{
    for (uint8_t i = 0; i < 4; i++)
    {
        if (allButtons[i] == (ButtonState *)arg)
            return i;
    }
    return 0;
}

static void confirmButtonIfSettled(void *arg, uint32_t nowMs)
{
    ButtonState *btn = (ButtonState *)arg;
    if (!btn->candidatePending)
        return;
    if ((uint32_t)(nowMs - btn->candidateSinceMs) < CONFIG_BUTTON_DEBOUNCE_MS)
        return;

    bool level = (digitalRead(btn->pin) == LOW);
    if (level != btn->candidatePressed)
        return; // changed again since the last edge we saw - leave it pending, a fresh interrupt (or the next call here) will catch up

    btn->candidatePending = false;
    btn->confirmedPressed = level;

    if (level)
    {
        btn->pressStartMs = btn->candidateFirstMs;
        if (swallowArmed)
        {
            // v23: this press only silences the SD card alarm - no event, no click, no pulse at the release.
            // A pulse already kicked at the first edge (a button without pulse-on-release) still gets its end.
            swallowArmed = false;
            btn->swallowed = true;
            btn->pulseArmed = false;
            if (btn->outputPin && btn->pulseKicked)
            {
                esp_timer_start_once(btn->pulseTimer, (uint64_t)CONFIG_BUTTON_PULSE_MS * 1000ULL);
            }
            swallowedPresses = swallowedPresses + 1;
            displayWakeUi();
            debugLogf("BTN %s pressed: swallowed (silences the SD alarm)", btn->name);
            return;
        }
        pressEvents = (uint8_t)(pressEvents | (1u << buttonIndexOf(arg)));
        pressStartTimes[buttonIndexOf(arg)] = btn->pressStartMs;
        displayWakeUi();

        if (btn->outputPin && btn->pulseKicked)
        {
            // The pulse itself already started (digitalWrite() in
            // buttonISR(), the instant the first candidate edge
            // arrived) - only arm the timer that ends it, sized for
            // however much of CONFIG_BUTTON_PULSE_MS is left counting
            // from that original start, not a fresh full duration
            // from just now, so a slow-to-confirm press still
            // produces a pulse of the configured length overall
            // rather than running long by the confirmation delay.
            uint32_t elapsedMs = nowMs - btn->candidateFirstMs;
            uint32_t remainingMs = (elapsedMs < CONFIG_BUTTON_PULSE_MS) ? (CONFIG_BUTTON_PULSE_MS - elapsedMs) : 1;
            esp_timer_start_once(btn->pulseTimer, (uint64_t)remainingMs * 1000ULL);
        }

        if (btn->pulseOnRelease)
        {
            btn->pulseArmed = relDbOutputsEnabled; // decided again at the release, see below
        }

        // v18: REL pressed while dB is held, or the other way round = the marker chord. Neither release
        // then sends its pulse; the UI gets one chord event, timed at the first edge of the EARLIER press.
        ButtonState *other = (btn == &relButton) ? &dbButton : ((btn == &dbButton) ? &relButton : NULL);
        if (other != NULL && other->confirmedPressed && !other->swallowed)
        {
            btn->chord = true;
            other->chord = true;
            chordStartMs = other->pressStartMs;
            chordEvent = true;
            debugLogf("BTN REL + dB together: marker");
        }

        // The click comes after the event is recorded and the UI woken, so
        // the 5 ms it takes never delays anything the person sees.
        beeperClick();
        debugLogf("BTN %s pressed", btn->name);
    }
    else
    {
        if (btn->swallowed)
        {
            // v23: the release of a swallowed press - nothing to report
            btn->swallowed = false;
            btn->pulseArmed = false;
            btn->pulseKicked = false;
            btn->chord = false;
            debugLogf("BTN %s released (swallowed press)", btn->name);
            return;
        }
        uint32_t durationMs = btn->candidateFirstMs - btn->pressStartMs;
        if (durationMs >= CONFIG_BUTTON_LONG_PRESS_MS)
            longReleaseEvents = (uint8_t)(longReleaseEvents | (1u << buttonIndexOf(arg)));
        else
            shortReleaseEvents = (uint8_t)(shortReleaseEvents | (1u << buttonIndexOf(arg)));
        displayWakeUi();
        debugLogf("BTN %s released (%s, %lums)", btn->name,
                  (durationMs >= CONFIG_BUTTON_LONG_PRESS_MS) ? "LONG" : "short", (unsigned long)durationMs);
        if (btn->pulseOnRelease && btn->outputPin && btn->pulseArmed && relDbOutputsEnabled && !btn->chord &&
            durationMs < (uint32_t)btn->releasePulseMaxMs)
        {
            // A short press of a release-pulse button (REL, v17 dB): only now is it
            // certain this was not the hold that opens the help screen (REL) or
            // the alarm settings (dB), so
            // the pulse towards the K197 starts here, full length. This runs
            // in the button task, where esp_timer_*() is allowed.
            digitalWrite(btn->outputPin, HIGH);
            esp_timer_start_once(btn->pulseTimer, (uint64_t)CONFIG_BUTTON_PULSE_MS * 1000ULL);
            debugLogf("BTN %s pulse sent at release", btn->name);
        }
        btn->pulseArmed = false;
        btn->pulseKicked = false; // ready for the next press cycle
        btn->chord = false;
    }
}

// True while any button has an edge that is still being debounced.
static bool anyCandidatePending()
{
    for (uint8_t i = 0; i < 4; i++)
    {
        if (allButtons[i]->candidatePending)
            return true;
    }
    return false;
}

// The button task. Sleeps until buttonISR() wakes it with the first edge
// of a press or release; from then on, while anything is still being
// debounced, it looks again every tick (1 ms) - so a press is confirmed
// CONFIG_BUTTON_DEBOUNCE_MS plus at most about a millisecond after its
// first edge, no matter what the UI or anything else is doing. When
// nothing is pending it sleeps without a timeout (no polling at all).
// A wake-up that arrives between the check and the wait is not lost: the
// notification stays pending and the wait returns at once.
static void buttonsTaskEntry(void *pvParameters)
{
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, anyCandidatePending() ? 1 : portMAX_DELAY);
        uint32_t nowMs = millis();
        for (uint8_t i = 0; i < 4; i++)
            confirmButtonIfSettled(allButtons[i], nowMs);
    }
}

// Consumes one bit of an event mask - true if it was set.
static bool takeEvent(volatile uint8_t *mask, uint8_t button)
{
    uint8_t bit = (uint8_t)(1u << button);
    if (*mask & bit)
    {
        *mask = (uint8_t)(*mask & ~bit);
        return true;
    }
    return false;
}

bool buttonsTakePress(uint8_t button)
{
    return takeEvent(&pressEvents, button);
}

bool buttonsTakeShortRelease(uint8_t button)
{
    return takeEvent(&shortReleaseEvents, button);
}

bool buttonsTakeLongRelease(uint8_t button)
{
    return takeEvent(&longReleaseEvents, button);
}

bool buttonsTakeChord(uint32_t *firstEdgeMs)
{
    if (!chordEvent)
    {
        return false;
    }
    chordEvent = false;
    *firstEdgeMs = chordStartMs;
    return true;
}

void buttonsSetRelDbEnabled(bool enabled)
{
    relDbOutputsEnabled = enabled;
}

uint32_t buttonsPressStartMs(uint8_t button)
{
    return pressStartTimes[button];
}

bool buttonsIsHeld(uint8_t button)
{
    if (button >= 4)
    {
        return false;
    }
    return allButtons[button]->confirmedPressed && !allButtons[button]->swallowed;
}

void buttonsArmSwallow(bool armed)
{
    swallowArmed = armed;
}

uint32_t buttonsSwallowedCount()
{
    return swallowedPresses;
}
