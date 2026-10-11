// config.h
//
// Every build-time setting a person adapting this firmware to their
// own physical wiring, communication preference, identification, or
// look-and-feel would want to change lives here - and ONLY here.
// Every other file (ESP32S3_K197.ino, display.*, serial_out.*,
// settings.*, ...) is expected to change freely across firmware
// updates without ever touching this one, so keeping this file
// untouched across an update is enough to keep a personal build's
// wiring/settings intact.
//
// If you find yourself changing a #define somewhere else to adapt
// this firmware to your own hardware, that #define probably belongs
// in here instead.

#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include "settings.h"

// ---------------------------------------------------------------
// Pin assignments
// ---------------------------------------------------------------

// TFT display (NV3007 panel, Adafruit_NV3007 library, default
// Arduino SPI object - see display.ino). MISO is not used, the
// display is write-only.
#define TFT_CS   2
#define TFT_DC   10
#define TFT_RST  9
#define TFT_SCK  12
#define TFT_MOSI 11

// DMM internal display bus tap (SPI3, ESP32-S3 acting as slave - see
// dmm_bridge.ino). MISO is not used, the ESP32 only listens, it
// never talks back on this bus.
#define DMM_SCK_PIN  4
#define DMM_MOSI_PIN 5
#define DMM_CS_PIN   6

// v13: no valid DMM frame for this long (milliseconds; the K197 sends
// about 3.5 per second) -> the LCD replica shows "Err197" and the SPI
// slave is restarted, again after every further such period of silence.
#define CONFIG_DMM_NO_DATA_MS 4000 // v22: 4 s (was 10 s)
// v22: if CS is low and has had no edge for CONFIG_DMM_OFF_QUIET_MS (the
// divider holds it low while the K197 is unpowered), it shows "oFF197"
// instead and the ESP32 may go to light sleep (power.h). Quiet time rather
// than "no edge in the whole period": while its supply decays the K197 still
// sends a few broken frames, and an SPI slave restart puts a glitch on CS.
#define CONFIG_DMM_OFF_QUIET_MS 2000 // v22: 2 s, must stay below CONFIG_DMM_NO_DATA_MS

// v22: light sleep while the K197 is switched off (power.h). 0 = never sleep.
#define CONFIG_POWER_SLEEP_ENABLE 1
// stay awake this long after a key, SCPI traffic or a key/UART wake-up (ms)
#define CONFIG_POWER_KEY_AWAKE_MS 60000
// ... and this long after a DMM / unknown wake-up, before sleeping again (ms)
#define CONFIG_POWER_MIN_AWAKE_MS 3000
// a timer wake-up every this many seconds as a safety net (0 = none), awake for (ms)
#define CONFIG_POWER_TIMER_WAKE_S   300
#define CONFIG_POWER_TIMER_AWAKE_MS 200
// 1 = a start bit on the SCPI UART RX wakes up (the first command is usually lost)
#define CONFIG_POWER_WAKE_ON_UART 1
// how often powerPoll() looks (ms), how long it waits for the display to finish
// a frame before sleeping (ms), and at most how long a waking key may stay held (ms)
#define CONFIG_POWER_CHECK_MS            100
#define CONFIG_POWER_IDLE_WAIT_MS        300
#define CONFIG_POWER_KEY_RELEASE_WAIT_MS 5000

// EXPERIMENT (v13): 1 = switch the ESP32's brownout detector off at the
// very start of setup(). It only covers what comes AFTER the start of
// setup() (TFT, PSRAM, tasks) - the ROM/bootloader phase before it keeps
// the detector. Use: when powered from the DMM, a dip during our start-up
// then no longer resets the ESP32 (one current surge instead of a second
// one on the restart). Risks: on a really low supply the chip may run
// erratically instead of resetting, and an EEPROM/flash write at that
// moment (settings) could be corrupted. Not a fix for the K197 itself.
//#define CONFIG_BROWNOUT_DETECTOR_DISABLE 0
#define CONFIG_BROWNOUT_DETECTOR_DISABLE 0

// SCPI command UART (serial_out.ino), separate from the USB/debug
// Serial port. Always begun (see serialOutBegin()) regardless of
// which port SYSTem:COMM currently has active, so switching between
// them at runtime needs no reboot.
#define SERIAL_OUT_TX_PIN 17
#define SERIAL_OUT_RX_PIN 18
#define SERIAL_OUT_BAUD   115200

