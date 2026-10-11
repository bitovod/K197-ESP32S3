// ui.h
// The user interface layer on top of the display: which screen is shown,
// what the front-panel buttons do, and the text the screens contain.
// New in v8: the quick statistics screen (STO button); v12 the
// help (REL held); v14 the graph screen and its settings (RCL, graph.h).
//
// Threading, deliberately simple: everything except
// uiUpdateLiveReading() runs in ONE task, the display render/UI task on
// core 1 (display.ino) - button events are taken there, statistics are
// computed there, the scene is filled there. uiUpdateLiveReading() is
// the one entry point for another task (the DMM capture task on core 0)
// and only stores a few bytes under a spinlock.

#ifndef UI_H
#define UI_H

#include <Arduino.h>
#include "keithley_shapes.h"
#include "framebuffer.h" // FbStatsScene

// Which screen is currently shown.
#define UI_SCREEN_LCD         0 // the emulated K197 LCD (default)
#define UI_SCREEN_QUICKSTATS  1 // live value + AVG/MED/MAX/MIN
#define UI_SCREEN_HELP        2 // v12: version/build preface + help_text.h, paged with REL/dB
#define UI_SCREEN_GRAPH       3 // v14: graph of the log (RCL on the LCD screen), graph.h
#define UI_SCREEN_GRAPH_SETUP 4 // v14: settings of the graph (RCL held on the graph)
#define UI_SCREEN_DMM_SETUP   5 // v15: display settings - language, theme, sound; v23: SD card, CSV (RCL held on the LCD)
#define UI_SCREEN_ALARM_SETUP 6 // v17: limit alarm of the function being measured (dB held on the LCD), alarm.h

// Call once from setup() before the display tasks start.
void uiBegin();

// Called by the DMM capture task (dmm_bridge.ino) for every decoded
// frame, right after the datalog call. Cheap: it only remembers the
// reading; formatting and statistics happen later in the UI task.
void uiUpdateLiveReading(const char digits[KEITHLEY_DIGIT_COUNT], int8_t dpPos,
                         uint16_t indicatorBits, uint8_t unit);

// Called by the render/UI task every few milliseconds: takes button
// events, switches screens, refreshes the texts of the active screen.
// Returns true if the picture needs to be re-rendered.
bool uiPoll();

// v22: back to the main LCD screen on the next uiPoll() (from any task, not
// from an interrupt) - the K197 was switched off ("oFF197", dmm_bridge.ino),
// or the ESP32 is about to sleep (power.ino).
void uiRequestHome();

uint8_t uiGetScreen();

// Fills `scene` with the current quick statistics texts (what the last
// uiPoll() prepared). Same task as uiPoll().
void uiFillStatsScene(FbStatsScene *scene);

// Fills `scene` with the lines of the help screen currently shown (what
// the last uiPoll() selected). Same task as uiPoll().
void uiFillHelpScene(FbHelpScene *scene);

// Help screen position (for tests/debug): index of the first line shown,
// and the number of lines of the whole help (preface + text).
uint16_t uiGetHelpTop();
uint16_t uiGetHelpLineCount();
// v15: the help text shown (HELP_SECTION_* of help_text.h), chosen by the
// screen the help was opened from (LCD -> DMM, STATS -> STAT, GRAPH -> GRAPH).
uint8_t uiGetHelpSection();

// v14: the graph screen's scene (prepared by the last uiPoll(), graph.ino) and
// the graph settings screen (same 6-line layout as the help). Same task as
// uiPoll(). uiGetGraphSetupRow(): the line the cursor is on (GRAPH_SETUP_ROW_*).
const FbGraphScene &uiGetGraphScene();
void uiFillGraphSetupScene(FbSetupScene *scene);
uint8_t uiGetGraphSetupRow();

