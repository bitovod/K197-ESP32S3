// settings.h
// Persistent settings for the SCPI interface (serial_out.ino),
// stored in the ESP32's emulated EEPROM (the standard Arduino EEPROM
// library, which on ESP32 emulates classic EEPROM byte access over a
// small flash partition) so they survive a power cycle.
//
// Values here mirror the SYSTem:READing / SYSTem:AUTO / SYSTem:THEMe /
// SYSTem:DEBUG SCPI commands - serial_out.ino calls the setters below
// whenever one of those commands changes something, which both
// updates the live value and writes it to flash in the same call.
// Nothing here is on any performance-critical path (these change only
// when someone sends a configuration command, not per measurement),
// so a flash write per call is an acceptable cost - see settings.ino.
// Exception: settings changed from the front panel are saved deferred
// (settingsSetStatsIntervalDeferred() + settingsPoll()), see below.

#ifndef SETTINGS_H
#define SETTINGS_H

#include <Arduino.h>

// SYSTem:READing values - selects how READ?/auto-send format the
// current reading. See serial_out.ino for exactly what each produces.
#define SETTINGS_READING_RAW      0 // unmodified, as the DMM sends it
#define SETTINGS_READING_PREFIXED 1 // function prefix + scientific notation
#define SETTINGS_READING_NUMERIC  2 // scientific notation only, no prefix

// SYSTem:COMM values - which physical port the SCPI interface talks
// on. See serial_out.ino.
#define SETTINGS_COMM_UART 0
#define SETTINGS_COMM_USB  1

// The persisted blob (layout of the emulated EEPROM). Internal to
// settings.ino - it is here only because of the Arduino prototype
// ordering (see settings.ino); use the getters/setters below.
// Bumped if KeithleySettings' layout ever changes, so a firmware
// update with a different struct doesn't misinterpret old bytes left
// over in flash from a previous version - settingsBegin() falls back
// to defaults whenever the stored magic is not recognized.
// History: 0xA5 -> 0xA6 debugEnabled, 0xA6 -> 0xA7 beepEnabled,
// 0xA7 -> 0xA8 commPort, 0xA8 -> 0xA9 displayYOffset,
// 0xA9 -> 0xAA statsInterval, 0xAA -> 0xAB debugPort (v13), 0xAB -> 0xAC the
// graph settings (v14), 0xAC -> 0xAD language (v15), 0xAE -> 0xAF the limit
// alarm (v17).
// From 0xAA on a new field is APPENDED and the previous magic is
// migrated (old fields kept, the new one gets its default) instead of
// resetting everything - so a firmware update does not lose the
// already dialed-in YOFFset/COMM/THEMe.
#define SETTINGS_MAGIC 0xAF
#define SETTINGS_MAGIC_V9 0xA9  // layout up to and including displayYOffset
#define SETTINGS_MAGIC_V11 0xAA // layout up to and including statsInterval
#define SETTINGS_MAGIC_V13 0xAB // layout up to and including debugPort
#define SETTINGS_MAGIC_V14 0xAC // layout up to and including graphSpan
#define SETTINGS_MAGIC_V15 0xAD // same layout as 0xAE; graphInterval counted without 10 SEC / 30 SEC
#define SETTINGS_MAGIC_V16 0xAE // layout up to and including language
// 0xAD -> 0xAE (v15): no new field - the graph got the intervals 10 SEC and 30 SEC IN FRONT of the
// others, so a stored graphInterval of 0xAC/0xAD is moved up by GRAPH_INTERVALS_ADDED_V15.
#define GRAPH_INTERVALS_ADDED_V15 2

// v17: one alarm configuration per measurement function, in the order of
// alarm.h (DCV, ACV, DCI, ACI, OHM, DB) - static_assert there.
#define SETTINGS_ALARM_FUNCTIONS 6

struct KeithleySettings
{
    uint8_t magic;
    uint8_t readingFormat;
    uint8_t autoEnabled;
    uint16_t autoIntervalSeconds;
    uint8_t theme;
    uint8_t debugEnabled;
    uint8_t beepEnabled;
    uint8_t commPort;
    int16_t displayYOffset;
    // ---- appended in 0xAA (offsets of the fields above unchanged) ----
    uint8_t statsInterval;
    // ---- appended in 0xAB ----
    uint8_t debugPort; // SETTINGS_COMM_UART / SETTINGS_COMM_USB
    // ---- appended in 0xAC (v14, graph screen - see graph.h) ----
    uint8_t graphInterval; // GRAPH_INTERVAL_* index
    uint8_t graphYMode;    // GRAPH_YMODE_*
    uint8_t graphTrace;    // GRAPH_TRACE_*
    uint8_t graphSpan;     // index of the SUPP span
    // ---- appended in 0xAD (v15) ----
    uint8_t language;      // HELP_LANG_* (help_text.h) - SYSTem:LANGuage
    // ---- appended in 0xAF (v17, limit alarm - see alarm.h) ----
    uint8_t alarmMode[SETTINGS_ALARM_FUNCTIONS]; // remembered ALARM_MODE_UPPER/LOWER/BOTH per function
    uint8_t alarmSignal;                         // ALARM_SIGNAL_*
    float alarmLow[SETTINGS_ALARM_FUNCTIONS];    // base units (V, A, Ohm, dB), NAN = not set
    float alarmHigh[SETTINGS_ALARM_FUNCTIONS];
};

// Call once from setup(), before anything else here is used, and
// before serialOutBegin() (which reads the loaded values to configure
// itself). Loads settings from EEPROM, or writes out defaults if none
// were stored yet (first boot, or the stored data doesn't look like
// it belongs to this project/version).
void settingsBegin();