// Front-panel buttons (new ESP32-driven front panel - the four
// original K197 buttons move to the rear panel, wired to the K197's
// own logic; see docs/HARDWARE.md, "Front-panel buttons"). Wired to
// the four GPIOs whose only alternate function is JTAG (MTCK/MTDO/
// MTDI/MTMS) - this project never uses JTAG, so they are free, and
// using them here leaves the more generic-purpose GPIOs open for
// other things. Active LOW, internal pull-up enabled in
// buttonsBegin() (buttons.ino) - these are ordinary bidirectional
// ESP32-S3 GPIOs, not input-only, and take an internal pull-up/
// pull-down fine (that restriction only applies to GPIO45/GPIO46, not
// to these).
//
// NOTE: only REL and dB drive an output pulse - STO/RCL are read
// (pressed/released/short/long all reported to the debug console,
// see buttons.ino) but nothing further acts on them yet, their K197
// UI behavior is still unscoped (see docs/DEVELOPMENT.md).
#define BTN_REL_PIN 39
#define BTN_DB_PIN  40
#define BTN_STO_PIN 41
#define BTN_RCL_PIN 42

// Debounce window for all four buttons above: a raw GPIO transition
// only becomes the new confirmed state once the line has held that
// way for this long, re-verified at confirmation time (not just
// trusted from the original edge) - see buttons.ino's own comment for
// why that re-check matters. 30ms is a conventional starting point
// for a mechanical push-button; raise it if a single press still
// occasionally misreports on real hardware.
#define CONFIG_BUTTON_DEBOUNCE_MS 30

// How long a button must stay held down to count as a LONG press
// rather than a SHORT one - classified at release, by comparing the
// held duration against this. 600ms is a conventional starting point.
#define CONFIG_BUTTON_LONG_PRESS_MS 600

// How long buttons.ino drives REL_OUT_PIN/DB_OUT_PIN HIGH for, once
// per REL/dB button press (a fixed-duration pulse, not "however long
// the button stays held down" - see buttons.ino). 100ms was the
// author's own starting point; raise or lower once it's confirmed
// against how long the K197's own original buttons need to be
// considered "pressed" by its internal logic.
#define CONFIG_BUTTON_PULSE_MS 100

// v12: REL sends its pulse to the K197 at the RELEASE of a short press
// (shorter than CONFIG_UI_HELP_HOLD_MS) instead of at the first edge,
// because holding REL opens the help screen and a REL already sent could
// not be taken back. Costs the K197 a delay of about the press length
// (typically 60-150 ms). 0 = old behavior (immediate pulse; the help hold
// then also toggles REL on the K197). dB: see the next setting.
#define CONFIG_BUTTON_REL_PULSE_ON_RELEASE 1

// v17: dB the same way - its pulse goes to the K197 at the RELEASE of a
// press shorter than CONFIG_UI_ALARM_HOLD_MS, because holding dB on the LCD
// screen opens the alarm settings (alarm.h). 0 = the old immediate pulse
// (the hold then also toggles dB on the K197).
#define CONFIG_BUTTON_DB_PULSE_ON_RELEASE 1

// The button task (buttons.ino) that confirms debounced presses and
// releases and wakes the UI. It must always get the CPU as soon as a
// button interrupt wakes it, so its priority is the highest of all
// tasks on core 1 (the display render/UI task has 2, the SCPI task 1).
#define CONFIG_BUTTONS_TASK_STACK_BYTES 4096
#define CONFIG_BUTTONS_TASK_PRIORITY    3

// REL/dB relay emulation towards the rear panel - drives a
// transistor (a PC817 optocoupler, per this session's hardware design
// - see docs/DEVELOPMENT.md) onto the K197's own REL/dB contacts,
// relocated to the rear panel. Active HIGH - buttons.ino drives one
// of these HIGH the instant the matching front-panel button is
// pressed (straight from the GPIO interrupt handler, for the fastest
// possible response), for CONFIG_BUTTON_PULSE_MS.
#define REL_OUT_PIN 13
#define DB_OUT_PIN  14

// I2C bus for the DS3231 RTC (see rtc.h/rtc.ino), backing
// SYSTem:TIME/SYSTem:TIME? in serial_out.ino.
#define I2C_SDA_PIN 15
#define I2C_SCL_PIN 16

// Acoustic signalling. Active buzzer module (has its own oscillator
// built in) - sounds from a plain HIGH, silent on LOW, so beeper.ino
// just uses digitalWrite(), no tone()/PWM generation needed. Shares
// GPIO48 with the onboard LED; that is fine here, the LED flickering
// along with the beeper is not a problem for this project.
#define BEEPER_PIN 48

// v13: rear-panel trigger signals (trigger_io.h), each through an
// optocoupler. GPIO7/GPIO8 were chosen because nothing else uses them and
// they are not strapping, USB, flash or octal-PSRAM pins (ESP32-S3R8:
// avoid 0, 3, 19, 20, 26-37, 45, 46). CHECK that both are on the header
// of this particular board; 1, 21, 38 and 47 are the other free candidates.
#define MC_OUT_PIN      7 // METERING COMPLETE OUTPUT
#define EXT_TRIG_IN_PIN 8 // EXTERNAL TRIGGER INPUT

