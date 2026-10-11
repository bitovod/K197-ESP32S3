// ui.ino
// See ui.h.

#include "ui.h"
#include "ui_format.h"
#include "buttons.h"
#include "datalog.h"
#include "reading_math.h"
#include "display.h"
#include "rtc.h"
#include "debuglog.h"
#include "settings.h"
#include "beeper.h"
#include "build_info.h"
#include "help_text.h"
#include "graph.h"
#include "alarm.h"
#include "marker.h"
#include "sdlog.h" // v23: SD card state on the DMM settings screen
#include "hold.h"  // v24: DATA HOLD (STO held on the LCD)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ---- the live reading, written by the capture task (core 0) ----

static portMUX_TYPE uiLiveMux = portMUX_INITIALIZER_UNLOCKED;
static char uiLiveDigits[KEITHLEY_DIGIT_COUNT] = {' ', ' ', ' ', ' ', ' ', ' '};
static int8_t uiLiveDp = -1;
static uint16_t uiLiveIndicators = 0;
static uint8_t uiLiveUnit = KEITHLEY_UNIT_NONE;
static volatile uint32_t uiLiveSeq = 0; // bumped for every stored reading; 0 = none yet

// ---- the statistics intervals ----

static const uint32_t uiIntervalSeconds[UI_INTERVAL_COUNT] = {5, 10, 60, 300, 900, 3600, 21600};
static const char *const uiIntervalLabels[UI_INTERVAL_COUNT] = {"5 SEC", "10 SEC", "1 MIN", "5 MIN", "15 MIN", "1 HOUR", "6 HRS"};

// ---- everything below belongs to the UI task alone ----

static uint8_t uiScreen = UI_SCREEN_LCD;
static uint8_t uiInterval = CONFIG_UI_STATS_DEFAULT_INTERVAL_INDEX;

// The texts of the quick statistics screen as last prepared. The live
// part follows every reading, the statistics part is the result of the
// last finished statistics job.
static FbStatsScene uiScene;
static uint32_t uiProcessedSeq = 0;
// v25: page of the statistics screen (FB_STATS_PAGE_*): RCL click cycles it, kept while the interval changes or the
// help is open, back to page 1 whenever the statistics are opened anew.
static uint8_t uiStatsPage = FB_STATS_PAGE_MAIN;

// The statistics job: a computation over the log that is advanced a
// slice per uiPoll() so that even the whole log (hundreds of thousands
// of records) never stalls the buttons or the picture. The "context" is
// everything the numbers depend on; when it changes the old numbers are
// wrong and the job starts over.
static DatalogStatsJob uiJob;
static bool uiJobRunning = false;
static bool uiHaveContext = false;
static char uiCtxFilter[8] = "";
static uint8_t uiCtxUnit = KEITHLEY_UNIT_NONE;
static uint8_t uiCtxDecimals = 0;
static uint8_t uiCtxInterval = 0;
static uint32_t uiStatsFinishedMs = 0;
// v16 (SYSTem:DIAGnostic?): duration of the statistics jobs, start to result
static uint32_t uiStatsStartMs = 0;
static uint32_t uiStatsLastMs = 0;
static uint32_t uiStatsMaxMs = 0;
static uint32_t uiStatsLastRecords = 0;

// RCL held on the statistics screen clears the log: armed by an RCL press
// that happened ON that screen, fired once the hold reaches
// CONFIG_UI_LOG_CLEAR_HOLD_MS, disarmed by the release, by firing, or by
// leaving the screen - so one hold clears exactly once.
static bool uiLogClearArmed = false;

// The help screen (v12): REL held on the LCD or statistics screen opens it.
// uiHelpArmed: a REL press that may become that hold (also what turns a
// short REL into a "click" on release). uiHelpTop: first line shown.
static bool uiHelpArmed = false;
static uint8_t uiHelpReturnScreen = UI_SCREEN_LCD;
static uint16_t uiHelpTop = 0;
// v13 help navigation: REL/dB act at the release of a short press, a hold of
// CONFIG_UI_HELP_HOLD_MS jumps to the start/end (while still held). Armed by
// a press that happened on the help screen.
static bool uiHelpRelArmed = false;
static bool uiHelpDbArmed = false;
// Last button activity on the help screen (press, or any button held) -
// after CONFIG_UI_HELP_TIMEOUT_MS without any, the help closes like STO.
static uint32_t uiHelpLastActivityMs = 0;
static uint16_t uiHelpTextLines = 0; // lines of the help text shown (help_text.h), counted when the help opens
// v15: which text is shown - chosen by the screen the help was opened from
// (HELP_SECTION_*) in the language of SYSTem:LANGuage, fixed while it is open.
static uint8_t uiHelpSection = HELP_SECTION_DMM;
static const char *uiHelpTextPtr = NULL;

// v14 graph screen: an RCL press on the graph is a click (released before
// CONFIG_GRAPH_SETUP_HOLD_MS: next Y axis mode) or a hold (settings screen).
// Armed by a press that happened on the graph screen.
static bool uiGraphRclArmed = false;
static bool uiGraphDbArmed = false; // v18: dB on the graph acts at the release (REL + dB = marker)
static bool uiChordActive = false;  // v18: REL + dB held together, until both are up again
static uint8_t uiGraphSetupRow = 0; // cursor line of the graph settings screen (kept while away)

// v15 LCD screen: an RCL press is a click (released before
// CONFIG_UI_DMM_SETUP_HOLD_MS: the graph) or a hold (the DMM settings).
static bool uiLcdRclArmed = false;
static uint8_t uiDmmSetupRow = 0; // cursor line of the DMM settings screen
static uint8_t uiDmmSetupTop = 0; // v23: first line shown (the list is longer than the screen)
static uint8_t uiDmmSdViewShown = 0xFE; // v23: SD state the DMM settings screen last showed
static bool uiLcdStoArmed = false;      // v24: STO pressed on the LCD - click (statistics / end HOLD) or hold (HOLD)
static bool uiLcdStoFrozen = false;     // v24: manual HOLD preview at the press (not confirmed yet)
// v15: last button activity on a settings screen (graph or DMM) - after
// CONFIG_UI_HELP_TIMEOUT_MS without any it returns to its parent screen,
// like the help closes.
static uint32_t uiSetupLastActivityMs = 0;

// v17 limit alarm: a dB press on the LCD screen that may become the hold
// opening the alarm settings (a shorter press is only the K197's dB, sent by
// buttons.ino at the release).
static bool uiLcdDbArmed = false;
static uint8_t uiAlarmSetupRow = 0;                        // cursor line of the alarm settings screen
static uint8_t uiAlarmFunction = ALARM_FUNCTION_NONE;      // the function the screen was opened for
static bool uiAlarmRclArmed = false;                       // RCL: next line at the release, or the take-over hold
static uint8_t uiAlarmRepeatButton = 0xFF;                 // REL/dB held on a limit line (BUTTON_*), 0xFF = none
static uint32_t uiAlarmRepeatNextMs = 0;                   // when the next repeated step is due
static uint16_t uiAlarmRepeatCount = 0;                    // repeated steps so far (sets the step size)

// Defined below uiSetStatsInterval(); declared here for uiBegin().
static void uiApplyStatsInterval(uint8_t index);

// Defined further below with the help screen; declared here for uiBegin().
static void uiHelpSelectText(uint8_t section);

void uiBegin()
{
    memset(&uiScene, 0, sizeof(uiScene));
    snprintf(uiScene.live, sizeof(uiScene.live), UI_PLACEHOLDER);
    snprintf(uiScene.average, sizeof(uiScene.average), UI_PLACEHOLDER);
    snprintf(uiScene.median, sizeof(uiScene.median), UI_PLACEHOLDER);
    snprintf(uiScene.maximum, sizeof(uiScene.maximum), UI_PLACEHOLDER);
    snprintf(uiScene.minimum, sizeof(uiScene.minimum), UI_PLACEHOLDER);
    // The interval chosen last time (settingsBegin() runs before uiBegin()).
    uiApplyStatsInterval(settingsGetStatsInterval());
    uiHelpSelectText(HELP_SECTION_DMM);
    graphBegin(); // v14: graph settings (settingsBegin() ran before)
}

void uiUpdateLiveReading(const char digits[KEITHLEY_DIGIT_COUNT], int8_t dpPos,
                         uint16_t indicatorBits, uint8_t unit)
{
    portENTER_CRITICAL(&uiLiveMux);
    for (uint8_t i = 0; i < KEITHLEY_DIGIT_COUNT; i++)
    {
        uiLiveDigits[i] = digits[i];
    }
    uiLiveDp = dpPos;
    uiLiveIndicators = indicatorBits;
    uiLiveUnit = unit;
    uiLiveSeq = uiLiveSeq + 1; // plain read + write: a compound/++ on a volatile is deprecated in C++20, and this is the only writer, inside the critical section
    portEXIT_CRITICAL(&uiLiveMux);
}

