// alarm.h
// v17: limit alarm - watches the reading of the K197 against a lower and/or
// an upper limit and signals a reading outside them with a triangle above
// the minus sign of the LCD replica and, if allowed, an intermittent beep.
//
// Configuration, one per measurement function (DCV, ACV, DCI, ACI, OHM, dB -
// the function names of LOG:* and keithleyFunctionPrefix()):
//   mode    OFF / UPPER / LOWER / BOTH - which limits are watched. Whether
//           the alarm is ON is deliberately NOT persisted: after power-up
//           every function starts OFF; the last mode that was used
//           (UPPER/LOWER/BOTH) is remembered and persisted, and is what
//           CALCulate:LIMit:STATe ON switches to.
//   limits  lower and upper limit in BASE units (V, A, Ohm, dB - the units
//           of the log), persisted; NAN = not set yet (a limit that is not
//           set is not checked; switching a limit on sets it first to the
//           MAX / MIN of the last CONFIG_ALARM_DEFAULT_WINDOW_S seconds).
// plus one global SIGNAL: beep + arrow, or arrow only. The beep is also
// subject to SYSTem:BEEP (beeper.h); the arrow is always drawn.
//
// The comparison is done in counts of the last digit of the live reading
// (its unit and decimals), so a limit of 1.20000 V and a reading of
// 1.20000 V are equal, not "above" because of float rounding; "outside"
// means strictly above the upper / strictly below the lower limit. An "OL"
// reading counts as above the upper limit, "-OL" as below the lower one.
// The value compared is what the display shows - with REL active on the
// K197 that is the relative value. A frame that is not a reading (the
// self-test, Err197 without DMM frames) gives no alarm.
//
// The symbol on the LCD replica (display.h setAlarmIndicator(), FB_ALARM_*
// in framebuffer.h), in the free area above the minus sign:
//   alarm off            nothing
//   watching, inside     outline in the indicator color: a narrow triangle
//                        up (UPPER), down (LOWER), or two flat triangles
//                        above each other (BOTH)
//   above / below        the narrow triangle filled, bright, pointing up /
//                        down, blinking (CONFIG_ALARM_BLINK_ON_MS of every
//                        CONFIG_ALARM_BEEP_PERIOD_MS, in step with the beeps)
//
// Threads: alarmUpdateReading() runs in the capture task (core 0), the
// setters in the UI task (front panel) and the comm task (SCPI); a spinlock
// guards the configuration and the last reading. The beep pattern runs on a
// periodic esp_timer (CONFIG_ALARM_TICK_MS) that only looks at the state.

#ifndef ALARM_H
#define ALARM_H

#include <Arduino.h>
#include "keithley_shapes.h"

#define ALARM_FUNCTION_DCV   0
#define ALARM_FUNCTION_ACV   1
#define ALARM_FUNCTION_DCI   2
#define ALARM_FUNCTION_ACI   3
#define ALARM_FUNCTION_OHM   4
#define ALARM_FUNCTION_DB    5
#define ALARM_FUNCTION_COUNT 6
#define ALARM_FUNCTION_NONE  0xFF

#define ALARM_MODE_OFF   0
#define ALARM_MODE_UPPER 1
#define ALARM_MODE_LOWER 2
#define ALARM_MODE_BOTH  3
#define ALARM_MODE_COUNT 4

#define ALARM_SIGNAL_BEEP  0 // beep + arrow
#define ALARM_SIGNAL_ARROW 1 // arrow only
#define ALARM_SIGNAL_COUNT 2

// What the newest reading is (alarmGetState(), CALCulate:LIMit:FAIL?).
#define ALARM_STATE_NONE  0
#define ALARM_STATE_ARMED 1
#define ALARM_STATE_HIGH  2
#define ALARM_STATE_LOW   3

