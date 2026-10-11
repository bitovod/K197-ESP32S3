// settings.ino
// See settings.h. Backed by the Arduino EEPROM library, which on the
// ESP32 core emulates EEPROM byte access over a small flash
// partition: EEPROM.begin(size) loads the whole blob into a RAM
// buffer, EEPROM.put()/.get() only touch that RAM copy, and
// EEPROM.commit() is what actually writes it back to flash.
//
// Two ways a change reaches flash:
// - The SYSTem:* setters (called from the SCPI side) save at once,
//   one put()+commit() per call - these only change when someone
//   sends a configuration command, so the cost does not matter.
// - Settings changed from the front panel (the statistics interval,
//   v14 the graph settings, v15 theme/beep/language from the DMM settings
//   screen) are saved DEFERRED: the setter only updates the RAM
//   value and remembers when it changed; settingsPoll() (comm task,
//   core 1) commits once the value has been left alone for
//   CONFIG_SETTINGS_DEFERRED_SAVE_MS. Stepping through the intervals
//   with REL/dB therefore costs one flash write, not one per press,
//   and the flash write never runs in the render/UI task.
//
// Every save writes a snapshot of the WHOLE struct, so an immediate
// SCPI save also takes any pending deferred change with it (and
// clears the pending flag). A save whose snapshot equals what was last
// committed is skipped (e.g. 1 MIN -> 5 MIN -> 1 MIN within the delay).
//
// Concurrency: the UI task (core 1, prio 2) and the comm task (core 1,
// prio 1) both touch `settings`. A spinlock guards the RAM copy and the
// pending flag; the slow EEPROM.put()/commit() run outside it on a
// private snapshot, so the UI task never waits on flash.

#include "settings.h"
#include "config.h" // CONFIG_DEFAULT_* first-boot values, CONFIG_SETTINGS_DEFERRED_SAVE_MS
#include <EEPROM.h>
#include <math.h> // NAN (v17: alarm limits not set)

// KeithleySettings and SETTINGS_MAGIC live in settings.h: Arduino generates
// prototypes for .ino functions at the top of the merged file, so a struct
// used as a parameter type (settingsWriteSnapshot) must come from a header.

static KeithleySettings settings;
static KeithleySettings settingsCommitted; // what flash holds (as far as we know)
static portMUX_TYPE settingsMux = portMUX_INITIALIZER_UNLOCKED;
static bool settingsPending = false;       // a deferred change waits for its commit
static uint32_t settingsPendingSinceMs = 0; // millis() of the LAST deferred change

// Writes `snap` to flash unless it equals what is already there.
// Runs outside the spinlock (flash writes take milliseconds).
static void settingsWriteSnapshot(const KeithleySettings &snap)
{
    if (memcmp(&snap, &settingsCommitted, sizeof(KeithleySettings)) == 0)
    {
        return;
    }
    EEPROM.put(0, snap);
    EEPROM.commit();
    settingsCommitted = snap;
}

// Immediate save (SCPI setters, settingsBegin). Takes any pending
// deferred change along.
static void settingsSave()
{
    KeithleySettings snap;
    portENTER_CRITICAL(&settingsMux);
    snap = settings;
    settingsPending = false;
    portEXIT_CRITICAL(&settingsMux);
    settingsWriteSnapshot(snap);
}

// Deferred save: only marks the change, settingsPoll() commits it.
// Caller must hold settingsMux.
static void settingsMarkPendingLocked()
{
    settingsPending = true;
    settingsPendingSinceMs = (uint32_t)millis();
}

// v14: first-boot values of the graph settings (config.h).
static void settingsGraphDefaults()
{
    settings.graphInterval = CONFIG_GRAPH_DEFAULT_INTERVAL_INDEX;
    settings.graphYMode = CONFIG_GRAPH_DEFAULT_YMODE;
    settings.graphTrace = CONFIG_GRAPH_DEFAULT_TRACE;
    settings.graphSpan = CONFIG_GRAPH_DEFAULT_SPAN_INDEX;
}

// v17: first-boot values of the limit alarm: no limits, mode BOTH (what
// turning the alarm on from SCPI without a mode uses), SIGNAL from config.h.
static void settingsAlarmDefaults()
{
    for (uint8_t f = 0; f < SETTINGS_ALARM_FUNCTIONS; f++)
    {
        settings.alarmMode[f] = 3; // ALARM_MODE_BOTH (alarm.h - not included here)
        settings.alarmLow[f] = NAN;
        settings.alarmHigh[f] = NAN;
    }
    settings.alarmSignal = CONFIG_ALARM_DEFAULT_SIGNAL;
}