uint8_t uiGetScreen()
{
    return uiScreen;
}

uint8_t uiGetStatsInterval()
{
    return uiInterval;
}

// Changes the interval in RAM only (no persistence) - used at start-up
// with the value loaded from EEPROM.
static void uiApplyStatsInterval(uint8_t index)
{
    if (index >= UI_INTERVAL_COUNT)
    {
        index = UI_INTERVAL_COUNT - 1;
    }
    uiInterval = index;
    snprintf(uiScene.interval, sizeof(uiScene.interval), "%s", uiIntervalLabels[uiInterval]);
}

void uiSetStatsInterval(uint8_t index)
{
    uiApplyStatsInterval(index);
    // Persisted with a delay (settings.ino): the flash write happens in the
    // comm task once the value has been left alone for
    // CONFIG_SETTINGS_DEFERRED_SAVE_MS, never here in the UI task.
    settingsSetStatsIntervalDeferred(uiInterval);
}

void uiFillStatsScene(FbStatsScene *scene)
{
    *scene = uiScene;
}

uint8_t uiGetStatsPage()
{
    return uiStatsPage;
}

// v25: the units of pages 2 and 3 for a context, with no numbers (placeholders).
static void uiSetPlaceholdersExtra()
{
    const char *perTime = (uiCtxInterval <= CONFIG_UI_TREND_PER_MINUTE_MAX_INTERVAL) ? "/m" : "/h";
    uiFormatEngineering(uiScene.sdev, sizeof(uiScene.sdev), uiScene.sdevUnit, sizeof(uiScene.sdevUnit), NAN, uiCtxUnit,
                        false, "", UI_PLACEHOLDER);
    uiFormatRatio(uiScene.rel, sizeof(uiScene.rel), uiScene.relUnit, sizeof(uiScene.relUnit), NAN, UI_PLACEHOLDER);
    snprintf(uiScene.dif, sizeof(uiScene.dif), UI_PLACEHOLDER);
    snprintf(uiScene.pts, sizeof(uiScene.pts), UI_PLACEHOLDER);
    uiFormatEngineering(uiScene.sem, sizeof(uiScene.sem), uiScene.semUnit, sizeof(uiScene.semUnit), NAN, uiCtxUnit,
                        false, "", UI_PLACEHOLDER_SHORT);
    uiFormatEngineering(uiScene.trend, sizeof(uiScene.trend), uiScene.trendUnit, sizeof(uiScene.trendUnit), NAN,
                        uiCtxUnit, true, perTime, UI_PLACEHOLDER_SHORT);
    snprintf(uiScene.sdevDigits, sizeof(uiScene.sdevDigits), UI_PLACEHOLDER_SHORT);
    uiFormatRatio(uiScene.difRel, sizeof(uiScene.difRel), uiScene.difRelUnit, sizeof(uiScene.difRelUnit), NAN,
                  UI_PLACEHOLDER_SHORT);
}

static void uiSetPlaceholders()
{
    snprintf(uiScene.average, sizeof(uiScene.average), UI_PLACEHOLDER);
    snprintf(uiScene.median, sizeof(uiScene.median), UI_PLACEHOLDER);
    snprintf(uiScene.maximum, sizeof(uiScene.maximum), UI_PLACEHOLDER);
    snprintf(uiScene.minimum, sizeof(uiScene.minimum), UI_PLACEHOLDER);
    uiSetPlaceholdersExtra();
}

// Writes the time and date of the RTC into the scene ("14:03:07 06.10.26"),
// or a note if there is no RTC time. Returns true if the text changed.
static bool uiUpdateDateTime()
{
    char text[sizeof(uiScene.dateTime)];
    uint16_t year;
    uint8_t month, day, hour, minute, second;
    if (rtcGetDateTime(&year, &month, &day, &hour, &minute, &second))
    {
        snprintf(text, sizeof(text), "%02u:%02u:%02u %02u.%02u.%02u", (unsigned)hour, (unsigned)minute, (unsigned)second,
                 (unsigned)day, (unsigned)month, (unsigned)(year % 100));
    }
    else
    {
        snprintf(text, sizeof(text), "NO RTC TIME");
    }
    if (strcmp(text, uiScene.dateTime) == 0)
    {
        return false;
    }
    snprintf(uiScene.dateTime, sizeof(uiScene.dateTime), "%s", text);
    return true;
}

// Formats a finished job's result into the scene, in the unit and with
// the decimals the job was started for.
static void uiPublishStats()
{
    DatalogStats stats;
    bool found = datalogStatsFinish(&uiJob, &stats);
    uiJobRunning = false;
    uiStatsFinishedMs = millis();
    uiStatsLastMs = uiStatsFinishedMs - uiStatsStartMs;
    uiStatsLastRecords = found ? stats.count : 0;
    if (uiStatsLastMs > uiStatsMaxMs)
    {
        uiStatsMaxMs = uiStatsLastMs;
    }

    if (!found)
    {
        uiSetPlaceholders();
        return;
    }
    uiFormatNumber(uiScene.average, sizeof(uiScene.average), stats.average, uiCtxUnit, uiCtxDecimals);
    uiFormatNumber(uiScene.maximum, sizeof(uiScene.maximum), stats.maximum, uiCtxUnit, uiCtxDecimals);
    uiFormatNumber(uiScene.minimum, sizeof(uiScene.minimum), stats.minimum, uiCtxUnit, uiCtxDecimals);
    if (stats.medianValid)
    {
        uiFormatNumber(uiScene.median, sizeof(uiScene.median), stats.median, uiCtxUnit, uiCtxDecimals);
    }
    else
    {
        snprintf(uiScene.median, sizeof(uiScene.median), UI_PLACEHOLDER);
    }

    // v25 pages 2 and 3
    uiSetPlaceholdersExtra(); // units for the context, "no number" where the result has none
    bool relative = (uiCtxUnit != KEITHLEY_UNIT_DB && stats.average != 0.0); // a ratio to a dB level means nothing
    double spread = stats.maximum - stats.minimum;
    uiFormatNumber(uiScene.dif, sizeof(uiScene.dif), spread, uiCtxUnit, uiCtxDecimals);
    snprintf(uiScene.pts, sizeof(uiScene.pts), "%lu", (unsigned long)stats.count);
    if (relative)
    {
        uiFormatRatio(uiScene.difRel, sizeof(uiScene.difRel), uiScene.difRelUnit, sizeof(uiScene.difRelUnit),
                      spread / fabs(stats.average), UI_PLACEHOLDER_SHORT);
    }
    if (stats.sdevValid)
    {
        uiFormatEngineering(uiScene.sdev, sizeof(uiScene.sdev), uiScene.sdevUnit, sizeof(uiScene.sdevUnit), stats.sdev,
                            uiCtxUnit, false, "", UI_PLACEHOLDER);
        uiFormatEngineering(uiScene.sem, sizeof(uiScene.sem), uiScene.semUnit, sizeof(uiScene.semUnit),
                            stats.sdev / sqrt((double)stats.count), uiCtxUnit, false, "", UI_PLACEHOLDER_SHORT);
        uiFormatDigits(uiScene.sdevDigits, sizeof(uiScene.sdevDigits), stats.sdev, uiCtxUnit, uiCtxDecimals);
        if (relative)
        {
            uiFormatRatio(uiScene.rel, sizeof(uiScene.rel), uiScene.relUnit, sizeof(uiScene.relUnit),
                          stats.sdev / fabs(stats.average), UI_PLACEHOLDER);
        }
    }
    if (stats.slopeValid)
    {
        bool perMinute = (uiCtxInterval <= CONFIG_UI_TREND_PER_MINUTE_MAX_INTERVAL);
        uiFormatEngineering(uiScene.trend, sizeof(uiScene.trend), uiScene.trendUnit, sizeof(uiScene.trendUnit),
                            stats.slopePerSecond * (perMinute ? 60.0 : 3600.0), uiCtxUnit, true, perMinute ? "/m" : "/h",
                            UI_PLACEHOLDER_SHORT);
    }
}