// METERING COMPLETE pulse length on the GPIO (microseconds). The
// receiving system reacts to the falling edge; it must not be shorter
// than 10 us. NOTE: a PC817-class optocoupler has switching times of
// several to ~20 us with typical resistor values, so 10 us on the GPIO
// may come out short or flattened at the connector - measure it, and
// raise this (e.g. 50-100 us, the capture task can afford it) or use a
// fast optocoupler (6N137, H11L1) if needed.
#define CONFIG_MC_PULSE_US 10
// GPIO level DURING the pulse. HIGH = the GPIO drives the optocoupler LED
// (lit during the pulse), so the output transistor pulls the connector
// to zero for the pulse.
#define CONFIG_MC_OUT_ACTIVE_LEVEL HIGH

// EXTERNAL TRIGGER: which GPIO edge counts. With the usual wiring (the
// optocoupler transistor pulls the GPIO down while the external signal is
// low) the falling edge of the external pulse is a FALLING edge here too.
// The interrupt catches pulses of a few microseconds; the optocoupler is
// the real limit (PC817: tens of us are safe, the 72 us of a K2000-style
// trigger is fine).
#define CONFIG_EXT_TRIG_EDGE   FALLING
#define CONFIG_EXT_TRIG_PULLUP 1 // internal pull-up on the input (open-collector optocoupler output)

// Startup chirp (beeper.ino) - one short beep once setup() finishes,
// gated by SYSTem:BEEP (settings.h/serial_out.ino).
#define CONFIG_BEEP_STARTUP_DURATION_MS 50

// Click on every front-panel button press (beeper.ino, called by the
// button task once a press is confirmed) - the short tick that tells the
// person the press was registered. Gated by SYSTem:BEEP like every other
// acoustic signal. In milliseconds; the button task waits this long, so
// keep it a few milliseconds.
#define CONFIG_BEEP_CLICK_MS 5

// Long beep confirming that the whole log was cleared from the front panel
// (RCL held on the statistics screen, see CONFIG_UI_LOG_CLEAR_HOLD_MS).
// Milliseconds. Played WITHOUT blocking (beeperBeepAsync(), an esp_timer
// ends it), so the picture and the buttons keep running meanwhile. Gated by
// SYSTem:BEEP like every other acoustic signal.
#define CONFIG_BEEP_LOG_CLEAR_MS 1000

// ---------------------------------------------------------------
// SCPI *IDN? identification
// ---------------------------------------------------------------

// SCPI convention is "<manufacturer>,<model>,<serial number>,
// <firmware version>". Manufacturer/model spelling matches the
// format Keithley's own higher-end models use. Update the serial
// number/firmware here if this snapshot ever gets reused for a
// different physical unit.
// ---------------------------------------------------------------
// Vrekrer SCPI parser limits (v12, 08.10.)
// ---------------------------------------------------------------
// The library sizes its internal tables from these macros, each wrapped
// in #ifndef in Vrekrer_scpi_parser.h - so a value defined BEFORE the
// first #include <Vrekrer_scpi_parser.h> wins. config.h is included at
// the top of the main sketch file, ahead of that include, and Arduino
// puts the main .ino first in the merged translation unit, so this is the
// place. Plain #undef + #define (not #ifdef-guarded): nothing defines
// them before this point, so an #ifdef here would silently do nothing;
// #undef of an undefined macro is harmless and gives no warning.
// Current use: 26 tokens, 40 commands (MEASure:*, FETCh? added).
// Headroom is cheap: a few bytes per entry.
#undef SCPI_MAX_TOKENS
#define SCPI_MAX_TOKENS 48       // distinct keywords (SYSTem, AUTO, MEASure, ...) - v22: 43 used
#undef SCPI_MAX_COMMANDS
#define SCPI_MAX_COMMANDS 80     // registered commands (each "X" and "X?" counts) - v22: 67 registered
#undef SCPI_HASH_TYPE
#define SCPI_HASH_TYPE uint32_t  // command hash width; the default 8 bits risks two commands sharing a hash at 40+ commands

#define SCPI_IDN_MANUFACTURER "KEITHLEY INSTRUMENTS INC."
#define SCPI_IDN_MODEL        "MODEL 197"
#define SCPI_IDN_SERIAL       "000000" // put your instrument's serial number here (reported by *IDN?)
#define SCPI_IDN_FIRMWARE     "C2v22"

// ---------------------------------------------------------------
// First-boot defaults
// ---------------------------------------------------------------
// Applied exactly once, the first time this firmware ever runs on a
// blank (or incompatible-version) EEPROM - see settingsBegin() in
// settings.ino. Every value below can still be changed at runtime
// over SCPI afterwards; changing it here only changes what a brand
// new or factory-reset unit starts out with.

