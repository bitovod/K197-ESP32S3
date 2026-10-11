// buttons.h
// Front-panel button reading (config.h's BTN_REL_PIN/BTN_DB_PIN/
// BTN_STO_PIN/BTN_RCL_PIN) and REL/dB rear-panel relay emulation
// (REL_OUT_PIN/DB_OUT_PIN) - see config.h for the pin assignments and
// docs/DEVELOPMENT.md for the PC817 optocoupler circuit those outputs
// are meant to drive.
//
// Interrupt-driven, not polled: each button has a GPIO interrupt
// (attachInterruptArg(), CHANGE mode) that reacts the instant a
// transition happens - not on the next pass of some loop. REL/dB's
// output pulse starts right there, at interrupt speed, the moment a
// press is first seen. The press/release EVENT itself (the debug
// report, and the SHORT/LONG classification - compared against
// CONFIG_BUTTON_LONG_PRESS_MS) is only finalized once
// CONFIG_BUTTON_DEBOUNCE_MS has passed with the line settled that
// way, confirmed by the button task - see buttons.ino
// for why that two-stage design (react instantly, confirm shortly
// after) is what it takes to be both fast AND correct against switch
// bounce. All four buttons report every confirmed press/release to
// the debug console. STO/RCL are read and reported the same way as
// REL/dB, but drive no output - their K197 UI behavior is still
// unscoped, see docs/DEVELOPMENT.md.
//
// NOT VERIFIED against a real build in this environment (no
// toolchain available) - attachInterruptArg() and one-shot esp_timer
// use (for ending the REL/dB pulse) are both standard ESP32 Arduino
// core / ESP-IDF facilities, but neither has been exercised in this
// project before now.

#ifndef BUTTONS_H
#define BUTTONS_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h> // TaskHandle_t (v16)

// Call once from setup(). Sets up all four button inputs (INPUT_PULLUP),
// both REL/dB output pins (OUTPUT, idle LOW), the one-shot pulse-end
// timers for each, creates the button task and attaches all four GPIO
// interrupts.
void buttonsBegin();

// Debounce confirmation and everything that is not interrupt safe
// (debug prints, arming the REL/dB pulse-end timer, recording UI events)
// runs in a small dedicated task, created by buttonsBegin() (new in v9;
// earlier versions polled buttonsProcessEvents() from a loop, which made
// the reaction time depend on how busy that loop was). The task sleeps
// until a button interrupt wakes it (vTaskNotifyGiveFromISR()), and while
// a press/release is still being debounced it looks once per tick (1 ms),
// so a press is confirmed CONFIG_BUTTON_DEBOUNCE_MS (+ about 1 ms) after
// its first edge, whatever the rest of the firmware is doing. Its
// priority (CONFIG_BUTTONS_TASK_PRIORITY) is the highest of all tasks on
// its core. Nothing needs to call it.

// ---------------------------------------------------------------
// Button events for the user interface (ui.ino) - added in v8
// ---------------------------------------------------------------
// The button task confirms presses/releases after the debounce
// period (see buttons.ino); besides the debug message it now also
// records each confirmed event in a small bitmask, one bit per button,
// that the UI picks up with the buttonsTake...() functions below. Each
// returns true once per event and consumes it. They are meant to be
// called from ONE task (the display render/UI task, see display.ino), which
// is why a plain bitmask without locking is enough - do not call them from
// a second task without adding it. Every recorded event also wakes that
// task at once (displayWakeUi(), display.h).
//
// Press events fire when the press is CONFIRMED (the moment the
// debounce window has passed with the line still low), not on
// release - so a button that should react immediately (STO's quick
// statistics view) feels instant. The short/long release events are
// there for the later menu (long press = confirm); the threshold is
// CONFIG_BUTTON_LONG_PRESS_MS.

#define BUTTON_REL 0
#define BUTTON_DB  1
#define BUTTON_STO 2
#define BUTTON_RCL 3

bool buttonsTakePress(uint8_t button);
bool buttonsTakeShortRelease(uint8_t button);
bool buttonsTakeLongRelease(uint8_t button);

// millis() at the moment the most recently confirmed press of the button
// was FIRST seen (the first edge, before debouncing) - used to measure how
// long a press waited before the UI handled it.
uint32_t buttonsPressStartMs(uint8_t button);

// True while the button is held down (the debounced state the button task
// last confirmed). Together with buttonsPressStartMs() this lets the UI act
// on a hold of a given length while the button is still down (RCL held on
// the statistics screen clears the log). Safe from any task.
bool buttonsIsHeld(uint8_t button);

// REL (v12, CONFIG_BUTTON_REL_PULSE_ON_RELEASE): its pulse towards the K197
// is sent at the RELEASE of a press shorter than CONFIG_UI_HELP_HOLD_MS,
// not at the first edge - a longer hold opens the help screen and must not
// reach the K197. The outputs must be enabled both when the press is
// confirmed and at the release. v17: dB the same way
// (CONFIG_BUTTON_DB_PULSE_ON_RELEASE), with CONFIG_UI_ALARM_HOLD_MS as its
// limit - a longer hold opens the alarm settings.
//
// While disabled, a REL or dB press no longer starts the output pulse
// towards the K197 (the buttons then only generate the events above).
// Used by the UI to make REL/dB navigation-only while a UI screen is
// shown; safe to call from any task (the flag is read from the GPIO
// interrupt handler).
void buttonsSetRelDbEnabled(bool enabled);

// v18: REL and dB held at the same time (one pressed while the other is still
// down, in either order) - the marker chord. True once per chord; *firstEdgeMs
// = millis() of the first edge of the press that came first (what the display
// showed then is what gets the marker). Neither button then sends its pulse to
// the K197 at its release (with CONFIG_BUTTON_REL/DB_PULSE_ON_RELEASE; an
// immediate pulse could not be taken back). Same task rule as above.
bool buttonsTakeChord(uint32_t *firstEdgeMs);

// v16 (SYSTem:DIAGnostic?): the button task handle (NULL before buttonsBegin()).
TaskHandle_t buttonsGetTask();

// v23: "swallow the next press" - while the SD card alarm sounds (sdlog.ino),
// the first press of ANY key only silences it: armed with
// buttonsArmSwallow(true), the next confirmed press records no event at all
// (no press, no release, no chord, no click, no REL/dB pulse at the release,
// buttonsIsHeld() false for it), disarms itself and counts up
// buttonsSwallowedCount(). Any task may call both.
void buttonsArmSwallow(bool armed);
uint32_t buttonsSwallowedCount();

#endif // BUTTONS_H