uint8_t settingsGetReadingFormat();
void settingsSetReadingFormat(uint8_t format);

// autoIntervalSeconds: 0 = send on every DMM measurement, N = send
// every N seconds. enabled=false pauses sending without forgetting
// the configured interval - see SYSTem:AUTO in serial_out.ino for the
// exact ON/OFF/0/NNN command grammar this backs.
bool settingsGetAutoEnabled();
uint16_t settingsGetAutoIntervalSeconds();
void settingsSetAuto(bool enabled, uint16_t intervalSeconds);

uint8_t settingsGetTheme();
void settingsSetTheme(uint8_t theme);

// SYSTem:DEBUG - see serial_out.ino. Persisted so a debugging session
// that needs frequent restarts doesn't lose the trace right when it
// matters most (the very first frames after power-up) - unlike the
// other three settings above, this one started out deliberately NOT
// persisted (a debug trace silently surviving a power cycle seemed
// like the wrong default), but got moved here once that trade-off
// turned out to be the wrong call in practice.
bool settingsGetDebugEnabled();
void settingsSetDebugEnabled(bool enabled);

// SYSTem:BEEP - see serial_out.ino/beeper.ino. Gates every acoustic
// signal (currently just the startup chirp) - persisted so the
// choice survives a power cycle, same reasoning as debugEnabled
// above.
bool settingsGetBeepEnabled();
void settingsSetBeepEnabled(bool enabled);

// SYSTem:COMM - which physical port (UART/USB, see SETTINGS_COMM_*
// above) the SCPI interface talks on. See serial_out.ino - switching
// this takes effect immediately, no reboot needed.
uint8_t settingsGetCommPort();
void settingsSetCommPort(uint8_t port);

// SYSTem:YOFFset - see display.h/serial_out.ino. Persisted so the
// real-hardware mounting correction only ever needs to be dialed in
// once per physical unit.
int16_t settingsGetDisplayYOffset();
void settingsSetDisplayYOffset(int16_t offset);

// Statistics interval index (ui.h, UI_INTERVAL_COUNT) chosen with
// REL/dB on the statistics screen. Persisted with a DEFERRED write: the
// setter only changes the RAM value, settingsPoll() commits it once it
// has not changed for CONFIG_SETTINGS_DEFERRED_SAVE_MS (config.h). A
// power-off inside that window keeps the previous value. The stored
// value is not range-checked here - ui.ino clamps it.
uint8_t settingsGetStatsInterval();
void settingsSetStatsIntervalDeferred(uint8_t index);

// Call often from ONE task (core1Task(), the comm task): commits a
// deferred change once it is due. Cheap when nothing is pending.
void settingsPoll();

// True while a deferred change has not been written yet (tests/debug).
bool settingsSavePending();

// SYSTem:DEBUG:PORT (v13) - where debug messages (debuglog.h: the frame
// trace of SYSTem:DEBUG ON and the always-on error/boot messages) go: the
// USB CDC port of the ESP32 or the SCPI UART (CH340 on the rear panel,
// stays connected while the instrument is switched off). Persisted, so the
// messages of the very first seconds after power-up land where the PC is
// already listening. Saved at once (SCPI command).
uint8_t settingsGetDebugPort();
void settingsSetDebugPort(uint8_t port);

// v14: the graph screen's settings (graph.ino), changed from the front panel
// and therefore saved DEFERRED like the statistics interval. The values are
// stored as given; graph.ino clamps them when it loads them.
uint8_t settingsGetGraphInterval();
uint8_t settingsGetGraphYMode();
uint8_t settingsGetGraphTrace();
uint8_t settingsGetGraphSpan();
void settingsSetGraphDeferred(uint8_t interval, uint8_t yMode, uint8_t trace, uint8_t span);
// v15: the same, saved AT ONCE - for the SCPI GRAPh:* commands (serial_out.ino). The graph adopts
// the new values on its next poll (graph.ino reads them back from here).
void settingsSetGraph(uint8_t interval, uint8_t yMode, uint8_t trace, uint8_t span);

// v15: language of the help and of the graph settings screen (HELP_LANG_*,
// help_text.h), set by SYSTem:LANGuage - saved at once like the other
// SYSTem:* settings. Stored as given; readers clamp it.
uint8_t settingsGetLanguage();
void settingsSetLanguage(uint8_t language);

// v15: the same three "look and feel" settings changed from the front panel
// (the DMM settings screen, RCL held on the LCD) - saved DEFERRED like the
// statistics interval and the graph settings; SCPI keeps the immediate
// setters above.
void settingsSetThemeDeferred(uint8_t theme);
void settingsSetBeepEnabledDeferred(bool enabled);
void settingsSetLanguageDeferred(uint8_t language);

// v17: the limit alarm (alarm.ino) - per function the remembered mode
// (UPPER/LOWER/BOTH; whether the alarm is ON is not persisted, it is always
// off after power-up) and the two limits in base units (NAN = not set), plus
// the global SIGNAL (beep + arrow / arrow only). Stored as given; alarm.ino
// clamps when it loads. The ...Deferred() variants are for the front panel,
// the others (SCPI CALCulate:LIMit:*) save at once. `function` out of range
// is ignored (getters return mode 0 and NAN).
void settingsGetAlarm(uint8_t function, uint8_t *mode, float *low, float *high);
void settingsSetAlarm(uint8_t function, uint8_t mode, float low, float high);
void settingsSetAlarmDeferred(uint8_t function, uint8_t mode, float low, float high);
uint8_t settingsGetAlarmSignal();
void settingsSetAlarmSignal(uint8_t signal);
void settingsSetAlarmSignalDeferred(uint8_t signal);

#endif // SETTINGS_H