// Which physical port the SCPI interface talks on - UART (the
// dedicated second UART above) or USB (the same port
// Serial.begin(115200) opens, shared with the debug prints already in
// this sketch - see serial_out.ino for what that trades off).
// Switchable at runtime with no reboot via SYSTem:COMM <UART|USB>.
// UART is the real hardware setup and the right default; USB is
// convenient during development (the Arduino IDE's own Serial Monitor
// is then enough to talk SCPI, no separate USB-serial adapter needed)
// but has to be asked for explicitly with SYSTem:COMM USB once a unit
// is up.
#define CONFIG_DEFAULT_COMM_PORT      SETTINGS_COMM_UART
// v13: where debug messages go after a first boot / settings update
// (SYSTem:DEBUG:PORT changes it at runtime, persisted). UART = the SCPI
// UART behind the CH340 on the rear panel, which keeps its USB link while
// the instrument is off - so nothing from the power-up is missed. Lines
// sent there start with "# " so they cannot be mistaken for SCPI replies.
#define CONFIG_DEFAULT_DEBUG_PORT     SETTINGS_COMM_UART
#define CONFIG_DEFAULT_READING_FORMAT SETTINGS_READING_RAW
#define CONFIG_DEFAULT_AUTO_ENABLED   false
#define CONFIG_DEFAULT_AUTO_INTERVAL  0
#define CONFIG_DEFAULT_THEME          0
#define CONFIG_DEFAULT_DEBUG_ENABLED  false
#define CONFIG_DEFAULT_BEEP_ENABLED   true

// Vertical offset (pixels from the top of the panel) where drawing
// starts - see setDisplayYOffset() (display.h) and SYSTem:YOFFset
// (serial_out.ino). 24 was this project's original assumption before
// the display was actually mounted behind the meter's real plastic
// bezel, which needed a small correction - adjust this to whatever
// the author's own unit needs once mounted; SYSTem:YOFFset persists
// the corrected value from then on, so this default only matters for
// a brand new/factory-reset unit's very first boot.
#define CONFIG_DEFAULT_DISPLAY_Y_OFFSET 24

// ---------------------------------------------------------------
// Dual-core task configuration
// ---------------------------------------------------------------
// Core 0 owns DMM acquisition (the capture task, core0Task() in
// ESP32S3_K197.ino) plus the display BLIT task, which streams
// finished frames to the panel (display.ino). Core 1 owns SCPI/UART
// communication (core1Task()) plus the display RENDER task, which
// composes those frames (display.ino). See display.ino's header
// comment and docs/DEVELOPMENT.md for the full reasoning.
//
// Priorities matter here, they are not arbitrary:
//   core 0: capture task (2) > blit task (1). A DMM frame arriving
//           mid-blit preempts the blit at once, so the display can
//           never delay capture. Keep the capture priority strictly
//           above the blit priority.
//   core 1: render task (2) > SCPI/comm task (1). A long SCPI reply
//           (a LOG:DATA? dump) therefore cannot freeze the picture:
//           the render task preempts it for the few milliseconds one
//           frame takes. The comm task may still block on slow I/O
//           without affecting acquisition on the other core.
//
// Stack sizes are in BYTES (the ESP32 Arduino core's
// xTaskCreatePinnedToCore(), unlike upstream FreeRTOS, takes a byte
// count here, not a word count) - generous starting points, not
// measured minimums. Check actual headroom with
// uxTaskGetStackHighWaterMark() once the tasks are running on real
// hardware (for the two display tasks, displayGetStats() in display.h
// reports it), and adjust if any is far from these figures. Core 1's
// comm task is the largest because serial_out.ino's SCPI command
// handling and datalog.ino's LOG:* replies use String and a qsort()
// scratch buffer (LOG:MEDian?), both heavier on stack than the
// straight-line capture/decode path.
#define CONFIG_CORE0_TASK_STACK_BYTES 8192
#define CONFIG_CORE1_TASK_STACK_BYTES 12288
#define CONFIG_CORE0_TASK_PRIORITY    2
#define CONFIG_CORE1_TASK_PRIORITY    1

// Display render task (core 1) and blit task (core 0).
#define CONFIG_DISPLAY_RENDER_TASK_STACK_BYTES 8192 // v8: also runs the UI (printf-style number formatting, debug prints of button events)
#define CONFIG_DISPLAY_RENDER_TASK_PRIORITY    2
#define CONFIG_DISPLAY_BLIT_TASK_STACK_BYTES   4096
#define CONFIG_DISPLAY_BLIT_TASK_PRIORITY      1

// ---------------------------------------------------------------
// User interface (ui.ino) - new in v8
// ---------------------------------------------------------------