// v15: the DMM settings screen (RCL held CONFIG_UI_DMM_SETUP_HOLD_MS on the
// LCD): language, theme and sound, the cursor line uiGetDmmSetupRow()
// (UI_DMM_SETUP_ROW_*). RCL = next line, REL/dB = change, STO = back to the
// LCD. Changes apply at once and are saved deferred (settings.h).
#define UI_DMM_SETUP_ROW_LANGUAGE 0
#define UI_DMM_SETUP_ROW_THEME    1
#define UI_DMM_SETUP_ROW_SOUND    2 // v23: ALL / ALARMS ONLY / OFF
#define UI_DMM_SETUP_ROW_SD       3 // v23: SD card logging on/off (+ the state of the card)
#define UI_DMM_SETUP_ROW_CSV      4 // v23: CSV format of the SD files
#define UI_DMM_SETUP_ROW_HOLD     5 // v24: what STO held starts - HOLD (manual) or AUTO HOLD
#define UI_DMM_SETUP_ROWS         6 // v23: more than FB_SETUP_ROWS - the screen scrolls
void uiFillDmmSetupScene(FbSetupScene *scene);
uint8_t uiGetDmmSetupRow();
uint8_t uiGetDmmSetupTop(); // v23: first line shown

// v17: the alarm settings screen (dB held CONFIG_UI_ALARM_HOLD_MS on the LCD)
// for the function measured when it was opened (uiGetAlarmSetupFunction(),
// ALARM_FUNCTION_*): the mode, the two limits and the signal. RCL = next line
// (at the release), on a limit line RCL held CONFIG_UI_ALARM_TAKE_HOLD_MS =
// take the live reading; REL/dB = change (on a limit line one count of the
// last digit, held: repeating and accelerating); STO = back to the LCD.
// Applies at once, saved deferred.
#define UI_ALARM_SETUP_ROW_MODE   0
#define UI_ALARM_SETUP_ROW_LOWER  1
#define UI_ALARM_SETUP_ROW_UPPER  2
#define UI_ALARM_SETUP_ROW_SIGNAL 3
#define UI_ALARM_SETUP_ROWS       4
void uiFillAlarmSetupScene(FbSetupScene *scene);
uint8_t uiGetAlarmSetupRow();
uint8_t uiGetAlarmSetupFunction();

// v16 (SYSTem:DIAGnostic?): last and longest duration of a statistics job
// (start to result, milliseconds) and the records the last one used; read
// from the SCPI task (plain 32-bit values, at worst one job stale).
// uiDiagClear() forgets the longest one (SYSTem:DIAGnostic:CLEar).
void uiGetStatsTiming(uint32_t *lastMs, uint32_t *maxMs, uint32_t *lastRecords);
void uiDiagClear();

// The interval the statistics are computed over: 0 = last 5 seconds,
// 1 = 10 seconds, 2 = 1 minute, 3 = 5 minutes, 4 = 15 minutes, 5 = 1 hour,
// 6 = 6 hours. On the statistics screen REL steps to a shorter and dB to a
// longer interval (v12: REL acts at the release of a short press, since
// holding it opens the help). Out-of-range values are clamped. uiSetStatsInterval()
// also persists the choice (deferred EEPROM write, settings.h); the value
// is restored by uiBegin(). The SCPI side of this setting does not exist yet. (The whole log is deliberately not offered
// here - that is for a separate screen with graphs. Six hours is at most
// about 75600 records, worked off in the background in slices.)
#define UI_INTERVAL_COUNT 7
uint8_t uiGetStatsInterval();
void uiSetStatsInterval(uint8_t index);

// v25: the page of the statistics screen (FB_STATS_PAGE_*, framebuffer.h):
// 1 AVG MAX / MED MIN, 2 SDV DIF / REL PTS, 3 SEM TRD / SDV(dgt) DIF(ppm or %).
// An RCL click (released before CONFIG_UI_LOG_CLEAR_HOLD_MS) shows the next
// one, cyclically; the page stays while the interval changes and while the
// help is open, the statistics opened anew start on page 1.
uint8_t uiGetStatsPage();

// True while the UI wants to be polled again as soon as possible (a
// statistics job is running and needs its next slice) instead of waiting
// for the next regular poll. The render/UI task uses it to shorten its
// wait.
bool uiBusy();

#endif // UI_H