// Brings the quick statistics screen up to date: the live text with
// every new reading, and the statistics job - started when the context
// changed (or on request), or when a new reading arrives and the last
// result is older than CONFIG_UI_STATS_REFRESH_MS; advanced by one
// slice per call. Returns true if anything on the screen changed.
static bool uiUpdateStatsScreen(bool force)
{
    char digits[KEITHLEY_DIGIT_COUNT];
    int8_t dpPos;
    uint16_t indicators;
    uint8_t unit;
    uint32_t seq;

    portENTER_CRITICAL(&uiLiveMux);
    memcpy(digits, uiLiveDigits, sizeof(digits));
    dpPos = uiLiveDp;
    indicators = uiLiveIndicators;
    unit = uiLiveUnit;
    seq = uiLiveSeq;
    portEXIT_CRITICAL(&uiLiveMux);

    bool changed = false;
    bool newReading = (seq != uiProcessedSeq);
    uiProcessedSeq = seq;

    if (newReading || force)
    {
        if (seq == 0)
        {
            return force; // no reading yet - the placeholder texts stay
        }
        uiFormatLiveText(uiScene.live, sizeof(uiScene.live), digits, dpPos, indicators);
        // v22: without a unit (K197 off, Err197) the unit of the statistics still shown stays
        uiUnitText((unit == KEITHLEY_UNIT_NONE && uiHaveContext) ? uiCtxUnit : unit, uiScene.unit, sizeof(uiScene.unit));
        changed = true;
    }

    if (seq != 0)
    {
        char filter[8];
        uiFunctionFilter(filter, sizeof(filter), keithleyFunctionPrefix(unit, indicators));
        uint8_t decimals = uiDecimalsFor(dpPos);
        if (filter[0] == '\0' && uiHaveContext && uiCtxFilter[0] != '\0')
        {
            // v22: not a reading (the K197 switched off, Err197, its self-test): the statistics of the last
            // function stay, so the log can still be looked at - the next real reading decides again
            snprintf(filter, sizeof(filter), "%s", uiCtxFilter);
            unit = uiCtxUnit;
            decimals = uiCtxDecimals;
        }

        bool contextChanged = !uiHaveContext || unit != uiCtxUnit || decimals != uiCtxDecimals ||
                              uiInterval != uiCtxInterval || strcmp(filter, uiCtxFilter) != 0;
        bool refreshDue = newReading && !uiJobRunning &&
                          (uint32_t)(millis() - uiStatsFinishedMs) >= CONFIG_UI_STATS_REFRESH_MS;

        if (contextChanged || force || refreshDue)
        {
            if (uiJobRunning)
            {
                datalogStatsAbort(&uiJob);
                uiJobRunning = false;
            }
            snprintf(uiCtxFilter, sizeof(uiCtxFilter), "%s", filter);
            uiCtxUnit = unit;
            uiCtxDecimals = decimals;
            uiCtxInterval = uiInterval;
            uiHaveContext = true;
            if (contextChanged)
            {
                uiSetPlaceholders(); // the old numbers are in another unit/function/interval (v25: after the new
                                     // context is set - the placeholders of pages 2 and 3 carry its units)
                changed = true;
            }

            if (filter[0] != '\0')
            {
                long first = datalogFirstRecordSince(uiIntervalSeconds[uiInterval], NULL);
                datalogStatsStart(&uiJob, filter, first, 0);
                uiStatsStartMs = millis();
                uiJobRunning = true;
            }
            else
            {
                uiStatsFinishedMs = millis(); // nothing to compute for a unit-less reading
            }
        }
    }

    if (uiJobRunning)
    {
        // One slice of the job: steps of CONFIG_UI_STATS_STEP_RECORDS records until the time budget is used up
        // (always at least one step), then back to the loop - so the picture and everything else get their turn.
        uint32_t sliceStartUs = micros();
        bool done;
        do
        {
            done = datalogStatsStep(&uiJob, CONFIG_UI_STATS_STEP_RECORDS);
        } while (!done && (uint32_t)(micros() - sliceStartUs) < (uint32_t)CONFIG_UI_STATS_SLICE_US);
        if (done)
        {
            uiPublishStats();
            changed = true;
        }
    }
    return changed;
}

bool uiBusy()
{
    bool graphShown = (uiScreen == UI_SCREEN_GRAPH || uiScreen == UI_SCREEN_GRAPH_SETUP);
    return uiJobRunning || (graphShown && graphBusy());
}

// With SYSTem:DEBUG on: how long a press waited between its first edge and
// the moment the UI handled it (debounce included, so about 31 ms is the
// floor), and the longest time one pass of the render/UI loop has kept
// that task busy so far. If the second number is large, the UI loop is
// what delays the buttons' effect. Goes through the debug queue
// (debuglog.h), never straight to Serial - this task must not wait for USB.
static void uiNoteButtonLatency(const char *name, uint8_t button)
{
    debugLogf("UI: %s press handled %lu ms after the first edge, longest UI loop pass %lu us", name,
              (unsigned long)(millis() - buttonsPressStartMs(button)), (unsigned long)displayGetStats().uiLoopMaxUs);
}

static void uiSetScreen(uint8_t screen)
{
    if (screen != UI_SCREEN_QUICKSTATS && uiJobRunning)
    {
        datalogStatsAbort(&uiJob); // leaving the statistics: free the job's memory
        uiJobRunning = false;
    }
    if (screen == UI_SCREEN_QUICKSTATS && uiScreen != UI_SCREEN_HELP && uiScreen != UI_SCREEN_QUICKSTATS)
    {
        uiStatsPage = FB_STATS_PAGE_MAIN; // v25: opened anew - page 1 (back from its help: the page stays)
        uiScene.page = uiStatsPage;
    }
    uiScreen = screen;
    uiSetupLastActivityMs = (uint32_t)millis(); // v15: a settings screen starts its timeout now
    uiLogClearArmed = false; // a hold that started on another screen never clears the log
    uiHelpArmed = false;     // ... nor opens the help
    uiHelpRelArmed = false;  // ... nor pages it
    uiHelpDbArmed = false;
    uiGraphRclArmed = false; // v14: an RCL hold that started elsewhere never acts on the graph
    uiGraphDbArmed = false;
    uiLcdRclArmed = false;   // v15: ... nor on the LCD screen
    uiLcdDbArmed = false;    // v17: ... nor opens the alarm settings
    uiAlarmRclArmed = false;
    uiAlarmRepeatButton = 0xFF;
    // REL/dB only act on the K197 while the plain LCD is shown (on the
    // statistics screen unless configured otherwise) - on the UI screens
    // they are navigation keys. On the help screen never.
    bool relDbActive = (screen == UI_SCREEN_LCD) ||
                       (screen == UI_SCREEN_QUICKSTATS && !CONFIG_UI_QUICKSTATS_BLOCKS_REL_DB);
    buttonsSetRelDbEnabled(relDbActive);
}

// ---- the help screen (v12) ----

// Number of lines of the preface (version, build) put in front of the DMM
// section of help_text.h; the STAT and GRAPH sections have none (v15).
#define UI_HELP_PREFACE_LINES 2

static uint16_t uiHelpPrefaceLines()
{
    return (uiHelpSection == HELP_SECTION_DMM) ? UI_HELP_PREFACE_LINES : 0;
}

// The language of SYSTem:LANGuage, clamped to the languages that exist.
static uint8_t uiLanguage()
{
    uint8_t language = settingsGetLanguage();
    return (language < HELP_LANG_COUNT) ? language : 0;
}

static uint16_t uiHelpCountTextLines(const char *text)
{
    uint16_t lines = 0;
    const char *p = text;
    while (*p != '\0')
    {
        const char *nl = strchr(p, '\n');
        lines++;
        if (nl == NULL)
        {
            break; // last line without a newline
        }
        p = nl + 1;
    }
    return lines;
}

// Selects the text of `section` in the current language (when the help opens).
static void uiHelpSelectText(uint8_t section)
{
    uiHelpSection = (section < HELP_SECTION_COUNT) ? section : HELP_SECTION_DMM;
    uiHelpTextPtr = helpTexts[uiLanguage()][uiHelpSection];
    uiHelpTextLines = uiHelpCountTextLines(uiHelpTextPtr);
}

static uint16_t uiHelpLineCount()
{
    return (uint16_t)(uiHelpPrefaceLines() + uiHelpTextLines);
}

// First line shown when the text is scrolled to its end: the last page
// ends with the last line (so it may overlap the page before it).
static uint16_t uiHelpLastTop()
{
    uint16_t count = uiHelpLineCount();
    return (count > FB_HELP_LINES) ? (uint16_t)(count - FB_HELP_LINES) : 0;
}

// Copies line `index` of the whole help (preface + text) into `out`
// (cut to its size) and tells whether it is underlined. All lines are
// drawn in one color (v13).
static void uiHelpGetLine(uint16_t index, char *out, size_t size, bool *underline, bool *rightAlign)
{
    *underline = false;
    *rightAlign = false;
    out[0] = '\0';

    if (index == 0 && uiHelpPrefaceLines() > 0)
    {
        // "R" = resurrected - a model letter Keithley never used.
        snprintf(out, size, "KEITHLEY 197R, fw. %s", BUILD_FIRMWARE);
        return;
    }
    if (index == 1 && uiHelpPrefaceLines() > 1)
    {
        char stamp[24];
        buildInfoFormatStamp(stamp, sizeof(stamp));
        snprintf(out, size, "build %s", stamp);
        *rightAlign = true; // flush with the right edge
        *underline = true;  // separates the header from the help text
        return;
    }

    // Walk to the wanted line of helpText - a few kB at most, only when a
    // page is drawn, so no index table is kept.
    uint16_t wanted = (uint16_t)(index - uiHelpPrefaceLines());
    const char *p = uiHelpTextPtr;
    for (uint16_t n = 0; n < wanted && *p != '\0'; n++)
    {
        const char *nl = strchr(p, '\n');
        if (nl == NULL)
        {
            return; // past the end: empty line
        }
        p = nl + 1;
    }
    size_t length = 0;
    while (p[length] != '\0' && p[length] != '\n' && length + 1 < size)
    {
        out[length] = p[length];
        length++;
    }
    out[length] = '\0';
}