// How often the render/UI task looks at the buttons and the UI state
// when nothing else wakes it (milliseconds). Together with the button
// debounce time this is the delay between a press and the screen
// reacting.
#define CONFIG_UI_POLL_MS 10

// Holding RCL this long (milliseconds, counted from the first edge of the
// press) on the statistics screen clears the WHOLE log (same as
// LOG:CLEar), confirmed by a long beep (CONFIG_BEEP_LOG_CLEAR_MS). It fires
// while the button is still held, once per press; the press must have
// started on the statistics screen. On the LCD screen RCL opens the graph (v14).
#define CONFIG_UI_LOG_CLEAR_HOLD_MS 3000

// v12: holding REL this long (milliseconds, from the first edge) on the
// LCD or the statistics screen opens the help screen (help_text.h): a
// two-line header (KEITHLEY 197R, firmware version, build time), then the
// help text, 6 lines per screen. On the help screen (v13): REL click =
// previous page, dB click = next page (at the release; always a full page,
// the last page ends with the last line of the text), REL held this long =
// back to the start, dB held = to the end, RCL nothing, STO = back to the
// screen it was opened from. REL/dB never reach the K197 there. Logging
// goes on meanwhile.
#define CONFIG_UI_HELP_HOLD_MS 1000

// v13: the help screen closes by itself (like STO) after this long without
// any button being pressed or held (milliseconds). v15: the same for the
// settings screens (graph settings -> graph, DMM settings -> LCD).
#define CONFIG_UI_HELP_TIMEOUT_MS 60000

// v15: the separator lines of the help and the settings screens (under the
// header/title) are dotted: this many pixels on, then this many off (1 / 1 =
// "1-0-1-0", chosen on the real panel - 1-00 looked fine on a monitor but gave
// odd effects on the panel). 1 / 0 = solid.
#define CONFIG_UI_SEPARATOR_ON  1
#define CONFIG_UI_SEPARATOR_OFF 1

// The quick statistics screen (STO button) computes AVG/MED/MAX/MIN
// over the log records of the current measurement function that are
// not older than the chosen interval: last minute, 5 minutes, 15
// minutes, hour or 6 hours, and down to 5 or 10 seconds (REL = shorter,
// dB = longer on that screen). The whole log is meant for a separate screen
// with graphs; 6 hours is at most about 75600 records (3.5 readings per
// second), which the sliced job handles in the background.
// This is the interval selected after power-up: 0 = 5 SEC, 1 = 10 SEC,
// 2 = 1 MIN, 3 = 5 MIN, 4 = 15 MIN, 5 = 1 HOUR, 6 = 6 HRS.
// Since v11 the chosen interval is persisted in EEPROM (settings.h), so this
// is only the FIRST-BOOT default (blank or incompatible EEPROM).
#define CONFIG_UI_STATS_DEFAULT_INTERVAL_INDEX 2

// Settings changed from the front panel (so far the statistics interval)
// are written to flash only after they have stayed unchanged this long
// (milliseconds) - stepping through the intervals costs one flash write,
// not one per press. Switching off sooner keeps the previous value.
#define CONFIG_SETTINGS_DEFERRED_SAVE_MS 10000

// The statistics job works in slices so that it never stalls the
// picture: each pass of the UI loop it processes records in steps of
// CONFIG_UI_STATS_STEP_RECORDS until about CONFIG_UI_STATS_SLICE_US
// microseconds have passed, then lets the loop get on with the rendering
// (and while a job runs the loop comes back after a single tick, 1 ms, not
// after CONFIG_UI_POLL_MS). A minute (a few hundred records) is done
// within the first slice, 6 hours (about 75600 records) takes a second
// or two in the background.
#define CONFIG_UI_STATS_STEP_RECORDS 256
#define CONFIG_UI_STATS_SLICE_US     3000

// Time windows need the records' RTC timestamps. Without them (no RTC,
// or not synced when the records were made) the number of records in a
// window is ESTIMATED from the recording rate: this many records per
// second when SYSTem:AUTO's interval is 0 (one record per measurement -
// the K197 delivers about 3.5 readings per second), otherwise one per
// SYSTem:AUTO interval.
#define CONFIG_UI_FALLBACK_RECORDS_PER_SECOND 3.5f

// The statistics are recomputed at most this often (milliseconds, counted
// from the end of the last computation) - and only when a new reading
// has arrived; the live value on the same screen still follows every
// reading.
#define CONFIG_UI_STATS_REFRESH_MS 500

// 1 = while a UI screen is shown, REL and dB presses no longer drive
// the K197's REL/dB (they are navigation keys there). 0 = they keep
// working everywhere.
#define CONFIG_UI_QUICKSTATS_BLOCKS_REL_DB 1