void settingsBegin()
{
    EEPROM.begin(sizeof(KeithleySettings));
    EEPROM.get(0, settings);
    // Forces the first settingsSave() below to really write.
    memset(&settingsCommitted, 0xFF, sizeof(settingsCommitted));
    if (settings.magic == SETTINGS_MAGIC_V9 || settings.magic == SETTINGS_MAGIC_V11 || settings.magic == SETTINGS_MAGIC_V13 ||
        settings.magic == SETTINGS_MAGIC_V14 || settings.magic == SETTINGS_MAGIC_V15 || settings.magic == SETTINGS_MAGIC_V16 ||
        settings.magic == SETTINGS_MAGIC_V17 || settings.magic == SETTINGS_MAGIC_V21)
    {
        // Firmware update: keep everything, give the fields added since
        // their defaults (each step adds what its layout lacked).
        if (settings.magic == SETTINGS_MAGIC_V9)
        {
            settings.statsInterval = CONFIG_UI_STATS_DEFAULT_INTERVAL_INDEX;
        }
        if (settings.magic == SETTINGS_MAGIC_V9 || settings.magic == SETTINGS_MAGIC_V11)
        {
            settings.debugPort = CONFIG_DEFAULT_DEBUG_PORT;
        }
        bool hasGraph = settings.magic == SETTINGS_MAGIC_V14 || settings.magic == SETTINGS_MAGIC_V15 ||
                        settings.magic == SETTINGS_MAGIC_V16 || settings.magic == SETTINGS_MAGIC_V17;
        if (settings.magic == SETTINGS_MAGIC_V14 || settings.magic == SETTINGS_MAGIC_V15)
        {
            // same interval as before, under its new index (10 SEC and 30 SEC were added in front)
            settings.graphInterval = (uint8_t)(settings.graphInterval + GRAPH_INTERVALS_ADDED_V15);
        }
        if (hasGraph)
        {
            // v21: same SUPP span as before, under its new index (+-1, +-2, +-5 were added in front)
            settings.graphSpan = (uint8_t)(settings.graphSpan + GRAPH_SPANS_ADDED_V21);
        }
        else if (settings.magic != SETTINGS_MAGIC_V21)
        {
            settingsGraphDefaults();
        }
        if (settings.magic != SETTINGS_MAGIC_V15 && settings.magic != SETTINGS_MAGIC_V16 && settings.magic != SETTINGS_MAGIC_V17 &&
            settings.magic != SETTINGS_MAGIC_V21)
        {
            settings.language = CONFIG_DEFAULT_LANGUAGE;
        }
        if (settings.magic != SETTINGS_MAGIC_V17 && settings.magic != SETTINGS_MAGIC_V21)
        {
            settingsAlarmDefaults(); // v17: every older layout lacks the alarm
        }
        settings.rtcAging = 0; // v22: every older layout lacks it - the chip keeps its own value
        settings.rtcAgingSet = 0;
        settings.magic = SETTINGS_MAGIC;
        settingsSave();
    }
    else if (settings.magic != SETTINGS_MAGIC)
    {
        // First boot, or a firmware/struct version we don't
        // recognize - load sane defaults and write them out so this
        // branch isn't hit again next boot.
        memset(&settings, 0, sizeof(settings));
        settings.magic = SETTINGS_MAGIC;
        settings.readingFormat = CONFIG_DEFAULT_READING_FORMAT;
        settings.autoEnabled = CONFIG_DEFAULT_AUTO_ENABLED ? 1 : 0;
        settings.autoIntervalSeconds = CONFIG_DEFAULT_AUTO_INTERVAL;
        settings.theme = CONFIG_DEFAULT_THEME;
        settings.debugEnabled = CONFIG_DEFAULT_DEBUG_ENABLED ? 1 : 0;
        settings.beepEnabled = CONFIG_DEFAULT_BEEP_ENABLED ? 1 : 0;
        settings.commPort = CONFIG_DEFAULT_COMM_PORT;
        settings.displayYOffset = CONFIG_DEFAULT_DISPLAY_Y_OFFSET;
        settings.statsInterval = CONFIG_UI_STATS_DEFAULT_INTERVAL_INDEX;
        settings.debugPort = CONFIG_DEFAULT_DEBUG_PORT;
        settingsGraphDefaults();
        settings.language = CONFIG_DEFAULT_LANGUAGE;
        settingsAlarmDefaults();
        settings.rtcAging = 0;
        settings.rtcAgingSet = 0;
        settingsSave();
    }
    else
    {
        // Valid content: this is exactly what flash holds.
        settingsCommitted = settings;
    }
}