static void uiOpenHelp()
{
    uiHelpReturnScreen = uiScreen;
    // v15: context help - the text of the screen it is opened from
    uint8_t section = HELP_SECTION_DMM;
    if (uiScreen == UI_SCREEN_QUICKSTATS)
    {
        section = HELP_SECTION_STAT;
    }
    else if (uiScreen == UI_SCREEN_GRAPH || uiScreen == UI_SCREEN_GRAPH_SETUP)
    {
        section = HELP_SECTION_GRAPH;
    }
    else if (uiScreen == UI_SCREEN_ALARM_SETUP)
    {
        section = HELP_SECTION_ALARM; // v17
    }
    uiHelpSelectText(section);
    uiHelpTop = 0;
    uiSetScreen(UI_SCREEN_HELP);
    uiHelpLastActivityMs = (uint32_t)millis();
    debugLogf("UI: help opened (REL held %u ms)", (unsigned)CONFIG_UI_HELP_HOLD_MS);
}

void uiFillHelpScene(FbHelpScene *scene)
{
    for (uint8_t i = 0; i < FB_HELP_LINES; i++)
    {
        uint16_t index = (uint16_t)(uiHelpTop + i);
        if (index < uiHelpLineCount())
        {
            uiHelpGetLine(index, scene->lines[i], sizeof(scene->lines[i]), &scene->underline[i], &scene->rightAlign[i]);
        }
        else
        {
            scene->lines[i][0] = '\0';
            scene->underline[i] = false;
            scene->rightAlign[i] = false;
        }
    }
}

uint16_t uiGetHelpTop()
{
    return uiHelpTop;
}

uint16_t uiGetHelpLineCount()
{
    return uiHelpLineCount();
}

uint8_t uiGetHelpSection()
{
    return uiHelpSection;
}

// Clears the whole log from the front panel (RCL held on the statistics
// screen). Runs in the UI task: the statistics job (same task) is stopped
// first so it never walks a log that changed under it, then the context is
// forgotten so the screen shows placeholders and a fresh job starts with
// the next reading. datalogClear() takes datalogMutex, like LOG:CLEar.
static void uiClearLog()
{
    if (uiJobRunning)
    {
        datalogStatsAbort(&uiJob);
        uiJobRunning = false;
    }
    datalogClear();
    graphInvalidate(); // v14 (the graph notices the clear by itself too, this just makes it immediate)
    uiHaveContext = false;
    uiSetPlaceholders();
    beeperPlayLogCleared(); // non-blocking, the UI keeps running during the 1 s beep
    debugLogAlways("UI: log cleared (RCL held %u ms on the statistics screen)", (unsigned)CONFIG_UI_LOG_CLEAR_HOLD_MS);
}

// ---- the graph screens (v14) ----

const FbGraphScene &uiGetGraphScene()
{
    return graphScene();
}

void uiFillGraphSetupScene(FbSetupScene *scene)
{
    graphFillSetupScene(scene, uiGraphSetupRow);
}

uint8_t uiGetGraphSetupRow()
{
    return uiGraphSetupRow;
}

// ---- the DMM settings screen (v15) ----

// Its words in every language of help_text.h (HELP_LANG_*). Theme names come
// from themes[] (config.h), language names from helpLanguageNames[].
#define UI_WORD_TITLE       0
#define UI_WORD_ROW0        1  // UI_WORD_ROW0 + UI_DMM_SETUP_ROW_*
#define UI_WORD_KEYS        7
#define UI_WORD_ON          8
#define UI_WORD_OFF         9
#define UI_WORD_LANG        10 // the language's own name, shown after its code
#define UI_WORD_SOUND0      11 // v23: + SETTINGS_BEEP_* (OFF, ALL, ALARMS)
#define UI_WORD_CSV0        14 // v23: + SETTINGS_CSV_* (COMMA, SEMICOLON)
#define UI_WORD_SD_NOCARD   16 // v23: SD state shown after ON
#define UI_WORD_SD_FULL     17
#define UI_WORD_SD_ERROR    18
#define UI_WORD_HOLD0       19 // v24: + SETTINGS_HOLD_* (MANUAL, AUTO)
#define UI_WORD_COUNT       21
static const char *const uiDmmSetupWords[HELP_LANG_COUNT][UI_WORD_COUNT] = {
    {"DMM - NASTAVENI", "JAZYK", "TEMA", "ZVUK", "SD KARTA", "FORMAT CSV", "HOLD",
     "REL/dB: zmena  RCL: dalsi  STO: zpet", "ZAP", "VYP", "CESTINA", "VYP", "VSE", "JEN ALARMY", "1.234 , (EN)",
     "1,234 ; (CZ)", "CHYBI KARTA", "PLNA KARTA", "CHYBA", "RUCNI", "AUTO HOLD"},
    {"DMM SETTINGS", "LANGUAGE", "THEME", "SOUND", "SD CARD", "CSV FORMAT", "HOLD",
     "REL/dB: change  RCL: next  STO: back", "ON", "OFF", "ENGLISH", "OFF", "ALL", "ALARMS ONLY", "1.234 , (EN)",
     "1,234 ; (CZ)", "NO CARD", "CARD FULL", "ERROR", "MANUAL", "AUTO HOLD"},
};

// v23: the order REL/dB step the sound through: ALL -> ALARMS ONLY -> OFF (and back).
static const uint8_t uiSoundOrder[SETTINGS_BEEP_MODES] = {SETTINGS_BEEP_ALL, SETTINGS_BEEP_ALARMS, SETTINGS_BEEP_OFF};

static const char *uiDmmWord(uint8_t word)
{
    return uiDmmSetupWords[uiLanguage()][word];
}

// One step of the value on the cursor line: the language, the theme, the
// sound and the CSV format go round, SD logging toggles. Applied at once,
// saved deferred.
static void uiDmmSetupStep(int8_t delta)
{
    switch (uiDmmSetupRow)
    {
        case UI_DMM_SETUP_ROW_LANGUAGE:
            settingsSetLanguageDeferred((uint8_t)(((int)uiLanguage() + delta + HELP_LANG_COUNT) % HELP_LANG_COUNT));
            break;
        case UI_DMM_SETUP_ROW_THEME:
        {
            uint8_t theme = settingsGetTheme();
            if (theme >= themeCount)
            {
                theme = 0;
            }
            theme = (uint8_t)(((int)theme + delta + themeCount) % themeCount);
            setTheme(theme); // the LCD replica and (through fbStatsTheme()) this screen at once
            settingsSetThemeDeferred(theme);
            break;
        }
        case UI_DMM_SETUP_ROW_SOUND:
        {
            uint8_t mode = settingsGetBeepMode();
            uint8_t pos = 0;
            for (uint8_t i = 0; i < SETTINGS_BEEP_MODES; i++)
            {
                if (uiSoundOrder[i] == mode)
                {
                    pos = i;
                }
            }
            pos = (uint8_t)(((int)pos + delta + SETTINGS_BEEP_MODES) % SETTINGS_BEEP_MODES);
            settingsSetBeepModeDeferred(uiSoundOrder[pos]);
            break;
        }
        case UI_DMM_SETUP_ROW_SD:
            settingsSetSdEnabledDeferred(!settingsGetSdEnabled()); // the SD task follows within CONFIG_SD_POLL_MS
            break;
        case UI_DMM_SETUP_ROW_CSV:
            settingsSetSdCsvFormatDeferred(
                (uint8_t)(((int)settingsGetSdCsvFormat() + delta + SETTINGS_CSV_FORMATS) % SETTINGS_CSV_FORMATS));
            break;
        case UI_DMM_SETUP_ROW_HOLD:
            // v24: the next STO hold uses it (a HOLD already on keeps its kind)
            settingsSetHoldModeDeferred(
                (uint8_t)(((int)settingsGetHoldMode() + delta + SETTINGS_HOLD_MODES) % SETTINGS_HOLD_MODES));
            break;
        default:
            break;
    }
}

// v23: keeps the cursor line inside the shown window of FB_SETUP_ROWS lines.
static void uiDmmSetupScroll()
{
    if (uiDmmSetupRow < uiDmmSetupTop)
    {
        uiDmmSetupTop = uiDmmSetupRow;
    }
    else if (uiDmmSetupRow >= uiDmmSetupTop + FB_SETUP_ROWS)
    {
        uiDmmSetupTop = (uint8_t)(uiDmmSetupRow - FB_SETUP_ROWS + 1);
    }
    if (uiDmmSetupTop + FB_SETUP_ROWS > UI_DMM_SETUP_ROWS)
    {
        uiDmmSetupTop = (UI_DMM_SETUP_ROWS > FB_SETUP_ROWS) ? (uint8_t)(UI_DMM_SETUP_ROWS - FB_SETUP_ROWS) : 0;
    }
}