// ---------------------------------------------------------------
// Graph screen (graph.ino) - new in v14
// ---------------------------------------------------------------
// RCL on the LCD screen opens the graph of the logged values of the
// current measurement function. On the graph: REL click = shorter, dB =
// longer interval, RCL click = next Y axis mode (AUTO -> ROBUST -> ZERO -> SUPP ->
// AUTO, SUPP centred on the newest value), RCL held CONFIG_GRAPH_SETUP_HOLD_MS
// = the graph settings screen, REL held CONFIG_UI_HELP_HOLD_MS = help, STO = back to the LCD. On the
// settings screen: RCL = next line, REL/dB = change the value, STO = back to
// the graph.
//
// First-boot defaults (the graph settings are persisted in EEPROM like the
// statistics interval, deferred by CONFIG_SETTINGS_DEFERRED_SAVE_MS):
//   interval: 0 = 10 SEC, 1 = 30 SEC (both v15), 2 = 1 MIN, 3 = 5 MIN,
//             4 = 15 MIN, 5 = 1 HOUR, 6 = 6 HRS, 7 = 24 HRS, 8 = FULL (the
//             whole log)
//   Y mode:   0 = AUTO (from the lowest to the highest value in the window),
//             1 = ZERO (the axis always includes zero),
//             2 = SUPP (suppressed zero: fixed centre +- a fixed span)
//   trace:    0 = MIN/MAX band + average, 1 = average only, 2 = band only
//   SUPP span: index into +-10, 20, 50, 100, 200, 500, 1000, 2000, 5000,
//             10000 counts of the last displayed digit (3 = +-100 counts)
#define CONFIG_GRAPH_DEFAULT_INTERVAL_INDEX 5
#define CONFIG_GRAPH_DEFAULT_YMODE          0
#define CONFIG_GRAPH_DEFAULT_TRACE          0
#define CONFIG_GRAPH_DEFAULT_SPAN_INDEX     6 // +-100 counts (v21: index moved by the new +-1/2/5)

// v15: language of the help screens and the graph settings screen after the
// first boot: 0 = CZ (Czech without diacritics), 1 = EN (help_text.h).
// Changed with SYSTem:LANGuage CZ|EN, persisted in EEPROM.
#define CONFIG_DEFAULT_LANGUAGE 0

// RCL held this long (milliseconds) on the graph screen opens the graph
// settings. A shorter RCL press (recognised at the release) switches to the
// next Y axis mode.
#define CONFIG_GRAPH_SETUP_HOLD_MS 1000

// v15: RCL held this long (milliseconds) on the LCD screen opens the DMM
// settings (language, theme, sound); a shorter RCL press (recognised at the
// release) opens the graph.
#define CONFIG_UI_DMM_SETUP_HOLD_MS 1000

// ---------------------------------------------------------------
// Limit alarm (alarm.h / alarm.ino) - new in v17
// ---------------------------------------------------------------
// dB held this long (milliseconds) on the LCD screen opens the alarm
// settings of the function being measured; a shorter dB press is sent to the
// K197 at its release (CONFIG_BUTTON_DB_PULSE_ON_RELEASE).
#define CONFIG_UI_ALARM_HOLD_MS 1000

// On the alarm settings screen, line of a limit: RCL held this long takes
// the live reading as the limit (a shorter RCL = next line, at the release).
#define CONFIG_UI_ALARM_TAKE_HOLD_MS 1000

// Limit editing with REL (-) / dB (+): one press = one count of the last
// digit; held, the value repeats after CONFIG_UI_ALARM_REPEAT_DELAY_MS every
// CONFIG_UI_ALARM_REPEAT_MS, and the step grows tenfold after every
// CONFIG_UI_ALARM_REPEAT_PER_DECADE repeats (1, 10, 100 ... counts, at most
// CONFIG_UI_ALARM_MAX_STEP_COUNTS); a grown step also rounds the value to a
// multiple of itself.
// v19 (user, after trying v18 on the instrument: too fast to stop on a
// wanted value): 5 steps per second instead of 10, and each step size lasts
// 3 s (15 repeats) instead of 1 s before it grows tenfold. Example from
// 1.23456 V held up: 3 s of single counts (-> 1.23471), 3 s of tens (-> about
// 1.2362), 3 s of hundreds (-> about 1.2512), then thousands.
#define CONFIG_UI_ALARM_REPEAT_DELAY_MS   600
#define CONFIG_UI_ALARM_REPEAT_MS         200
#define CONFIG_UI_ALARM_REPEAT_PER_DECADE 15
#define CONFIG_UI_ALARM_MAX_STEP_COUNTS   10000