// "DCV" ... "DB" (the uiFunctionFilter() form) -> ALARM_FUNCTION_*, or
// ALARM_FUNCTION_NONE. Case-insensitive.
uint8_t alarmFunctionFromName(const char *name);
// ALARM_FUNCTION_* -> "DCV" ... "DB" ("" for anything else).
const char *alarmFunctionName(uint8_t function);

// Call once from setup(), after settingsBegin() and beeperBegin(), before
// the capture task runs: loads the configuration, starts the beep timer.
void alarmBegin();

// Every decoded frame (capture task, dmm_bridge.ino), right after
// setIndicators()/setUnit(); also with a blank frame when the DMM is silent.
void alarmUpdateReading(const char digits[KEITHLEY_DIGIT_COUNT], int8_t dpPos, uint16_t indicatorBits,
                        uint8_t unit);

// ALARM_STATE_* of the newest reading.
uint8_t alarmGetState();

// The function of the newest reading (ALARM_FUNCTION_NONE if it is not a
// reading of a known function).
uint8_t alarmGetLiveFunction();

// The newest reading as the alarm saw it: function, unit and decimals of
// the display, and the value in base units. Returns false (and leaves
// *value alone) if the newest frame is not a plain number (OL, self-test, no
// DMM).
bool alarmGetLiveValue(uint8_t *function, uint8_t *unit, uint8_t *decimals, double *value);

// Unit and decimals the display had the last time `function` was measured
// (for formatting its limits); false if it has not been measured since
// power-up - then a sensible default unit and decimals for the function are given.
bool alarmGetDisplayFormat(uint8_t function, uint8_t *unit, uint8_t *decimals);

// ALARM_MODE_* in effect for `function` (OFF while it is switched off).
uint8_t alarmGetMode(uint8_t function);
// The remembered mode (UPPER/LOWER/BOTH, never OFF) - what "ON" switches to.
uint8_t alarmGetStoredMode(uint8_t function);
// Switches the alarm of `function`: OFF, or a mode (which is then also
// remembered). A limit the mode needs that is not set yet starts at the
// MAX (upper) / MIN (lower) of this function's records of the last
// CONFIG_ALARM_DEFAULT_WINDOW_S seconds of the log, or - with none there - at
// the newest reading, if it is a number of this function. saveNow: true = SCPI
// (EEPROM at once), false = front panel (deferred).
void alarmSetMode(uint8_t function, uint8_t mode, bool saveNow);

// MIN and MAX of `function`'s records (OL excluded) in the last
// CONFIG_ALARM_DEFAULT_WINDOW_S seconds of the log (without RTC time: the
// usual number of readings of that time). False if there is none. Reads the
// log in small chunks (short mutex holds); any task except the capture task.
bool alarmRecentRange(uint8_t function, double *minimum, double *maximum);

// A limit in base units, NAN if not set.
double alarmGetLimit(uint8_t function, bool upper);
// Sets a limit (NAN clears it). Not range-checked here.
void alarmSetLimit(uint8_t function, bool upper, double baseValue, bool saveNow);

// One step of a limit on the settings screen, `direction` -1/+1 times
// `stepCounts` counts of the last digit the limit is shown with (in the unit
// and decimals of alarmGetDisplayFormat()); a step of more than one count
// also rounds the result to a multiple of the step. A limit that is not set
// starts from the newest reading (or zero). Clamped to +-199999 counts of
// the display at zero decimals. Saved deferred. Returns false if nothing
// could be changed.
bool alarmStepLimit(uint8_t function, bool upper, int8_t direction, uint32_t stepCounts);

// Formats a limit for the screens: "-0.50000" (no unit) in the unit and
// decimals of alarmGetDisplayFormat(), "--" if it is not set.
void alarmFormatLimit(char *out, size_t size, uint8_t function, bool upper);

uint8_t alarmGetSignal();
void alarmSetSignal(uint8_t signal, bool saveNow);

// The body of the beep timer (public for the host tests, which call it by
// hand): every CONFIG_ALARM_TICK_MS.
void alarmTick();

#endif // ALARM_H