void settingsPoll()
{
    KeithleySettings snap;
    bool due = false;
    portENTER_CRITICAL(&settingsMux);
    // Unsigned difference: correct across the millis() wrap-around.
    if (settingsPending &&
        (uint32_t)((uint32_t)millis() - settingsPendingSinceMs) >= (uint32_t)CONFIG_SETTINGS_DEFERRED_SAVE_MS)
    {
        snap = settings;
        settingsPending = false;
        due = true;
    }
    portEXIT_CRITICAL(&settingsMux);
    if (due)
    {
        settingsWriteSnapshot(snap);
    }
}

bool settingsSavePending()
{
    portENTER_CRITICAL(&settingsMux);
    bool pending = settingsPending;
    portEXIT_CRITICAL(&settingsMux);
    return pending;
}

uint8_t settingsGetReadingFormat()
{
    return settings.readingFormat;
}

void settingsSetReadingFormat(uint8_t format)
{
    portENTER_CRITICAL(&settingsMux);
    settings.readingFormat = format;
    portEXIT_CRITICAL(&settingsMux);
    settingsSave();
}

bool settingsGetAutoEnabled()
{
    return settings.autoEnabled != 0;
}

uint16_t settingsGetAutoIntervalSeconds()
{
    return settings.autoIntervalSeconds;
}

void settingsSetAuto(bool enabled, uint16_t intervalSeconds)
{
    portENTER_CRITICAL(&settingsMux);
    settings.autoEnabled = enabled ? 1 : 0;
    settings.autoIntervalSeconds = intervalSeconds;
    portEXIT_CRITICAL(&settingsMux);
    settingsSave();
}

uint8_t settingsGetTheme()
{
    return settings.theme;
}

void settingsSetTheme(uint8_t theme)
{
    portENTER_CRITICAL(&settingsMux);
    settings.theme = theme;
    portEXIT_CRITICAL(&settingsMux);
    settingsSave();
}

bool settingsGetDebugEnabled()
{
    return settings.debugEnabled != 0;
}

void settingsSetDebugEnabled(bool enabled)
{
    portENTER_CRITICAL(&settingsMux);
    settings.debugEnabled = enabled ? 1 : 0;
    portEXIT_CRITICAL(&settingsMux);
    settingsSave();
}

bool settingsGetBeepEnabled()
{
    return settings.beepEnabled != 0;
}

void settingsSetBeepEnabled(bool enabled)
{
    portENTER_CRITICAL(&settingsMux);
    settings.beepEnabled = enabled ? 1 : 0;
    portEXIT_CRITICAL(&settingsMux);
    settingsSave();
}

uint8_t settingsGetCommPort()
{
    return settings.commPort;
}

void settingsSetCommPort(uint8_t port)
{
    portENTER_CRITICAL(&settingsMux);
    settings.commPort = port;
    portEXIT_CRITICAL(&settingsMux);
    settingsSave();
}

int16_t settingsGetDisplayYOffset()
{
    return settings.displayYOffset;
}

void settingsSetDisplayYOffset(int16_t offset)
{
    portENTER_CRITICAL(&settingsMux);
    settings.displayYOffset = offset;
    portEXIT_CRITICAL(&settingsMux);
    settingsSave();
}

uint8_t settingsGetStatsInterval()
{
    return settings.statsInterval;
}

void settingsSetStatsIntervalDeferred(uint8_t index)
{
    portENTER_CRITICAL(&settingsMux);
    settings.statsInterval = index;
    settingsMarkPendingLocked(); // restarts the delay on every change
    portEXIT_CRITICAL(&settingsMux);
}

uint8_t settingsGetDebugPort()
{
    return settings.debugPort;
}

void settingsSetDebugPort(uint8_t port)
{
    portENTER_CRITICAL(&settingsMux);
    settings.debugPort = port;
    portEXIT_CRITICAL(&settingsMux);
    settingsSave();
}

uint8_t settingsGetGraphInterval()
{
    return settings.graphInterval;
}

uint8_t settingsGetGraphYMode()
{
    return settings.graphYMode;
}

uint8_t settingsGetGraphTrace()
{
    return settings.graphTrace;
}

uint8_t settingsGetGraphSpan()
{
    return settings.graphSpan;
}