// The acoustic signal while a reading is outside a limit (and both the
// alarm's SIGNAL setting and SYSTem:BEEP allow sound): a beep of
// CONFIG_ALARM_BEEP_MS every CONFIG_ALARM_BEEP_PERIOD_MS, the first one at
// once. The pattern runs on a periodic timer of CONFIG_ALARM_TICK_MS
// (both times are rounded to it).
#define CONFIG_ALARM_BEEP_MS        100
#define CONFIG_ALARM_BEEP_PERIOD_MS 500
// The bright triangle blinks with the same period, shown for this long of it
// (starting together with the beep) - also with the sound off.
#define CONFIG_ALARM_BLINK_ON_MS    300
#define CONFIG_ALARM_TICK_MS        50

// SIGNAL after the first boot: 0 = beep + arrow, 1 = arrow only. The limits
// and the remembered mode (UPPER/LOWER/BOTH) of every function are persisted
// (deferred from the front panel, at once from SCPI); the alarm itself is
// always OFF after power-up.
#define CONFIG_ALARM_DEFAULT_SIGNAL 0

// A limit switched on without a value (front panel or CALCulate:LIMit:STATe)
// starts at the MAX (upper) / MIN (lower) of the function's readings of the
// last this many seconds of the log - the live reading if there are none.
#define CONFIG_ALARM_DEFAULT_WINDOW_S 10

// v20: the first this many DMM readings after a change of the measurement
// function (DCV/ACV/DCI/ACI/OHM/dB), after power-up and after the DMM's
// self-test are still settling: they are logged with a SETTLE flag, but the
// statistics, the graph, LOG:MIN?/MAX?/AVERage?/MEDian?, the alarm (and its
// default limits) leave them out. 0 = off.
#define CONFIG_DATALOG_SETTLE_READINGS 7 // about 2 s - 3 was not enough on the instrument
// v22: after the K197 was switched on (it was off - "oFF197" - or the ESP32
// has just started) its readings take much longer to settle (~6-7 s on the
// graph), so the first this many readings then settle (~3.3 a second).
#define CONFIG_DATALOG_SETTLE_POWERON_READINGS 20 // about 6 s, enough on the instrument

// v21 TEST: caps the log at this many records (0 = off, the whole free PSRAM
// as usual). With e.g. 20000 records the ring buffer wraps after ~1 h 45 min
// at 3.2 readings/s instead of ~52 h, so the wrap-around (statistics, graph
// FULL, markers, LOG:DATA?) can be tested overnight. The boot report and
// SYSTem:DIAGnostic? say "TEST LIMIT" while it is on. Set back to 0 for normal use.
#define CONFIG_DATALOG_TEST_CAPACITY 0

// ---------------------------------------------------------------
// Markers (marker.h / marker.ino) - new in v18
// ---------------------------------------------------------------
// REL + dB together on the LCD or graph screen, an EXTERNAL TRIGGER edge with
// MEASure:TRIGger MARK, or LOG:MARK puts a marker on the reading shown. "TAG"
// (in place of REM) is lit for CONFIG_MARKER_SHOW_MS; a double beep of
// CONFIG_MARKER_BEEP_MS with CONFIG_MARKER_BEEP_GAP_MS between.
#define CONFIG_MARKER_SHOW_MS     2000
#define CONFIG_MARKER_BEEP_MS     40
#define CONFIG_MARKER_BEEP_GAP_MS 80
// EXTERNAL TRIGGER as the marker input: edges closer than this to the last
// accepted one are ignored (contact bounce of a foot switch, a double press).
#define CONFIG_MARKER_EXT_HOLDOFF_MS 300
// The EXTERNAL TRIGGER line must be quiet this long before a burst of edges is
// judged (pressed = resting at the active level) - longer than contact bounce.
#define CONFIG_EXT_TRIG_SETTLE_MS 20

// SUPP mode: the axis shows the chosen span (+- counts) plus this much
// reserve on each side, in percent of the span - values slightly outside
// the span are still drawn, not clipped (e.g. +-100 counts, 10 % -> the
// axis covers +-110 counts). The grid stays on 1-2-5 values. 0 = exactly
// the span.
#define CONFIG_GRAPH_SUPP_MARGIN_PERCENT 10

// v21: Y axis mode ROBUST - the share of the columns (each end: the highest
// maxima, the lowest minima) left out of the axis range, in percent, and the
// least number of columns with data below which ROBUST is plain AUTO.
#define CONFIG_GRAPH_ROBUST_PERCENT     2
#define CONFIG_GRAPH_ROBUST_MIN_COLUMNS 20

// Two columns with values are joined by the average line unless the gap
// between them is longer than this many seconds (or 2.5 x the
// SYSTem:AUTO interval, if that is longer) - a longer gap means the DMM
// delivered nothing (off, Err197, ...) and stays visible as a gap.
#define CONFIG_GRAPH_GAP_SECONDS 10