// The value text of one line of the DMM settings.
static void uiDmmSetupValue(uint8_t row, char *out, size_t size)
{
    switch (row)
    {
        case UI_DMM_SETUP_ROW_LANGUAGE:
            snprintf(out, size, "%s - %s", helpLanguageNames[uiLanguage()], uiDmmWord(UI_WORD_LANG));
            break;
        case UI_DMM_SETUP_ROW_THEME:
        {
            uint8_t theme = settingsGetTheme();
            if (theme >= themeCount)
            {
                theme = 0;
            }
            snprintf(out, size, "%s (%u/%u)", themes[theme].name, (unsigned)(theme + 1), (unsigned)themeCount);
            break;
        }
        case UI_DMM_SETUP_ROW_SOUND:
            snprintf(out, size, "%s", uiDmmWord((uint8_t)(UI_WORD_SOUND0 + settingsGetBeepMode())));
            break;
        case UI_DMM_SETUP_ROW_SD:
        {
            if (!settingsGetSdEnabled())
            {
                snprintf(out, size, "%s", uiDmmWord(UI_WORD_OFF));
                break;
            }
            uint8_t state = sdlogState();
            int8_t word = -1;
            if (state == SDLOG_STATE_NO_CARD)
            {
                word = UI_WORD_SD_NOCARD;
            }
            else if (state == SDLOG_STATE_FULL)
            {
                word = UI_WORD_SD_FULL;
            }
            else if (state == SDLOG_STATE_ERROR)
            {
                word = UI_WORD_SD_ERROR;
            }
            if (word < 0)
            {
                snprintf(out, size, "%s", uiDmmWord(UI_WORD_ON));
            }
            else
            {
                snprintf(out, size, "%s - %s", uiDmmWord(UI_WORD_ON), uiDmmWord((uint8_t)word));
            }
            break;
        }
        case UI_DMM_SETUP_ROW_CSV:
            snprintf(out, size, "%s", uiDmmWord((uint8_t)(UI_WORD_CSV0 + settingsGetSdCsvFormat())));
            break;
        case UI_DMM_SETUP_ROW_HOLD:
            snprintf(out, size, "%s", uiDmmWord((uint8_t)(UI_WORD_HOLD0 + settingsGetHoldMode())));
            break;
        default:
            out[0] = '\0';
            break;
    }
}

void uiFillDmmSetupScene(FbSetupScene *scene)
{
    memset(scene, 0, sizeof(*scene));
    uiDmmSetupScroll();
    snprintf(scene->title, sizeof(scene->title), "%s", uiDmmWord(UI_WORD_TITLE));
    uint8_t shown = 0;
    for (uint8_t i = 0; i < FB_SETUP_ROWS && uiDmmSetupTop + i < UI_DMM_SETUP_ROWS; i++)
    {
        uint8_t row = (uint8_t)(uiDmmSetupTop + i);
        snprintf(scene->labels[i], sizeof(scene->labels[i]), "%s", uiDmmWord((uint8_t)(UI_WORD_ROW0 + row)));
        uiDmmSetupValue(row, scene->values[i], sizeof(scene->values[i]));
        shown++;
    }
    snprintf(scene->keys, sizeof(scene->keys), "%s", uiDmmWord(UI_WORD_KEYS));
    scene->cursorRow = (uint8_t)(uiDmmSetupRow - uiDmmSetupTop);
    scene->rowCount = shown;
    scene->moreAbove = uiDmmSetupTop > 0;
    scene->moreBelow = uiDmmSetupTop + shown < UI_DMM_SETUP_ROWS;
}

// v23: first line shown (host tests).
uint8_t uiGetDmmSetupTop()
{
    return uiDmmSetupTop;
}

uint8_t uiGetDmmSetupRow()
{
    return uiDmmSetupRow;
}

// ---- the alarm settings screen (v17) ----

#define UI_AWORD_TITLE      0
#define UI_AWORD_ROW0       1 // UI_AWORD_ROW0 + UI_ALARM_SETUP_ROW_*
#define UI_AWORD_KEYS       5
#define UI_AWORD_KEYS_LIMIT 6
#define UI_AWORD_MODE0      7 // UI_AWORD_MODE0 + ALARM_MODE_*
#define UI_AWORD_SIGNAL0    11 // UI_AWORD_SIGNAL0 + ALARM_SIGNAL_*
#define UI_AWORD_COUNT      13
static const char *const uiAlarmSetupWords[HELP_LANG_COUNT][UI_AWORD_COUNT] = {
    {"ALARM", "ALARM", "DOLNI MEZ", "HORNI MEZ", "SIGNAL", "REL/dB: zmena  RCL: dalsi  STO: zpet",
     "REL/dB: -/+  RCL 1s: = mereni  STO: zpet", "VYP", "HORNI", "DOLNI", "OBE MEZE", "ZVUK + SIPKA", "JEN SIPKA"},
    {"ALARM", "ALARM", "LOW LIMIT", "HIGH LIMIT", "SIGNAL", "REL/dB: change  RCL: next  STO: back",
     "REL/dB: -/+  RCL 1s: = reading  STO: back", "OFF", "UPPER", "LOWER", "BOTH LIMITS", "BEEP + ARROW",
     "ARROW ONLY"},
};

static const char *uiAlarmWord(uint8_t word)
{
    return uiAlarmSetupWords[uiLanguage()][word];
}

// Order of the modes on the screen: off, upper, lower, both (the ALARM_MODE_* values).
static void uiAlarmSetupStep(int8_t delta)
{
    if (uiAlarmFunction == ALARM_FUNCTION_NONE)
    {
        return;
    }
    if (uiAlarmSetupRow == UI_ALARM_SETUP_ROW_MODE)
    {
        uint8_t mode = alarmGetMode(uiAlarmFunction);
        mode = (uint8_t)(((int)mode + delta + ALARM_MODE_COUNT) % ALARM_MODE_COUNT);
        alarmSetMode(uiAlarmFunction, mode, false);
    }
    else if (uiAlarmSetupRow == UI_ALARM_SETUP_ROW_SIGNAL)
    {
        alarmSetSignal((uint8_t)(((int)alarmGetSignal() + delta + ALARM_SIGNAL_COUNT) % ALARM_SIGNAL_COUNT), false);
    }
    else
    {
        alarmStepLimit(uiAlarmFunction, uiAlarmSetupRow == UI_ALARM_SETUP_ROW_UPPER, delta, 1);
    }
}

// Step size (counts) of the n-th repeated step while REL/dB is held on a limit line.
static uint32_t uiAlarmRepeatStep(uint16_t repeats)
{
    uint32_t step = 1;
    for (uint16_t decade = (uint16_t)(repeats / CONFIG_UI_ALARM_REPEAT_PER_DECADE); decade > 0; decade--)
    {
        if (step * 10 > (uint32_t)CONFIG_UI_ALARM_MAX_STEP_COUNTS)
        {
            break;
        }
        step *= 10;
    }
    return step;
}

static void uiOpenAlarmSetup(uint8_t function)
{
    uiAlarmFunction = function;
    uiAlarmSetupRow = UI_ALARM_SETUP_ROW_MODE;
    uiSetScreen(UI_SCREEN_ALARM_SETUP);
    beeperBeepAsync(CONFIG_BEEP_CLICK_MS * 4); // the hold was recognised
    debugLogf("UI: alarm settings opened (%s)", alarmFunctionName(function));
}