void settingsSetGraphDeferred(uint8_t interval, uint8_t yMode, uint8_t trace, uint8_t span)
{
    portENTER_CRITICAL(&settingsMux);
    settings.graphInterval = interval;
    settings.graphYMode = yMode;
    settings.graphTrace = trace;
    settings.graphSpan = span;
    settingsMarkPendingLocked(); // restarts the delay on every change
    portEXIT_CRITICAL(&settingsMux);
}

uint8_t settingsGetLanguage()
{
    return settings.language;
}

int8_t settingsGetRtcAging(bool *isSet)
{
    if (isSet)
    {
        *isSet = settings.rtcAgingSet != 0;
    }
    return settings.rtcAging;
}

void settingsSetRtcAging(int8_t aging)
{
    portENTER_CRITICAL(&settingsMux);
    settings.rtcAging = aging;
    settings.rtcAgingSet = 1;
    portEXIT_CRITICAL(&settingsMux);
    settingsSave();
}

void settingsSetLanguage(uint8_t language)
{
    portENTER_CRITICAL(&settingsMux);
    settings.language = language;
    portEXIT_CRITICAL(&settingsMux);
    settingsSave();
}

void settingsSetThemeDeferred(uint8_t theme)
{
    portENTER_CRITICAL(&settingsMux);
    settings.theme = theme;
    settingsMarkPendingLocked();
    portEXIT_CRITICAL(&settingsMux);
}

void settingsSetBeepEnabledDeferred(bool enabled)
{
    portENTER_CRITICAL(&settingsMux);
    settings.beepEnabled = enabled ? 1 : 0;
    settingsMarkPendingLocked();
    portEXIT_CRITICAL(&settingsMux);
}

void settingsSetLanguageDeferred(uint8_t language)
{
    portENTER_CRITICAL(&settingsMux);
    settings.language = language;
    settingsMarkPendingLocked();
    portEXIT_CRITICAL(&settingsMux);
}

void settingsSetGraph(uint8_t interval, uint8_t yMode, uint8_t trace, uint8_t span)
{
    portENTER_CRITICAL(&settingsMux);
    settings.graphInterval = interval;
    settings.graphYMode = yMode;
    settings.graphTrace = trace;
    settings.graphSpan = span;
    portEXIT_CRITICAL(&settingsMux);
    settingsSave();
}

// ---- v17: the limit alarm ----

void settingsGetAlarm(uint8_t function, uint8_t *mode, float *low, float *high)
{
    if (function >= SETTINGS_ALARM_FUNCTIONS)
    {
        *mode = 0;
        *low = NAN;
        *high = NAN;
        return;
    }
    portENTER_CRITICAL(&settingsMux);
    *mode = settings.alarmMode[function];
    *low = settings.alarmLow[function];
    *high = settings.alarmHigh[function];
    portEXIT_CRITICAL(&settingsMux);
}

// Caller must hold settingsMux.
static void settingsPutAlarmLocked(uint8_t function, uint8_t mode, float low, float high)
{
    settings.alarmMode[function] = mode;
    settings.alarmLow[function] = low;
    settings.alarmHigh[function] = high;
}

void settingsSetAlarm(uint8_t function, uint8_t mode, float low, float high)
{
    if (function >= SETTINGS_ALARM_FUNCTIONS)
    {
        return;
    }
    portENTER_CRITICAL(&settingsMux);
    settingsPutAlarmLocked(function, mode, low, high);
    portEXIT_CRITICAL(&settingsMux);
    settingsSave();
}

void settingsSetAlarmDeferred(uint8_t function, uint8_t mode, float low, float high)
{
    if (function >= SETTINGS_ALARM_FUNCTIONS)
    {
        return;
    }
    portENTER_CRITICAL(&settingsMux);
    settingsPutAlarmLocked(function, mode, low, high);
    settingsMarkPendingLocked();
    portEXIT_CRITICAL(&settingsMux);
}

uint8_t settingsGetAlarmSignal()
{
    return settings.alarmSignal;
}

void settingsSetAlarmSignal(uint8_t signal)
{
    portENTER_CRITICAL(&settingsMux);
    settings.alarmSignal = signal;
    portEXIT_CRITICAL(&settingsMux);
    settingsSave();
}

void settingsSetAlarmSignalDeferred(uint8_t signal)
{
    portENTER_CRITICAL(&settingsMux);
    settings.alarmSignal = signal;
    settingsMarkPendingLocked();
    portEXIT_CRITICAL(&settingsMux);
}