// The graph is redrawn at most this often while new readings arrive or the
// log is being read (milliseconds); the clock in the header still ticks
// every second.
#define CONFIG_GRAPH_REFRESH_MS 250

// Reading the log for a graph (up to the whole log, ~500k records) runs in
// slices like the statistics job: records in steps of 64 until this many
// microseconds have passed per pass of the UI loop.
#define CONFIG_GRAPH_SLICE_US 3000

// Brightness of the graph's secondary elements, as a coverage 0-255 on the
// palette ramps (rounded to the 21 steps of a ramp): grid (dotted, on the
// indicator ramp), axes (indicator ramp) and the MIN/MAX band (bright
// ramp). The average line and the numbers are the full `bright`, the labels
// the full `indicator`. Tune on the real panel.
#define CONFIG_GRAPH_GRID_LEVEL     85
#define CONFIG_GRAPH_AXIS_LEVEL     140
#define CONFIG_GRAPH_ENVELOPE_LEVEL 105

// After the render task is woken it waits this long before it looks
// at the requested state, so the three setReadingSegments()/
// setIndicators()/setUnit() calls the capture task makes for one DMM
// frame (microseconds apart) are always rendered together instead of
// possibly as a half-updated picture. Costs this much extra latency
// per update, nothing else.
#define CONFIG_DISPLAY_RENDER_SETTLE_MS 2

// How many framebuffer rows the blit task converts to RGB565 and
// sends per SPI call. Larger = fewer calls, bigger scratch buffer
// (rows * 428 * 2 bytes of RAM). 8 rows = ~6.8 kB.
#define CONFIG_DISPLAY_BLIT_CHUNK_ROWS 8

// 0 = stream frames with one setAddrWindow() + writePixels() calls
// (fast, standard Adafruit_SPITFT API, but not yet exercised against
// the real Adafruit_NV3007 library on hardware). 1 = fall back to one
// drawRGBBitmap() per chunk instead - the call the previous versions
// of this project used for every glyph. Try 1 if the picture comes
// out garbled, shifted or with swapped colors with 0.
#define CONFIG_DISPLAY_BLIT_USE_DRAWRGBBITMAP 0

// ---------------------------------------------------------------
// Color themes
// ---------------------------------------------------------------
// See display.h for KeithleyTheme's use and RGB565(). Add, remove or
// recolor themes here. The theme index a running unit has selected
// is persisted in EEPROM (see settings.h) and just indexes into this
// array - keep existing entries in place and append new ones at the
// end rather than reordering, or a previously-chosen index could
// resolve to a different theme after an update.

// Converts classic 8-bit-per-channel RGB (0-255 each) into the
// RGB565 format the display actually needs. constexpr means this
// runs at compile time when given literal arguments (as in themes[]
// below), so it costs nothing at runtime.
constexpr uint16_t RGB565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
}

// Four colors, not three - this matters:
//   bg        - screen/off background
//   ghost     - unlit digit segments (very dim, barely visible outline)
//   indicator - indicator words (AUT, REL, ...) when ON - dimmed
//               relative to the digit color, NOT the same as ghost
//   bright    - lit digit segments, minus sign, unit when shown
struct KeithleyTheme
{
    const char *name;
    uint16_t bg;
    uint16_t ghost;
    uint16_t indicator;
    uint16_t bright;
};

const KeithleyTheme themes[] = {
    // name         bg                      ghost                   indicator               bright
    {"RED",  RGB565(0, 0, 0),       RGB565(35, 0, 0),       RGB565(255, 20, 40),    RGB565(255, 20, 40)}, 
    {"GRN",  RGB565(0, 0, 0),       RGB565(25, 31, 28),     RGB565(87, 214, 148),   RGB565(41, 255, 144)},
    {"ORG1", RGB565(0, 0, 0),       RGB565(56, 42, 20),     RGB565(224, 145, 50),   RGB565(255, 165, 57)},
    {"ORG2", RGB565(255, 157, 43),  RGB565(245, 147, 33),   RGB565(36, 12, 0),      RGB565(0, 0, 0)},    
    {"VFD",  RGB565(0, 0, 0),       RGB565(10, 29, 33),     RGB565(132, 189, 160),  RGB565(175, 255, 215)},
    {"LCD1", RGB565(205, 255, 212), RGB565(185, 235, 192),  RGB565(82, 86, 88),     RGB565(10, 20, 20)}, 
    {"LCD2", RGB565(255, 255, 255), RGB565(240, 240, 240),  RGB565(30, 30, 30),     RGB565(0, 0, 0)},
    {"LCD3", RGB565(0, 0, 0),       RGB565(35, 35, 35),     RGB565(230, 230, 230),  RGB565(255, 255, 255)},
};
uint8_t themeCount = (sizeof(themes) / sizeof(themes[0]));

#endif // CONFIG_H