void uiFillAlarmSetupScene(FbSetupScene *scene)
{
    memset(scene, 0, sizeof(*scene));
    uint8_t f = uiAlarmFunction;
    const char *name = alarmFunctionName(f);
    snprintf(scene->title, sizeof(scene->title), "%s - %s", (f == ALARM_FUNCTION_DB) ? "dB" : name,
             uiAlarmWord(UI_AWORD_TITLE));
    for (uint8_t row = 0; row < UI_ALARM_SETUP_ROWS; row++)
    {
        snprintf(scene->labels[row], sizeof(scene->labels[row]), "%s", uiAlarmWord((uint8_t)(UI_AWORD_ROW0 + row)));
    }
    uint8_t mode = alarmGetMode(f);
    snprintf(scene->values[UI_ALARM_SETUP_ROW_MODE], sizeof(scene->values[0]), "%s",
             uiAlarmWord((uint8_t)(UI_AWORD_MODE0 + (mode < ALARM_MODE_COUNT ? mode : 0))));
    uint8_t unit, decimals;
    alarmGetDisplayFormat(f, &unit, &decimals);
    char unitText[8];
    uiUnitText(unit, unitText, sizeof(unitText));
    for (uint8_t i = 0; i < 2; i++)
    {
        bool upper = (i == 1);
        uint8_t row = upper ? UI_ALARM_SETUP_ROW_UPPER : UI_ALARM_SETUP_ROW_LOWER;
        char number[16];
        alarmFormatLimit(number, sizeof(number), f, upper);
        if (strcmp(number, "--") == 0 || unitText[0] == '\0')
        {
            snprintf(scene->values[row], sizeof(scene->values[0]), "%s", number);
        }
        else
        {
            snprintf(scene->values[row], sizeof(scene->values[0]), "%s %s", number, unitText);
        }
    }
    uint8_t signal = alarmGetSignal();
    snprintf(scene->values[UI_ALARM_SETUP_ROW_SIGNAL], sizeof(scene->values[0]), "%s",
             uiAlarmWord((uint8_t)(UI_AWORD_SIGNAL0 + (signal < ALARM_SIGNAL_COUNT ? signal : 0))));
    bool limitRow = (uiAlarmSetupRow == UI_ALARM_SETUP_ROW_LOWER || uiAlarmSetupRow == UI_ALARM_SETUP_ROW_UPPER);
    snprintf(scene->keys, sizeof(scene->keys), "%s", uiAlarmWord(limitRow ? UI_AWORD_KEYS_LIMIT : UI_AWORD_KEYS));
    scene->cursorRow = uiAlarmSetupRow;
    scene->rowCount = UI_ALARM_SETUP_ROWS;
}

uint8_t uiGetAlarmSetupRow()
{
    return uiAlarmSetupRow;
}

uint8_t uiGetAlarmSetupFunction()
{
    return uiAlarmFunction;
}

// Hands the newest live reading to the graph (function, unit, decimals, text).
static void uiUpdateGraphLive()
{
    char digits[KEITHLEY_DIGIT_COUNT];
    int8_t dpPos;
    uint16_t indicators;
    uint8_t unit;
    uint32_t seq;
    portENTER_CRITICAL(&uiLiveMux);
    memcpy(digits, uiLiveDigits, sizeof(digits));
    dpPos = uiLiveDp;
    indicators = uiLiveIndicators;
    unit = uiLiveUnit;
    seq = uiLiveSeq;
    portEXIT_CRITICAL(&uiLiveMux);

    // v22: the last reading with a measurement function - kept while there is none (K197 off, Err197), so the
    // graph still shows that function's log instead of "NO READING"
    static GraphLive lastValid;
    static bool haveLastValid = false;

    GraphLive live;
    memset(&live, 0, sizeof(live)); // the graph compares the whole struct, so no stale bytes
    if (seq != 0)
    {
        uiFunctionFilter(live.filter, sizeof(live.filter), keithleyFunctionPrefix(unit, indicators));
        live.valid = (live.filter[0] != '\0');
        live.unit = unit;
        live.decimals = uiDecimalsFor(dpPos);
        uiFormatLiveText(live.text, sizeof(live.text), digits, dpPos, indicators);
    }
    if (live.valid)
    {
        lastValid = live;
        haveLastValid = true;
    }
    else if (haveLastValid)
    {
        live = lastValid;
        snprintf(live.text, sizeof(live.text), "%s", UI_PLACEHOLDER); // no live value, the function's log stays
    }
    graphSetLive(&live);
}

// v22: see uiRequestHome()
static volatile bool uiHomeRequested = false;

void uiRequestHome()
{
    uiHomeRequested = true;
    displayWakeUi();
}

bool uiPoll()
{
    bool changed = false;
    bool forceRefresh = false;

    if (uiHomeRequested)
    {
        uiHomeRequested = false;
        if (uiScreen != UI_SCREEN_LCD)
        {
            debugLogf("UI: back to the main display (DMM off / going to sleep)");
            uiSetScreen(UI_SCREEN_LCD);
            changed = true;
        }
    }

    // Release events are not used (REL's short press is recognised by the
    // button no longer being held, see below); they are drained so a stale
    // one cannot trigger anything later.
    buttonsTakeShortRelease(BUTTON_REL);
    buttonsTakeShortRelease(BUTTON_DB);
    buttonsTakeShortRelease(BUTTON_STO);
    buttonsTakeShortRelease(BUTTON_RCL);
    buttonsTakeLongRelease(BUTTON_REL);
    buttonsTakeLongRelease(BUTTON_DB);
    buttonsTakeLongRelease(BUTTON_STO);
    buttonsTakeLongRelease(BUTTON_RCL);

    bool relPressed = buttonsTakePress(BUTTON_REL);
    bool dbPressed = buttonsTakePress(BUTTON_DB);
    bool stoPressed = buttonsTakePress(BUTTON_STO);
    bool rclPressed = buttonsTakePress(BUTTON_RCL);
    if (relPressed)
        uiNoteButtonLatency("REL", BUTTON_REL);
    if (dbPressed)
        uiNoteButtonLatency("dB", BUTTON_DB);
    if (stoPressed)
        uiNoteButtonLatency("STO", BUTTON_STO);

    // v18: REL + dB held together = a marker on the reading shown at the first edge (LCD and graph screens).
    // The two presses belong to the chord: whatever they had started (help / alarm hold, a click at the
    // release) is cancelled; buttons.ino sends no pulse to the K197 for them.
    uint32_t chordMs = 0;
    if (buttonsTakeChord(&chordMs))
    {
        uiChordActive = true;
        uiHelpArmed = false;
        uiLcdDbArmed = false;
        uiGraphDbArmed = false;
        if (uiScreen == UI_SCREEN_LCD || uiScreen == UI_SCREEN_GRAPH)
        {
            markerAdd(chordMs, MARKER_SOURCE_KEYS);
            changed = true;
        }
    }
    if (uiChordActive)
    {
        // until both keys are up, REL/dB events belong to the chord (also a press event that arrived just after it)
        relPressed = dbPressed = false;
        if (!buttonsIsHeld(BUTTON_REL) && !buttonsIsHeld(BUTTON_DB))
        {
            uiChordActive = false;
        }
    }

    // v24: STO on the LCD: a click (at the release) opens the statistics - or, while HOLD is on, only ends it;
    // held CONFIG_UI_HOLD_HOLD_MS (while still held) starts HOLD of the kind on the DMM settings screen.
    // Manual HOLD: AT THE PRESS the LCD freezes on the reading shown at its first edge and HOLD lights (a
    // preview, undone if it turns out to be a click); after the hold time HOLD is confirmed: beep, the record
    // marked, TAG lit.
    if (stoPressed && uiScreen == UI_SCREEN_LCD)
    {
        uiLcdStoArmed = true;
        stoPressed = false;
        if (holdState() == HOLD_STATE_OFF && settingsGetHoldMode() == SETTINGS_HOLD_MANUAL)
        {
            holdPreview(buttonsPressStartMs(BUTTON_STO)); // frozen + HOLD at once, on what was shown at the first edge
            uiLcdStoFrozen = true;
        }
    }
    if (uiLcdStoArmed)
    {
        if (uiScreen != UI_SCREEN_LCD)
        {
            uiLcdStoArmed = false; // the screen changed under it (DMM off / sleep): forget the press
            if (uiLcdStoFrozen)
            {
                uiLcdStoFrozen = false;
                holdCancelPreview();
            }
        }
        else if (!buttonsIsHeld(BUTTON_STO))
        {
            uiLcdStoArmed = false;
            if (uiLcdStoFrozen)
            {
                uiLcdStoFrozen = false;
                holdCancelPreview(); // only a click: live again, HOLD dark
            }
            if (holdState() != HOLD_STATE_OFF)
            {
                holdStop();
                debugLogf("UI: HOLD ended");
            }
            else
            {
                uiSetScreen(UI_SCREEN_QUICKSTATS);
                forceRefresh = true;
                relPressed = dbPressed = rclPressed = false; // a press in the same instant belongs to the old screen
            }
            changed = true;
        }
        else if ((uint32_t)(millis() - buttonsPressStartMs(BUTTON_STO)) >= (uint32_t)CONFIG_UI_HOLD_HOLD_MS)
        {
            uiLcdStoArmed = false;
            uiLcdStoFrozen = false; // HOLD takes the preview over (the same frame)
            holdStart(settingsGetHoldMode(), buttonsPressStartMs(BUTTON_STO), true); // manual: marked, TAG lit now
            beeperBeepAsync(CONFIG_BEEP_CLICK_MS * 4); // the hold was recognised
            changed = true;
            debugLogf("UI: HOLD started (%s)", settingsGetHoldMode() == SETTINGS_HOLD_AUTO ? "AUTO" : "manual");
        }
    }

    if (stoPressed)
    {
        // STO: on the help screen back to where it was opened from; on the
        // graph back to the LCD, on the graph settings back to the graph;
        // otherwise it toggles the statistics screen (LCD <-> STATS).
        if (uiScreen == UI_SCREEN_HELP)
        {
            uiSetScreen(uiHelpReturnScreen);
        }
        else if (uiScreen == UI_SCREEN_GRAPH)
        {
            uiSetScreen(UI_SCREEN_LCD); // v14
        }
        else if (uiScreen == UI_SCREEN_GRAPH_SETUP)
        {
            uiSetScreen(UI_SCREEN_GRAPH); // v14
        }
        else if (uiScreen == UI_SCREEN_DMM_SETUP || uiScreen == UI_SCREEN_ALARM_SETUP)
        {
            uiSetScreen(UI_SCREEN_LCD); // v15, v17
        }
        else
        {
            uiSetScreen(uiScreen == UI_SCREEN_QUICKSTATS ? UI_SCREEN_LCD : UI_SCREEN_QUICKSTATS);
        }
        changed = true;
        forceRefresh = true;
        relPressed = dbPressed = rclPressed = false; // a press in the same instant belongs to the old screen
    }

    // REL on the LCD or statistics screen: a hold of CONFIG_UI_HELP_HOLD_MS
    // opens the help (while still held); a shorter press is a "REL click",
    // recognised when the button is no longer held. On the LCD the click
    // itself needs nothing here - buttons.ino sends the pulse to the K197
    // at the release; on the statistics screen it selects a shorter interval.
    bool relClick = false;
    // v17: also on the alarm settings, on its ALARM and SIGNAL lines (on a limit line holding REL repeats the step)
    bool alarmHelpLine = (uiScreen == UI_SCREEN_ALARM_SETUP) &&
                         (uiAlarmSetupRow == UI_ALARM_SETUP_ROW_MODE || uiAlarmSetupRow == UI_ALARM_SETUP_ROW_SIGNAL);
    if (relPressed &&
        (uiScreen == UI_SCREEN_LCD || uiScreen == UI_SCREEN_QUICKSTATS || uiScreen == UI_SCREEN_GRAPH || alarmHelpLine))
    {
        uiHelpArmed = true;
        relPressed = false; // consumed by the hold logic
    }
    if (uiHelpArmed)
    {
        if (!buttonsIsHeld(BUTTON_REL))
        {
            uiHelpArmed = false;
            relClick = true;
        }
        else if ((uint32_t)(millis() - buttonsPressStartMs(BUTTON_REL)) >= (uint32_t)CONFIG_UI_HELP_HOLD_MS)
        {
            uiOpenHelp();
            changed = true;
            dbPressed = rclPressed = false;
        }
    }

    if (uiScreen == UI_SCREEN_HELP)
    {
        // Full pages: REL click = previous page, dB click = next page (both at
        // the release); REL held = start, dB held = end (as soon as the hold
        // time is reached). The last page always ends with the last line of the
        // text. v25: RCL (at the press) = next page, after the last one back to
        // the first - like RCL = next line on the settings screens. STO is
        // handled above.
        uint32_t nowMs = (uint32_t)millis();
        uint16_t lastTop = uiHelpLastTop();
        uint16_t top = uiHelpTop;
        if (rclPressed)
        {
            top = (top >= lastTop) ? 0 : (uint16_t)(top + FB_HELP_LINES);
            if (top > lastTop)
            {
                top = lastTop;
            }
        }
        if (relPressed)
        {
            uiHelpRelArmed = true;
        }
        if (dbPressed)
        {
            uiHelpDbArmed = true;
        }
        if (uiHelpRelArmed)
        {
            if (!buttonsIsHeld(BUTTON_REL))
            {
                uiHelpRelArmed = false;
                top = (top > FB_HELP_LINES) ? (uint16_t)(top - FB_HELP_LINES) : 0;
            }
            else if ((uint32_t)(nowMs - buttonsPressStartMs(BUTTON_REL)) >= (uint32_t)CONFIG_UI_HELP_HOLD_MS)
            {
                uiHelpRelArmed = false;
                top = 0;
            }
        }
        if (uiHelpDbArmed)
        {
            if (!buttonsIsHeld(BUTTON_DB))
            {
                uiHelpDbArmed = false;
                top = (uint16_t)(top + FB_HELP_LINES);
                if (top > lastTop)
                {
                    top = lastTop;
                }
            }
            else if ((uint32_t)(nowMs - buttonsPressStartMs(BUTTON_DB)) >= (uint32_t)CONFIG_UI_HELP_HOLD_MS)
            {
                uiHelpDbArmed = false;
                top = lastTop;
            }
        }
        if (top != uiHelpTop)
        {
            uiHelpTop = top;
            changed = true;
        }

        // Timeout: any press or any button still held counts as activity.
        if (relPressed || dbPressed || rclPressed || buttonsIsHeld(BUTTON_REL) || buttonsIsHeld(BUTTON_DB) ||
            buttonsIsHeld(BUTTON_STO) || buttonsIsHeld(BUTTON_RCL))
        {
            uiHelpLastActivityMs = nowMs;
        }
        else if ((uint32_t)(nowMs - uiHelpLastActivityMs) >= (uint32_t)CONFIG_UI_HELP_TIMEOUT_MS)
        {
            debugLogf("UI: help closed after %u ms without a button", (unsigned)CONFIG_UI_HELP_TIMEOUT_MS);
            uiSetScreen(uiHelpReturnScreen);
            changed = true;
            forceRefresh = true;
        }
    }

    // v14: RCL on the LCD opens the graph; v15: at the release of a click - a hold of
    // CONFIG_UI_DMM_SETUP_HOLD_MS opens the DMM settings instead (while still held; the release then happens
    // on the settings screen, which only reacts to presses)
    if (uiScreen == UI_SCREEN_LCD)
    {
        if (rclPressed)
        {
            uiLcdRclArmed = true;
            rclPressed = false;
        }
        if (uiLcdRclArmed)
        {
            if (!buttonsIsHeld(BUTTON_RCL))
            {
                uiSetScreen(UI_SCREEN_GRAPH);
                changed = true;
                forceRefresh = true;
                debugLogf("UI: graph opened");
            }
            else if ((uint32_t)(millis() - buttonsPressStartMs(BUTTON_RCL)) >= (uint32_t)CONFIG_UI_DMM_SETUP_HOLD_MS)
            {
                uiSetScreen(UI_SCREEN_DMM_SETUP);
                beeperBeepAsync(CONFIG_BEEP_CLICK_MS * 4); // the hold was recognised
                changed = true;
                debugLogf("UI: DMM settings opened");
            }
        }
    }
    // v17: dB on the LCD - a hold of CONFIG_UI_ALARM_HOLD_MS opens the alarm settings of the function being
    // measured (while still held); a shorter press needs nothing here (buttons.ino sends it to the K197 at the
    // release). Without a known function (no DMM, self-test) the hold does nothing.
    if (uiScreen == UI_SCREEN_LCD)
    {
        if (dbPressed)
        {
            uiLcdDbArmed = true;
            dbPressed = false;
        }
        if (uiLcdDbArmed)
        {
            if (!buttonsIsHeld(BUTTON_DB))
            {
                uiLcdDbArmed = false;
            }
            else if ((uint32_t)(millis() - buttonsPressStartMs(BUTTON_DB)) >= (uint32_t)CONFIG_UI_ALARM_HOLD_MS)
            {
                uiLcdDbArmed = false;
                uint8_t function = alarmGetLiveFunction();
                if (function != ALARM_FUNCTION_NONE)
                {
                    uiOpenAlarmSetup(function);
                    changed = true;
                    relPressed = rclPressed = false;
                }
            }
        }
    }
    else if (uiScreen == UI_SCREEN_ALARM_SETUP)
    {
        uint32_t nowMs = (uint32_t)millis();
        bool limitRow = (uiAlarmSetupRow == UI_ALARM_SETUP_ROW_LOWER || uiAlarmSetupRow == UI_ALARM_SETUP_ROW_UPPER);
        // RCL: next line at the release; on a limit line a hold takes the live reading instead
        if (rclPressed)
        {
            uiAlarmRclArmed = true;
        }
        if (uiAlarmRclArmed)
        {
            if (!buttonsIsHeld(BUTTON_RCL))
            {
                uiAlarmRclArmed = false;
                uiAlarmSetupRow = (uint8_t)((uiAlarmSetupRow + 1) % UI_ALARM_SETUP_ROWS);
                uiAlarmRepeatButton = 0xFF;
                changed = true;
            }
            else if (limitRow &&
                     (uint32_t)(nowMs - buttonsPressStartMs(BUTTON_RCL)) >= (uint32_t)CONFIG_UI_ALARM_TAKE_HOLD_MS)
            {
                uiAlarmRclArmed = false;
                uint8_t liveFunction, liveUnit, liveDecimals;
                double live;
                if (alarmGetLiveValue(&liveFunction, &liveUnit, &liveDecimals, &live) && liveFunction == uiAlarmFunction)
                {
                    alarmSetLimit(uiAlarmFunction, uiAlarmSetupRow == UI_ALARM_SETUP_ROW_UPPER, live, false);
                    beeperBeepAsync(CONFIG_BEEP_CLICK_MS * 4);
                    changed = true;
                }
            }
        }
        // REL/dB: one step at the press; on a limit line repeating while held, the step growing tenfold. On the
        // ALARM and SIGNAL lines REL acts at the release of a click instead (relClick) - held it opens the help.
        if (relClick && !limitRow)
        {
            uiAlarmSetupStep(-1);
            changed = true;
        }
        if (relPressed || dbPressed)
        {
            uiAlarmSetupStep(relPressed ? -1 : +1);
            changed = true;
            if (limitRow)
            {
                uiAlarmRepeatButton = relPressed ? BUTTON_REL : BUTTON_DB;
                uiAlarmRepeatNextMs = buttonsPressStartMs(uiAlarmRepeatButton) + CONFIG_UI_ALARM_REPEAT_DELAY_MS;
                uiAlarmRepeatCount = 0;
            }
        }
        if (uiAlarmRepeatButton != 0xFF)
        {
            if (!limitRow || !buttonsIsHeld(uiAlarmRepeatButton))
            {
                uiAlarmRepeatButton = 0xFF;
            }
            else if ((int32_t)(nowMs - uiAlarmRepeatNextMs) >= 0)
            {
                alarmStepLimit(uiAlarmFunction, uiAlarmSetupRow == UI_ALARM_SETUP_ROW_UPPER,
                               (uiAlarmRepeatButton == BUTTON_REL) ? -1 : +1, uiAlarmRepeatStep(uiAlarmRepeatCount));
                uiAlarmRepeatCount++;
                uiAlarmRepeatNextMs += CONFIG_UI_ALARM_REPEAT_MS;
                changed = true;
            }
        }
    }
    else if (uiScreen == UI_SCREEN_DMM_SETUP)
    {
        // v23: the SD line shows the card state - redraw when it changes (sdlog.ino wakes the UI then)
        uint8_t sdView = settingsGetSdEnabled() ? sdlogState() : 0xFF;
        if (sdView != uiDmmSdViewShown)
        {
            uiDmmSdViewShown = sdView;
            changed = true;
        }
        // RCL = next line, REL/dB = change the value (at the press), STO (above) = back to the LCD
        if (rclPressed)
        {
            uiDmmSetupRow = (uint8_t)((uiDmmSetupRow + 1) % UI_DMM_SETUP_ROWS);
            uiDmmSetupScroll(); // v23: the list scrolls with the cursor
            changed = true;
        }
        if (relPressed)
        {
            uiDmmSetupStep(-1);
            changed = true;
        }
        if (dbPressed)
        {
            uiDmmSetupStep(+1);
            changed = true;
        }
    }

    if (uiScreen == UI_SCREEN_GRAPH)
    {
        // REL click = shorter interval, dB click = longer - both at the release (v18: so that REL + dB
        // together can be the marker instead)
        if (relClick && graphGetInterval() > 0)
        {
            graphSetInterval((uint8_t)(graphGetInterval() - 1));
            changed = true;
        }
        if (dbPressed)
        {
            uiGraphDbArmed = true;
        }
        if (uiGraphDbArmed && !buttonsIsHeld(BUTTON_DB))
        {
            uiGraphDbArmed = false;
            if (graphGetInterval() + 1 < GRAPH_INTERVAL_COUNT)
            {
                graphSetInterval((uint8_t)(graphGetInterval() + 1));
                changed = true;
            }
        }
        // RCL click (at release) = next Y axis mode AUTO -> ZERO -> SUPP -> AUTO (SUPP is centred on the
        // newest value every time it is entered); RCL held = the settings screen (while still held - the
        // release then happens on the settings screen, which only reacts to presses)
        if (rclPressed)
        {
            uiGraphRclArmed = true;
        }
        if (uiGraphRclArmed)
        {
            if (!buttonsIsHeld(BUTTON_RCL))
            {
                uiGraphRclArmed = false;
                graphSetupStep(GRAPH_SETUP_ROW_YMODE, +1);
                changed = true;
            }
            else if ((uint32_t)(millis() - buttonsPressStartMs(BUTTON_RCL)) >= (uint32_t)CONFIG_GRAPH_SETUP_HOLD_MS)
            {
                uiGraphRclArmed = false;
                uiSetScreen(UI_SCREEN_GRAPH_SETUP);
                beeperBeepAsync(CONFIG_BEEP_CLICK_MS * 4); // a slightly longer click: the hold was recognised
                changed = true;
            }
        }
    }
    else if (uiScreen == UI_SCREEN_GRAPH_SETUP)
    {
        // RCL = next line, REL/dB = change the value (at the press - no help hold here)
        if (rclPressed)
        {
            uiGraphSetupRow = (uint8_t)((uiGraphSetupRow + 1) % GRAPH_SETUP_ROWS);
            changed = true;
        }
        if (relPressed)
        {
            graphSetupStep(uiGraphSetupRow, -1);
            changed = true;
        }
        if (dbPressed)
        {
            graphSetupStep(uiGraphSetupRow, +1);
            changed = true;
        }
    }
    // v15: the settings screens close by themselves like the help: after CONFIG_UI_HELP_TIMEOUT_MS without any
    // button pressed or held, back to the graph / the LCD
    if (uiScreen == UI_SCREEN_GRAPH_SETUP || uiScreen == UI_SCREEN_DMM_SETUP || uiScreen == UI_SCREEN_ALARM_SETUP)
    {
        uint32_t nowMs = (uint32_t)millis();
        if (relPressed || dbPressed || rclPressed || buttonsIsHeld(BUTTON_REL) || buttonsIsHeld(BUTTON_DB) ||
            buttonsIsHeld(BUTTON_STO) || buttonsIsHeld(BUTTON_RCL))
        {
            uiSetupLastActivityMs = nowMs;
        }
        else if ((uint32_t)(nowMs - uiSetupLastActivityMs) >= (uint32_t)CONFIG_UI_HELP_TIMEOUT_MS)
        {
            debugLogf("UI: settings closed after %u ms without a button", (unsigned)CONFIG_UI_HELP_TIMEOUT_MS);
            uiSetScreen(uiScreen == UI_SCREEN_GRAPH_SETUP ? UI_SCREEN_GRAPH : UI_SCREEN_LCD);
            changed = true;
            forceRefresh = true;
        }
    }

    if (uiScreen == UI_SCREEN_GRAPH || uiScreen == UI_SCREEN_GRAPH_SETUP)
    {
        // The graph keeps reading the log on its settings screen too, so going
        // back shows it at once; only the graph screen itself redraws for it.
        uiUpdateGraphLive();
        bool graphChanged = graphPoll();
        if (uiScreen == UI_SCREEN_GRAPH && (graphChanged || forceRefresh))
        {
            changed = true;
        }
    }

    if (uiScreen == UI_SCREEN_QUICKSTATS)
    {
        // RCL held: clear the whole log (once per press, only for a press that started here)
        if (rclPressed)
        {
            uiLogClearArmed = true;
        }
        if (uiLogClearArmed)
        {
            if (!buttonsIsHeld(BUTTON_RCL))
            {
                // released before the log was cleared: a click - v25: the next page of the statistics
                uiLogClearArmed = false;
                uiStatsPage = (uint8_t)((uiStatsPage + 1) % FB_STATS_PAGE_COUNT);
                uiScene.page = uiStatsPage;
                changed = true;
                debugLogf("UI: statistics page %u", (unsigned)(uiStatsPage + 1));
            }
            else if ((uint32_t)(millis() - buttonsPressStartMs(BUTTON_RCL)) >= (uint32_t)CONFIG_UI_LOG_CLEAR_HOLD_MS)
            {
                uiLogClearArmed = false;
                uiClearLog();
                forceRefresh = true;
                changed = true;
            }
        }

        // REL (click, at release) = shorter interval, dB (at press) = longer
        // (the triangles on the screen point the same way)
        if (relClick && uiInterval > 0)
        {
            uiSetStatsInterval((uint8_t)(uiInterval - 1));
            changed = true;
        }
        if (dbPressed && uiInterval + 1 < UI_INTERVAL_COUNT)
        {
            uiSetStatsInterval((uint8_t)(uiInterval + 1));
            changed = true;
        }
        if (uiUpdateStatsScreen(forceRefresh))
        {
            changed = true;
        }
        if (uiUpdateDateTime())
        {
            changed = true;
        }
    }

    return changed;
}

// ---- v16: numbers for SYSTem:DIAGnostic? ----

void uiGetStatsTiming(uint32_t *lastMs, uint32_t *maxMs, uint32_t *lastRecords)
{
    *lastMs = uiStatsLastMs;
    *maxMs = uiStatsMaxMs;
    *lastRecords = uiStatsLastRecords;
}

void uiDiagClear()
{
    uiStatsMaxMs = 0;
}
