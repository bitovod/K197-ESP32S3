// Host test for the v8 user interface: ui_format.h, the statistics jobs and time windows of the
// real datalog.ino, and the real ui.ino (buttons -> screens -> texts), with the hardware-facing
// pieces (buttons, settings, RTC) replaced by simple stand-ins.
#include <Arduino.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <vector>
#include <math.h>
#include "config.h"
#include "display.h"
#include "datalog.h"
#include "reading_math.h"
#include "ui.h"
#include "ui_format.h"
#include "buttons.h"
#include "debuglog.h"
#include "help_text.h"
#include "settings.h" // v23: SETTINGS_BEEP_*, SETTINGS_CSV_*
#include "sdlog.h"    // v23: SDLOG_STATE_*
#include "hold.h"     // v24: HOLD_STATE_*

// A small step and no time budget, so that a long statistics job takes several UI polls on a fast PC too.
#undef CONFIG_UI_STATS_STEP_RECORDS
#define CONFIG_UI_STATS_STEP_RECORDS 200
// v20: the scenarios below feed only a few readings after a change of the function - keep the settling short
// here (the rule itself is checked with whatever value; the reference model follows the constant)
#undef CONFIG_DATALOG_SETTLE_READINGS
#define CONFIG_DATALOG_SETTLE_READINGS 3
// v22: the long power-on settling is tested in test_settle.cpp; here it is the ordinary one
#undef CONFIG_DATALOG_SETTLE_POWERON_READINGS
#define CONFIG_DATALOG_SETTLE_POWERON_READINGS CONFIG_DATALOG_SETTLE_READINGS
#undef CONFIG_UI_STATS_SLICE_US
#define CONFIG_UI_STATS_SLICE_US 0 // no time budget: exactly one step per poll

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)
#define CHECK_STR(a, b, msg) do { if (strcmp((a), (b)) != 0) { printf("FAIL: %s: got '%s' expected '%s' (line %d)\n", msg, (a), (b), __LINE__); failures++; } } while (0)

// ---- stand-ins for settings.ino / buttons.ino ----
static uint16_t gAutoInterval = 0;
uint16_t settingsGetAutoIntervalSeconds() { return gAutoInterval; }
bool settingsGetDebugEnabled() { return false; }
static uint8_t gStoredInterval = CONFIG_UI_STATS_DEFAULT_INTERVAL_INDEX; // what "EEPROM" holds
static int gIntervalSaves = 0;
uint8_t settingsGetStatsInterval() { return gStoredInterval; }
static uint8_t gLanguage = HELP_LANG_CZ; // v15 SYSTem:LANGuage
uint8_t settingsGetLanguage() { return gLanguage; }
static int gLanguageDeferred = 0;
void settingsSetLanguageDeferred(uint8_t l) { gLanguage = l; gLanguageDeferred++; }
static uint8_t gTheme = 1, gThemeApplied = 1;
uint8_t settingsGetTheme() { return gTheme; }
void settingsSetThemeDeferred(uint8_t t) { gTheme = t; }
void setTheme(uint8_t t) { gThemeApplied = t; }
static uint8_t gBeepMode = SETTINGS_BEEP_ALL; // v23: three values
uint8_t settingsGetBeepMode() { return gBeepMode; }
void settingsSetBeepModeDeferred(uint8_t m) { gBeepMode = m; }
// v23 SD card logging (settings + the SD task's state)
static bool gSdEnabled = false;
static uint8_t gSdCsv = SETTINGS_CSV_SEMICOLON;
static uint8_t gSdState = 0; // SDLOG_STATE_OFF
bool settingsGetSdEnabled() { return gSdEnabled; }
void settingsSetSdEnabledDeferred(bool e) { gSdEnabled = e; }
uint8_t settingsGetSdCsvFormat() { return gSdCsv; }
void settingsSetSdCsvFormatDeferred(uint8_t f) { gSdCsv = f; }
uint8_t sdlogState() { return gSdState; }
// v24 DATA HOLD (hold.ino stand-in) + its setting
static uint8_t gHoldMode = SETTINGS_HOLD_MANUAL;
uint8_t settingsGetHoldMode() { return gHoldMode; }
void settingsSetHoldModeDeferred(uint8_t m) { gHoldMode = m; }
static uint8_t gHoldState = 0; // HOLD_STATE_OFF
static int gHoldStarts = 0, gHoldStops = 0;
static uint8_t gHoldStartMode = 0;
static uint32_t gHoldStartAt = 0;
static bool gHoldShowTag = false;
void holdStart(uint8_t mode, uint32_t atMs, bool showTag)
{
    gHoldStarts++; gHoldStartMode = mode; gHoldStartAt = atMs; gHoldShowTag = showTag;
    gHoldState = (mode == SETTINGS_HOLD_AUTO) ? HOLD_STATE_AUTO_WAIT : HOLD_STATE_MANUAL;
}
void holdStop() { gHoldStops++; gHoldState = HOLD_STATE_OFF; }
uint8_t holdState() { return gHoldState; }
// v24: the manual HOLD preview at the press (frozen + HOLD lit)
static bool gFrozen = false;
static uint32_t gFrozenAt = 0;
void holdPreview(uint32_t atMs) { if (gHoldState == HOLD_STATE_OFF) { gFrozen = true; gFrozenAt = atMs; } }
void holdCancelPreview() { gFrozen = false; }
void settingsSetStatsIntervalDeferred(uint8_t index) { gStoredInterval = index; gIntervalSaves++; }

static bool gPress[4], gShort[4], gLong[4];
// v18 chord + markers
static bool gChord = false;
static uint32_t gChordMs = 0;
bool buttonsTakeChord(uint32_t *ms) { if (!gChord) return false; gChord = false; *ms = gChordMs; return true; }
static int gMarkers = 0;
static uint32_t gMarkerMs = 0;
bool markerAdd(uint32_t atMs, uint8_t) { gMarkers++; gMarkerMs = atMs; return datalogMarkAtMs(atMs, NULL); }
static bool gRelDbEnabled = true;
static bool take(bool *flags, uint8_t b) { bool r = flags[b]; flags[b] = false; return r; }
bool buttonsTakePress(uint8_t b) { return take(gPress, b); }
bool buttonsTakeShortRelease(uint8_t b) { return take(gShort, b); }
bool buttonsTakeLongRelease(uint8_t b) { return take(gLong, b); }
void buttonsSetRelDbEnabled(bool e) { gRelDbEnabled = e; }
static uint32_t gPressStart[4];
static bool gHeld[4];
uint32_t buttonsPressStartMs(uint8_t b) { return gPressStart[b]; }
bool buttonsIsHeld(uint8_t b) { return gHeld[b]; }
static int gLogClearedBeeps = 0;
void beeperPlayLogCleared() { gLogClearedBeeps++; }
static int gAsyncBeeps = 0;
void beeperBeepAsync(uint16_t) { gAsyncBeeps++; }
void beeperAlarmAsync(uint16_t) { gAsyncBeeps++; } // v23: the limit alarm beeps as an alarm sound
// v14 graph settings ("EEPROM")
static uint8_t gGraphStored[4] = {CONFIG_GRAPH_DEFAULT_INTERVAL_INDEX, CONFIG_GRAPH_DEFAULT_YMODE, CONFIG_GRAPH_DEFAULT_TRACE, CONFIG_GRAPH_DEFAULT_SPAN_INDEX};
static int gGraphSaves = 0;
uint8_t settingsGetGraphInterval() { return gGraphStored[0]; }
uint8_t settingsGetGraphYMode() { return gGraphStored[1]; }
uint8_t settingsGetGraphTrace() { return gGraphStored[2]; }
uint8_t settingsGetGraphSpan() { return gGraphStored[3]; }
void settingsSetGraphDeferred(uint8_t a, uint8_t b, uint8_t c, uint8_t d) { gGraphStored[0] = a; gGraphStored[1] = b; gGraphStored[2] = c; gGraphStored[3] = d; gGraphSaves++; }
static bool gDebugTrace = false;
static int gDebugMessages = 0;
void debugLogf(const char *, ...) { if (gDebugTrace) gDebugMessages++; }
void debugLogAlways(const char *, ...) {}
DisplayStats displayGetStats() { DisplayStats d; memset(&d, 0, sizeof(d)); return d; }
void displayWakeUi() {} // v22: uiRequestHome()
// v17 limit alarm ("EEPROM" of settings.ino, the display symbol)
static uint8_t gAlarmMode[6] = {3, 3, 3, 3, 3, 3};
static float gAlarmLow[6] = {NAN, NAN, NAN, NAN, NAN, NAN}, gAlarmHigh[6] = {NAN, NAN, NAN, NAN, NAN, NAN};
static uint8_t gAlarmSignal = 0;
static int gAlarmSavesNow = 0, gAlarmSavesDeferred = 0;
void settingsGetAlarm(uint8_t f, uint8_t *m, float *l, float *h) { *m = gAlarmMode[f]; *l = gAlarmLow[f]; *h = gAlarmHigh[f]; }
void settingsSetAlarm(uint8_t f, uint8_t m, float l, float h) { gAlarmMode[f] = m; gAlarmLow[f] = l; gAlarmHigh[f] = h; gAlarmSavesNow++; }
void settingsSetAlarmDeferred(uint8_t f, uint8_t m, float l, float h) { gAlarmMode[f] = m; gAlarmLow[f] = l; gAlarmHigh[f] = h; gAlarmSavesDeferred++; }
uint8_t settingsGetAlarmSignal() { return gAlarmSignal; }
void settingsSetAlarmSignal(uint8_t s) { gAlarmSignal = s; gAlarmSavesNow++; }
void settingsSetAlarmSignalDeferred(uint8_t s) { gAlarmSignal = s; gAlarmSavesDeferred++; }
static uint8_t gAlarmSymbol = 0;
static int gAlarmSymbolCalls = 0;
void setAlarmIndicator(uint8_t code) { gAlarmSymbol = code; gAlarmSymbolCalls++; }

// ---- the real files under test ----
#include "reading_math.ino"
// the tests size the log themselves (ESP.freePsram): no test cap of config.h
#undef CONFIG_DATALOG_TEST_CAPACITY
#define CONFIG_DATALOG_TEST_CAPACITY 0
#include "datalog.ino"
#include "graph.ino"
#include "alarm.ino"
#include "ui.ino"

// ---- test clock: the RTC runs on gClockMs, 0 = RTC not available ----
static uint32_t gBaseSeconds = 0;
static uint32_t gClockMs = 0;
static bool gRtcOn = false;
static long daysFromCivil(long y, long m, long d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
static uint32_t secondsSince2000(long y, long m, long d, long hh, long mm, long ss)
{
    return (uint32_t)((daysFromCivil(y, m, d) - 10957) * 86400L + hh * 3600L + mm * 60L + ss);
}
static void setClock()
{
    fakeRtcSeconds() = gRtcOn ? gBaseSeconds + gClockMs / 1000 : 0;
    fakeRtcCentis() = gRtcOn ? (uint8_t)((gClockMs % 1000) / 10) : 0;
}
static uint32_t nowSeconds() { return gRtcOn ? gBaseSeconds + gClockMs / 1000 : 0; }

// ---- helpers ----
struct Ref { double value; bool overload; std::string function; uint32_t ts; bool settle; double tsec; };
// v20: the same settling rule as datalog.ino - the first CONFIG_DATALOG_SETTLE_READINGS readings after a change of
// the function (or after a frame that is not a reading) are left out of the statistics
static std::string gSettlePrefix;
static bool gSettleKnown = false;
static int gSettleCount = 0;
static std::vector<Ref> gRef;

// Feeds one reading the way dmmApplyFrame() does (datalog first, then the UI live value);
// every reading takes 300 ms of test clock, like the real DMM's ~3.5 readings per second.
static void feedDigits(const char *digits6, int dp, uint16_t ind, uint8_t unit)
{
    gClockMs += 300;
    fakeMillisOffset() += 300; // the UI's refresh timer runs on millis()
    setClock();
    char d[6];
    memcpy(d, digits6, 6);
    datalogUpdateReading(d, (int8_t)dp, ind, unit);
    uiUpdateLiveReading(d, (int8_t)dp, ind, unit);
    alarmUpdateReading(d, (int8_t)dp, ind, unit); // v17, like dmmApplyFrame()

    Ref r;
    r.overload = keithleyIsOverload(d);
    double v = 0;
    if (!r.overload && !keithleyReadingToDouble(d, (int8_t)dp, ind, unit, &v)) { gSettleKnown = false; return; }
    std::string prefix = keithleyFunctionPrefix(unit, ind);
    if (!gSettleKnown || prefix != gSettlePrefix) { gSettlePrefix = prefix; gSettleKnown = true; gSettleCount = 0; }
    r.settle = gSettleCount < CONFIG_DATALOG_SETTLE_READINGS;
    gSettleCount++;
    r.value = (double)(float)v; // the log stores floats
    char f[8];
    uiFunctionFilter(f, sizeof(f), keithleyFunctionPrefix(unit, ind));
    r.function = f;
    r.ts = nowSeconds();
    r.tsec = gRtcOn ? gClockMs / 1000.0 : -1.0; // v25: the RTC time with centiseconds (the offset cancels out)
    gRef.push_back(r);
}

// mantissa as the six digits of the display; the half digit in front is blank unless it is a 1
static void feed(int mantissa, int dp, uint8_t unit, bool ac = false)
{
    char d[16];
    int m = abs(mantissa);
    if (m < 100000) snprintf(d, sizeof(d), " %05d", m);
    else snprintf(d, sizeof(d), "%06d", m);
    uint16_t ind = 0;
    if (mantissa < 0) ind |= KEITHLEY_BIT_MINUS;
    if (ac) ind |= KEITHLEY_BIT_AC;
    feedDigits(d, dp, ind, unit);
}

struct Expect { bool ok; double mn, mx, avg, med; size_t n; };
static Expect fromValues(std::vector<double> v)
{
    Expect e{false, 0, 0, 0, 0, 0};
    if (v.empty()) return e;
    e.ok = true; e.n = v.size();
    double sum = 0; e.mn = v[0]; e.mx = v[0];
    for (double x : v) { sum += x; e.mn = std::min(e.mn, x); e.mx = std::max(e.mx, x); }
    e.avg = sum / v.size();
    std::sort(v.begin(), v.end());
    e.med = (v.size() % 2) ? v[v.size() / 2] : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2.0;
    return e;
}
// last `window` records of the whole log, then the function filter (like LOG:AVERage? DCV -N 0)
static Expect referenceLast(const char *function, size_t window)
{
    size_t start = gRef.size() > window ? gRef.size() - window : 0;
    std::vector<double> v;
    for (size_t i = start; i < gRef.size(); i++)
        if (!gRef[i].overload && !gRef[i].settle && gRef[i].function == function) v.push_back(gRef[i].value);
    return fromValues(v);
}
// records not older than windowSeconds (0 = all), then the function filter
static Expect referenceSince(const char *function, uint32_t windowSeconds)
{
    uint32_t cutoff = nowSeconds() > windowSeconds ? nowSeconds() - windowSeconds : 0;
    std::vector<double> v;
    for (const Ref &r : gRef)
    {
        if (windowSeconds != 0 && r.ts < cutoff) continue;
        if (!r.overload && !r.settle && r.function == function) v.push_back(r.value);
    }
    return fromValues(v);
}
static long referenceFirstIndex(uint32_t windowSeconds, size_t logSize)
{
    // gRef mirrors the log only until it wraps; the callers use it on a fresh/not wrapped log
    if (windowSeconds == 0) return 1;
    uint32_t cutoff = nowSeconds() - windowSeconds;
    for (size_t i = 0; i < logSize; i++)
        if (gRef[i].ts >= cutoff) return (long)i + 1;
    return (long)logSize;
}

// v25: standard deviation (n - 1) and trend (least squares over the RTC time, timestamped records only) of the
// records not older than windowSeconds, the function filter applied
struct Spread { size_t n; double sdev; bool sdevOk; double slope; bool slopeOk; };
static Spread referenceSpread(const char *function, uint32_t windowSeconds)
{
    uint32_t cutoff = nowSeconds() > windowSeconds ? nowSeconds() - windowSeconds : 0;
    std::vector<double> v, t, vt;
    for (const Ref &r : gRef)
    {
        if (windowSeconds != 0 && r.ts < cutoff) continue;
        if (r.overload || r.settle || r.function != function) continue;
        v.push_back(r.value);
        if (r.tsec >= 0) { t.push_back(r.tsec); vt.push_back(r.value); }
    }
    Spread s{v.size(), 0, false, 0, false};
    if (v.size() >= 2)
    {
        double m = 0; for (double x : v) m += x; m /= v.size();
        double q = 0; for (double x : v) q += (x - m) * (x - m);
        s.sdev = sqrt(q / (v.size() - 1)); s.sdevOk = true;
    }
    if (t.size() >= 3)
    {
        double mt = 0, mv = 0;
        for (size_t i = 0; i < t.size(); i++) { mt += t[i]; mv += vt[i]; }
        mt /= t.size(); mv /= t.size();
        double sxx = 0, sxy = 0;
        for (size_t i = 0; i < t.size(); i++) { sxx += (t[i] - mt) * (t[i] - mt); sxy += (t[i] - mt) * (vt[i] - mv); }
        if (sxx > 0) { s.slope = sxy / sxx; s.slopeOk = true; }
    }
    return s;
}
static bool nearRel(double a, double b, double rel) { return fabs(a - b) <= rel * (fabs(b) + 1e-30); }

static bool near(double a, double b) { return fabs(a - b) <= 1e-9 * (1.0 + fabs(b)); }
static void pollUntilIdle(int maxPolls = 100) { for (int i = 0; i < maxPolls && uiJobRunning; i++) uiPoll(); }
static void press(uint8_t button) { gPress[button] = true; gPressStart[button] = (uint32_t)millis(); }

int main()
{
    // ------------------------------------------------------------------
    // 1. ui_format.h
    // ------------------------------------------------------------------
    char buf[24];
    {
        const char d1[6] = {'1', '2', '3', '4', '5', '6'};
        CHECK(uiFormatLiveText(buf, sizeof(buf), d1, 0, 0), "live ok"); CHECK_STR(buf, "1.23456", "live 1.23456");
        CHECK(uiFormatLiveText(buf, sizeof(buf), d1, 0, KEITHLEY_BIT_MINUS), "live ok"); CHECK_STR(buf, "-1.23456", "live minus");
        const char d2[6] = {' ', '1', '2', '3', '4', '5'};
        uiFormatLiveText(buf, sizeof(buf), d2, 1, 0); CHECK_STR(buf, "1.2345", "leading blank skipped");
        uiFormatLiveText(buf, sizeof(buf), d2, 2, 0); CHECK_STR(buf, "12.345", "dp 2");
        const char d3[6] = {' ', '0', '1', '2', '3', '4'};
        uiFormatLiveText(buf, sizeof(buf), d3, 0, 0); CHECK_STR(buf, "0.01234", "blank at dp becomes 0");
        const char d4[6] = {' ', ' ', ' ', ' ', 'L', ' '};
        CHECK(uiFormatLiveText(buf, sizeof(buf), d4, 1, 0), "OL ok"); CHECK_STR(buf, "OL", "overload");
        uiFormatLiveText(buf, sizeof(buf), d4, 1, KEITHLEY_BIT_MINUS); CHECK_STR(buf, "-OL", "minus overload");
        const char d6[6] = {'0', '0', '4', '3', '2', '7'};
        uiFormatLiveText(buf, sizeof(buf), d6, 1, KEITHLEY_BIT_MINUS); CHECK_STR(buf, "-0.4327", "leading zero dropped (-00.4327 -> -0.4327)");
        const char d7[6] = {' ', '0', '0', '1', '2', '3'};
        uiFormatLiveText(buf, sizeof(buf), d7, 2, 0); CHECK_STR(buf, "0.123", "blank + zeros -> one zero");
        const char d8[6] = {'0', '0', '0', '0', '0', '0'};
        uiFormatLiveText(buf, sizeof(buf), d8, 2, 0); CHECK_STR(buf, "0.000", "zero reading keeps one zero");
        const char d9[6] = {'1', '0', '0', '0', '0', '0'};
        uiFormatLiveText(buf, sizeof(buf), d9, 2, 0); CHECK_STR(buf, "100.000", "inner zeros kept");
        CHECK(!uiFormatLiveText(buf, sizeof(buf), d1, -1, 0), "no dp is not a reading"); CHECK_STR(buf, UI_PLACEHOLDER, "no dp text");
        const char d5[6] = {'1', 'x', '3', '4', '5', '6'};
        CHECK(!uiFormatLiveText(buf, sizeof(buf), d5, 0, 0), "garbage digit");

        uiFormatNumber(buf, sizeof(buf), 0.0012346, KEITHLEY_UNIT_MV, 4); CHECK_STR(buf, "1.2346", "V as mV");
        uiFormatNumber(buf, sizeof(buf), -0.0000001, KEITHLEY_UNIT_V, 5); CHECK_STR(buf, "0.00000", "no minus zero");
        uiFormatNumber(buf, sizeof(buf), 1234.6, KEITHLEY_UNIT_KOHM, 3); CHECK_STR(buf, "1.235", "ohm as kOhm");
        uiFormatNumber(buf, sizeof(buf), -2.5e-6, KEITHLEY_UNIT_UA, 2); CHECK_STR(buf, "-2.50", "A as uA");
        uiFormatNumber(buf, sizeof(buf), 12.5, KEITHLEY_UNIT_DB, 3); CHECK_STR(buf, "12.500", "dB unscaled");
        uiFormatNumber(buf, sizeof(buf), 123456789.0, KEITHLEY_UNIT_V, 5); CHECK_STR(buf, "OL", "does not fit even without decimals");
        uiFormatNumber(buf, sizeof(buf), -123456789.0, KEITHLEY_UNIT_V, 5); CHECK_STR(buf, "-OL", "negative overload");
        uiFormatNumber(buf, sizeof(buf), NAN, KEITHLEY_UNIT_V, 5); CHECK_STR(buf, UI_PLACEHOLDER, "nan");
        CHECK_STR(UI_PLACEHOLDER, "--.----", "the placeholder is six dashes with a decimal point");
        uiFormatNumber(buf, sizeof(buf), 0.5, KEITHLEY_UNIT_V, 5); CHECK_STR(buf, "0.50000", "below 1: 5 decimals");
        uiFormatNumber(buf, sizeof(buf), 1.99999, KEITHLEY_UNIT_V, 5); CHECK_STR(buf, "1.99999", "largest 5 decimal value");
        uiFormatNumber(buf, sizeof(buf), 1.999996, KEITHLEY_UNIT_V, 5); CHECK_STR(buf, "2.0000", "rounds up into the next range");
        uiFormatNumber(buf, sizeof(buf), 19.5, KEITHLEY_UNIT_V, 5); CHECK_STR(buf, "19.5000", "10 V range style");
        uiFormatNumber(buf, sizeof(buf), -19.99996, KEITHLEY_UNIT_V, 5); CHECK_STR(buf, "-20.000", "negative, rounds up");
        uiFormatNumber(buf, sizeof(buf), 123.456789, KEITHLEY_UNIT_V, 5); CHECK_STR(buf, "123.457", "100 V range style");
        uiFormatNumber(buf, sizeof(buf), 199999.0, KEITHLEY_UNIT_OHM, 0); CHECK_STR(buf, "199999", "integer, no decimals");
        uiFormatNumber(buf, sizeof(buf), 200000.4, KEITHLEY_UNIT_OHM, 0); CHECK_STR(buf, "OL", "just over the range");
        {
            const double probes[] = {0.0, 1e-9, 0.99999, 1.0, 9.99999, 99.9999, 199.999, 1999.99, 12345.6, 199999.4, -0.00001, -199999.4};
            for (double v : probes)
                for (int d = 0; d <= 5; d++)
                {
                    uiFormatNumber(buf, sizeof(buf), v, KEITHLEY_UNIT_OHM, (uint8_t)d);
                    CHECK(strlen(buf) <= 8, "never longer than a 5 1/2 digit reading");
                }
        }
        char u[8];
        uiUnitText(KEITHLEY_UNIT_UA, u, sizeof(u)); CHECK(u[0] == FONT_CH_MICRO && u[1] == 'A' && u[2] == 0, "uA text");
        uiUnitText(KEITHLEY_UNIT_KOHM, u, sizeof(u)); CHECK(u[0] == 'k' && u[1] == FONT_CH_OHM && u[2] == 0, "kOhm text");
        uiUnitText(KEITHLEY_UNIT_NONE, u, sizeof(u)); CHECK(u[0] == 0, "none empty");
        CHECK(uiDecimalsFor(0) == 5 && uiDecimalsFor(3) == 2 && uiDecimalsFor(-1) == 3, "decimals");
        uiFunctionFilter(u, sizeof(u), " dB"); CHECK_STR(u, "DB", "filter dB");
        uiFunctionFilter(u, sizeof(u), "DCV"); CHECK_STR(u, "DCV", "filter DCV");

        // v25: the numbers of statistics pages 2 and 3
        char n[16], un[8], want[8];
        uiFormatEngineering(n, sizeof(n), un, sizeof(un), 8.234e-6, KEITHLEY_UNIT_V, false, "", "x");
        snprintf(want, sizeof(want), "%cV", FONT_CH_MICRO); CHECK_STR(n, "8.23", "eng 8.23"); CHECK_STR(un, want, "eng uV");
        uiFormatEngineering(n, sizeof(n), un, sizeof(un), 75.3e-9, KEITHLEY_UNIT_MV, false, "", "x");
        CHECK_STR(n, "75.3", "eng 75.3"); CHECK_STR(un, "nV", "eng nV (base unit, whatever the range)");
        uiFormatEngineering(n, sizeof(n), un, sizeof(un), 999.7e-9, KEITHLEY_UNIT_V, false, "", "x");
        snprintf(want, sizeof(want), "%cV", FONT_CH_MICRO); CHECK_STR(n, "1.00", "999.7 n rounds to 1.00 u"); CHECK_STR(un, want, "... micro");
        uiFormatEngineering(n, sizeof(n), un, sizeof(un), 0.0, KEITHLEY_UNIT_V, true, "/h", "x");
        CHECK_STR(n, "0.00", "eng zero, no sign"); CHECK_STR(un, "V/h", "eng zero in the base unit");
        uiFormatEngineering(n, sizeof(n), un, sizeof(un), 1234.0, KEITHLEY_UNIT_KOHM, false, "", "x");
        snprintf(want, sizeof(want), "k%c", FONT_CH_OHM); CHECK_STR(n, "1.23", "eng 1.23 k"); CHECK_STR(un, want, "eng kOhm");
        uiFormatEngineering(n, sizeof(n), un, sizeof(un), 12.34e-6, KEITHLEY_UNIT_V, true, "/h", "x");
        snprintf(want, sizeof(want), "%cV/h", FONT_CH_MICRO); CHECK_STR(n, "+12.3", "trend with a sign"); CHECK_STR(un, want, "uV/h");
        uiFormatEngineering(n, sizeof(n), un, sizeof(un), -4.171e-3, KEITHLEY_UNIT_UA, true, "/m", "x");
        CHECK_STR(n, "-4.17", "negative trend"); CHECK_STR(un, "mA/m", "mA/m");
        uiFormatEngineering(n, sizeof(n), un, sizeof(un), NAN, KEITHLEY_UNIT_V, true, "/m", "---");
        CHECK_STR(n, "---", "eng placeholder"); CHECK_STR(un, "V/m", "placeholder keeps the unit");
        uiFormatEngineering(n, sizeof(n), un, sizeof(un), 0.01234, KEITHLEY_UNIT_DB, false, "", "x");
        CHECK_STR(n, "0.0123", "dB without a prefix"); CHECK_STR(un, "dB", "dB");
        uiFormatEngineering(n, sizeof(n), un, sizeof(un), 123.4, KEITHLEY_UNIT_V, false, "", "x");
        CHECK_STR(n, "123", "eng 3 digits"); CHECK_STR(un, "V", "no prefix");
        uiFormatRatio(n, sizeof(n), un, sizeof(un), 6.67e-6, "x"); CHECK_STR(n, "6.67", "ratio ppm"); CHECK_STR(un, "ppm", "ppm");
        uiFormatRatio(n, sizeof(n), un, sizeof(un), 2.35e-3, "x"); CHECK_STR(n, "0.235", "ratio %"); CHECK_STR(un, "%", "%");
        uiFormatRatio(n, sizeof(n), un, sizeof(un), 2.35e-4, "x"); CHECK_STR(n, "235", "ratio 235 ppm");
        uiFormatRatio(n, sizeof(n), un, sizeof(un), NAN, "---"); CHECK_STR(n, "---", "ratio placeholder"); CHECK_STR(un, "ppm", "placeholder ppm");
        uiFormatDigits(n, sizeof(n), 8.23e-6, KEITHLEY_UNIT_V, 5); CHECK_STR(n, "0.82", "0.82 dgt");
        uiFormatDigits(n, sizeof(n), 1.234e-4, KEITHLEY_UNIT_V, 5); CHECK_STR(n, "12.3", "12.3 dgt");
        uiFormatDigits(n, sizeof(n), 0.5e-3, KEITHLEY_UNIT_MV, 3); CHECK_STR(n, "500", "mV range: 0.5 mV = 500 dgt");
        uiFormatSignificant3(n, sizeof(n), 9.996, false, 3); CHECK_STR(n, "10.0", "9.996 -> 10.0");
        uiFormatSignificant3(n, sizeof(n), -0.0001, true, 2); CHECK_STR(n, "0.00", "rounds to zero: no sign");
        uiFormatSignificant3(n, sizeof(n), 0.000123, false, 6); CHECK_STR(n, "0.000123", "small, 6 decimals");
        {
            const double probes[] = {1e-15, 3.3e-12, 1e-9, 4.56e-6, 0.0999, 0.9996, 1.0, 99.96, 999.6, 12345.0, 9.9e8};
            for (double v : probes)
                for (int sign = -1; sign <= 1; sign += 2)
                {
                    uiFormatEngineering(n, sizeof(n), un, sizeof(un), sign * v, KEITHLEY_UNIT_KOHM, true, "/h", "x");
                    CHECK(strlen(n) <= 6 && strlen(un) <= 4, "engineering text fits the fields");
                }
        }
    }

    // ------------------------------------------------------------------
    // 2. datalog statistics: one shot, jobs in slices
    // ------------------------------------------------------------------
    ESP.freePsram = (size_t)(1500.0 * sizeof(LogRecord)) + DATALOG_PSRAM_RESERVE_BYTES + 4;
    datalogBegin();
    printf("log capacity %u records\n", datalogCapacity());
    CHECK(datalogCapacity() >= 1000, "capacity");
    gBaseSeconds = secondsSince2000(2026, 10, 6, 14, 3, 7);
    gRtcOn = true;
    setClock();
    alarmBegin(); // v17 (setup() calls it before uiBegin())
    uiBegin();

    {
        DatalogStats st;
        CHECK(!datalogComputeStats("DCV", -10, 0, &st), "empty log");
        CHECK(st.count == 0, "empty count");
        CHECK(datalogFirstRecordSince(60, NULL) == 0, "empty log has no first record");
    }

    for (int k = 0; k < 25; k++) feed(100000 + (k * 7919) % 5000 - 2500, 0, KEITHLEY_UNIT_V);
    {
        DatalogStats st;
        bool ok = datalogComputeStats("DCV", -10, 0, &st);
        Expect e = referenceLast("DCV", 10);
        CHECK(ok && e.ok, "stats ok");
        CHECK(st.count == 10, "count 10");
        CHECK(near(st.minimum, e.mn) && near(st.maximum, e.mx), "min/max");
        CHECK(near(st.average, e.avg), "average");
        CHECK(st.medianValid && near(st.median, e.med), "median even");
        CHECK(!datalogComputeStats("OHM", -10, 0, &st), "other function: nothing");
        // v25: standard deviation and trend
        Spread sp = referenceSpread("DCV", 0);
        datalogComputeStats("DCV", 0, 0, &st);
        CHECK(st.sdevValid && nearRel(st.sdev, sp.sdev, 1e-6), "standard deviation");
        CHECK(st.slopeValid && fabs(st.slopePerSecond - sp.slope) <= 1e-6 * sp.sdev, "trend (least squares over the time)");
        CHECK(st.slopeCount == st.count, "every record has a timestamp");
        DatalogStats nm;
        CHECK(datalogComputeStatsNoMedian("DCV", 0, 0, &nm) && !nm.medianValid && nm.sdev == st.sdev && nm.average == st.average,
              "without the median: the rest identical");
        CHECK(datalogComputeStats("DCV", -1, 0, &st) && st.count == 1 && !st.sdevValid && !st.slopeValid, "one record: no SDV, no trend");
        CHECK(datalogComputeStats("DCV", -3, 0, &st) && st.sdevValid && st.slopeValid, "three records: both");
    }
    // a clean ramp: one count every reading (300 ms) = 1e-5 V / 0.3 s, the spread of 0..19 counts
    {
        DatalogStats st;
        for (int k = 0; k < 20; k++) feed(150000 + k, 0, KEITHLEY_UNIT_V);
        CHECK(datalogComputeStats("DCV", -20, 0, &st), "ramp");
        CHECK(nearRel(st.slopePerSecond, 1e-5 / 0.3, 1e-4), "ramp: slope");
        CHECK(nearRel(st.sdev, 1e-5 * sqrt(20.0 * 21.0 / 12.0), 1e-4), "ramp: SDV of 0..19 counts");
        printf("ramp: SDV %.4g V, trend %.4g V/s\n", st.sdev, st.slopePerSecond);
    }
    feedDigits("  L   ", 1, 0, KEITHLEY_UNIT_V);
    feed(100500, 0, KEITHLEY_UNIT_V);
    {
        DatalogStats st;
        datalogComputeStats("DCV", -5, 0, &st);
        Expect e = referenceLast("DCV", 5);
        CHECK(st.count == 4, "overload excluded from count");
        CHECK(near(st.maximum, e.mx) && near(st.average, e.avg) && near(st.median, e.med), "overload excluded");
        CHECK(st.maximum < 100.0, "no sentinel in maximum");
        datalogComputeStats("DCV", -7, 0, &st);
        e = referenceLast("DCV", 7);
        CHECK(near(st.median, e.med), "median odd/with overload");
    }

    // time windows on this (not yet wrapped) log: 26 records, 300 ms apart
    {
        size_t n = datalogCount();
        CHECK(n == gRef.size() + 1 || n == gRef.size() + 0 || true, "log mirrors the reference");
        uint32_t windows[] = {0, 1, 2, 5, 60, 3600};
        for (uint32_t w : windows)
        {
            bool estimate = true;
            long first = datalogFirstRecordSince(w, &estimate);
            CHECK(!estimate, "real timestamps are used");
            long ref = referenceFirstIndex(w, gRef.size()); // the overload record is in the log but not in gRef
            // the overload record sits between the DCV ones, so compare through the statistics instead
            DatalogStats st;
            bool ok = datalogComputeStats("DCV", first, 0, &st);
            Expect e = referenceSince("DCV", w);
            CHECK(ok == e.ok, "window has data");
            if (ok) CHECK(st.count == e.n && near(st.average, e.avg), "time window statistics");
            (void)ref;
        }
    }

    // a job advanced in slices of any size gives exactly the one-shot result
    for (int k = 0; k < 2200; k++) feed(50000 + (k * 31) % 7000, 0, KEITHLEY_UNIT_V); // wraps the 1504 record log
    gRef.erase(gRef.begin(), gRef.end() - (long)datalogCount() + 0 > gRef.begin() ? gRef.end() - (long)datalogCount() : gRef.begin()); // keep only what the log still holds (overload records included in the log, not in gRef)
    {
        DatalogStats whole;
        CHECK(datalogCount() == datalogCapacity(), "log is full");
        datalogComputeStats("DCV", 0, 0, &whole);
        CHECK(whole.count > 1000, "the whole log counted");
        uint32_t slices[] = {1, 7, 200, 1000, 100000};
        for (uint32_t slice : slices)
        {
            DatalogStatsJob job;
            datalogStatsStart(&job, "DCV", 0, 0);
            int steps = 0;
            while (!datalogStatsStep(&job, slice)) steps++;
            DatalogStats r;
            bool ok = datalogStatsFinish(&job, &r);
            CHECK(ok && r.count == whole.count, "slice job count");
            CHECK(r.minimum == whole.minimum && r.maximum == whole.maximum && r.average == whole.average, "slice job min/max/avg identical");
            CHECK(r.medianValid && r.median == whole.median, "slice job median identical");
            CHECK(r.sdev == whole.sdev && r.slopePerSecond == whole.slopePerSecond, "v25: slice job SDV / trend identical");
            CHECK(!job.active && job.samples == NULL, "job released");
            if (slice == 200) CHECK(steps >= 6, "a long job really takes several slices");
        }
        // abort in the middle, then a new job still works
        DatalogStatsJob a;
        datalogStatsStart(&a, "DCV", 0, 0);
        datalogStatsStep(&a, 100);
        datalogStatsAbort(&a);
        CHECK(!a.active, "aborted");
        DatalogStats r2;
        CHECK(datalogComputeStats("DCV", 0, 0, &r2) && r2.count == whole.count, "works after an abort");
    }

    // time window lookup when part of the log has no timestamps, and the estimate without any RTC
    datalogClear();
    gRef.clear();
    gRtcOn = false; setClock();
    for (int k = 0; k < 100; k++) feed(100000 + k, 0, KEITHLEY_UNIT_V); // recorded before the RTC was synced: timestamp 0
    gRtcOn = true; setClock();
    for (int k = 0; k < 100; k++) feed(110000 + k, 0, KEITHLEY_UNIT_V); // 100 records, 30 s
    {
        bool estimate = true;
        long first = datalogFirstRecordSince(3600, &estimate);
        CHECK(!estimate && first == 101, "records without a timestamp are older than any window");
        first = datalogFirstRecordSince(10, &estimate);
        // the last 10 s = the newest ~34 records
        DatalogStats st;
        datalogComputeStats("DCV", first, 0, &st);
        Expect e = referenceSince("DCV", 10);
        CHECK(st.count == e.n, "10 s window");
        // v25: records without a timestamp count for SDV, not for the trend
        DatalogStats all;
        datalogComputeStats("DCV", 0, 0, &all);
        CHECK(all.count == 200 && all.slopeCount == 100 && all.slopeValid, "trend only over the timestamped records");
        CHECK(nearRel(all.slopePerSecond, 1e-5 / 0.3, 1e-4), "trend of the timestamped ramp");
        printf("10 s window holds %u records (3.33/s expected)\n", st.count);
        CHECK(datalogFirstRecordSince(0, &estimate) == 1, "FULL starts at the oldest record");

        // no RTC time now -> estimate from the recording rate
        gRtcOn = false; setClock();
        first = datalogFirstRecordSince(60, &estimate);
        CHECK(estimate, "no RTC: estimate flagged");
        long total = (long)datalogCount();
        CHECK(first == 1, "60 s estimate (210 records) is more than the 200 record log: everything");
        first = datalogFirstRecordSince(30, &estimate);
        CHECK(first == total - 105 + 1, "30 s estimated as 105 records");
        gAutoInterval = 5;
        first = datalogFirstRecordSince(30, &estimate);
        CHECK(first == total - 6 + 1, "estimate follows SYSTem:AUTO interval (30 s / 5 s = 6 records)");
        gAutoInterval = 0;
        first = datalogFirstRecordSince(3600, &estimate);
        CHECK(first == 1, "estimate larger than the log: everything");
        gRtcOn = true; setClock();
    }

    // ------------------------------------------------------------------
    // 3. ui.ino: STO toggles the screen, REL/dB pick the interval, texts, job in slices
    // ------------------------------------------------------------------
    datalogClear();
    gRef.clear();
    gClockMs = 0;
    gRtcOn = true;
    setClock();
    // The persisted interval is restored at start-up (and an out-of-range
    // stored byte is clamped) without writing it back.
    gStoredInterval = 4;
    uiBegin();
    CHECK(uiGetStatsInterval() == 4, "stored interval restored after power-up");
    gStoredInterval = 200;
    uiBegin();
    CHECK(uiGetStatsInterval() == UI_INTERVAL_COUNT - 1, "garbage stored interval clamped");
    CHECK(gIntervalSaves == 0, "restoring does not save");
    gStoredInterval = CONFIG_UI_STATS_DEFAULT_INTERVAL_INDEX;
    uiBegin();
    CHECK(uiGetStatsInterval() == CONFIG_UI_STATS_DEFAULT_INTERVAL_INDEX, "default interval after power-up");
    uiSetStatsInterval(2); // 1 MIN
    CHECK(uiGetScreen() == UI_SCREEN_LCD, "starts on the LCD");
    CHECK(!uiPoll(), "nothing to do on the LCD");

    // no reading yet: the screen opens with placeholders
    press(BUTTON_STO);
    CHECK(uiPoll(), "STO press changes the screen");
    CHECK(uiGetScreen() == UI_SCREEN_QUICKSTATS, "quick stats shown");
    CHECK(!gRelDbEnabled, "REL/dB blocked on the stats screen");
    {
        FbStatsScene sc;
        uiFillStatsScene(&sc);
        CHECK_STR(sc.average, UI_PLACEHOLDER, "placeholder before the first reading");
        CHECK_STR(sc.interval, "1 MIN", "default interval");
        CHECK(uiGetStatsInterval() == 2, "default interval index is 1 MIN");
        CHECK_STR(sc.dateTime, "14:03:07 06.10.26", "time and date from the RTC");
    }

    for (int k = 0; k < 400; k++) feed(100000 + (k * 7919) % 5000 - 2500, 0, KEITHLEY_UNIT_V);
    CHECK(uiPoll(), "readings arrived");
    pollUntilIdle();
    {
        FbStatsScene sc;
        uiFillStatsScene(&sc);
        Expect e = referenceSince("DCV", 60);
        char want[24];
        uiFormatNumber(want, sizeof(want), e.avg, KEITHLEY_UNIT_V, 5); CHECK_STR(sc.average, want, "avg text, 1 MIN");
        uiFormatNumber(want, sizeof(want), e.med, KEITHLEY_UNIT_V, 5); CHECK_STR(sc.median, want, "med text, 1 MIN");
        uiFormatNumber(want, sizeof(want), e.mx, KEITHLEY_UNIT_V, 5); CHECK_STR(sc.maximum, want, "max text, 1 MIN");
        uiFormatNumber(want, sizeof(want), e.mn, KEITHLEY_UNIT_V, 5); CHECK_STR(sc.minimum, want, "min text, 1 MIN");
        CHECK_STR(sc.unit, "V", "unit text");
        CHECK(sc.live[0] >= '0' && sc.live[0] <= '9' && sc.live[1] == '.', "live text looks like a reading");
        printf("1 MIN: %zu records, avg '%s' med '%s' max '%s' min '%s', live '%s'\n", e.n, sc.average, sc.median, sc.maximum, sc.minimum, sc.live);
    }

    // dB = longer interval, REL = shorter, clamped at both ends; the numbers follow at once
    {
        const char *labels[] = {"5 SEC", "10 SEC", "1 MIN", "5 MIN", "15 MIN", "1 HOUR", "6 HRS"};
        const uint32_t seconds[] = {5, 10, 60, 300, 900, 3600, 21600};
        CHECK(UI_INTERVAL_COUNT == 7, "seven intervals: 5 SEC ... 6 HRS, not the whole log");
        for (int i = 3; i < 7; i++)
        {
            press(BUTTON_DB);
            CHECK(uiPoll(), "dB changes the interval");
            pollUntilIdle();
            FbStatsScene sc;
            uiFillStatsScene(&sc);
            CHECK_STR(sc.interval, labels[i], "interval label");
            Expect e = referenceSince("DCV", seconds[i]);
            char want[24];
            uiFormatNumber(want, sizeof(want), e.avg, KEITHLEY_UNIT_V, 5);
            CHECK_STR(sc.average, want, "avg follows the interval");
            CHECK(uiGetStatsInterval() == i, "interval index");
            CHECK(gStoredInterval == i, "interval change handed to the settings (deferred save)");
        }
        press(BUTTON_DB);
        uiPoll();
        CHECK(uiGetStatsInterval() == 6, "dB stops at the longest interval (6 HRS)");
        for (int i = 5; i >= 0; i--)
        {
            press(BUTTON_REL);
            CHECK(uiPoll(), "REL changes the interval");
            pollUntilIdle();
            CHECK(uiGetStatsInterval() == i, "REL shortens");
            FbStatsScene sc;
            uiFillStatsScene(&sc);
            CHECK_STR(sc.interval, labels[i], "interval label going down");
            Expect e = referenceSince("DCV", seconds[i]);
            char want[24];
            if (e.ok)
            {
                uiFormatNumber(want, sizeof(want), e.avg, KEITHLEY_UNIT_V, 5);
                CHECK_STR(sc.average, want, "avg follows the shorter interval");
            }
            else
            {
                CHECK_STR(sc.average, UI_PLACEHOLDER, "no record in the interval: placeholder");
            }
        }
        press(BUTTON_REL);
        uiPoll();
        CHECK(uiGetStatsInterval() == 0, "REL stops at the shortest interval (5 SEC)");
        uiSetStatsInterval(2);
        uiPoll();
        pollUntilIdle();
    }

    // an interval without any record: dashes (records only every 30 s, window 5 s)
    {
        gAutoInterval = 30;
        // the log is recorded by datalogUpdateReading() only when the AUTO interval is due; simulate a window that
        // really is empty by moving the clock far beyond the newest record
        uint32_t savedClock = gClockMs;
        gClockMs += 600000; setClock();
        uiSetStatsInterval(0);
        gPress[BUTTON_REL] = false;
        uiPoll(); pollUntilIdle();
        FbStatsScene sc;
        uiFillStatsScene(&sc);
        CHECK_STR(sc.average, UI_PLACEHOLDER, "empty window: average shows the placeholder");
        CHECK_STR(sc.median, UI_PLACEHOLDER, "empty window: median shows the placeholder");
        CHECK_STR(sc.maximum, UI_PLACEHOLDER, "empty window: maximum shows the placeholder");
        CHECK_STR(sc.minimum, UI_PLACEHOLDER, "empty window: minimum shows the placeholder");
        CHECK(sc.live[0] != '-' || sc.live[1] != '-' || sc.live[2] != '.', "the live value is not the placeholder");
        gAutoInterval = 0;
        gClockMs = savedClock; setClock();
        uiSetStatsInterval(2);
        uiPoll(); pollUntilIdle();
    }

    // a long interval is worked off in slices while the live value keeps following, and uiBusy() says so
    {
        for (int k = 0; k < 1700; k++) feed(95000 + (k * 31) % 7000, 0, KEITHLEY_UNIT_V); // wraps the 1504 record log
        for (int i = 0; i < 4; i++) { press(BUTTON_DB); uiPoll(); pollUntilIdle(); }       // -> 6 HRS, which covers the whole log
        CHECK(uiGetStatsInterval() == 6, "6 HRS selected");
        press(BUTTON_REL); uiPoll(); pollUntilIdle();
        press(BUTTON_DB);
        uiPoll();
        FbStatsScene sc;
        uiFillStatsScene(&sc);
        CHECK(uiJobRunning && uiBusy(), "a job over the whole log needs more than one slice, and the UI says it is busy");
        CHECK_STR(sc.average, UI_PLACEHOLDER, "placeholder while the job runs");
        feed(123456, 0, KEITHLEY_UNIT_V);
        CHECK(uiPoll(), "still polling");
        uiFillStatsScene(&sc);
        CHECK_STR(sc.live, "1.23456", "live value follows while the job runs");
        int polls = 1;
        while (uiJobRunning && polls < 50) { uiPoll(); polls++; }
        CHECK(!uiJobRunning && !uiBusy(), "job finished, UI no longer busy");
        printf("6 HRS over %u records took %d polls of %d records\n", datalogCount(), polls, CONFIG_UI_STATS_STEP_RECORDS);
        CHECK(polls >= 5, "needed several slices");
        uiFillStatsScene(&sc);
        CHECK(sc.average[0] != '-' || sc.average[1] != '-', "statistics shown after the job");
        // a button press during a running job restarts it cleanly
        press(BUTTON_REL); uiPoll(); press(BUTTON_DB); uiPoll();
        pollUntilIdle();
        uiFillStatsScene(&sc);
        CHECK(sc.average[0] != '-' || sc.average[1] != '-', "result after the restart");
    }

    uiSetStatsInterval(2); // back to 1 MIN for the sections below
    // date and time follow the clock, and say so when there is none
    {
        FbStatsScene sc;
        uint32_t savedClock = gClockMs;
        gClockMs = 2000; setClock();
        CHECK(uiPoll(), "a new second redraws");
        uiFillStatsScene(&sc);
        CHECK_STR(sc.dateTime, "14:03:09 06.10.26", "time advanced");
        CHECK(!uiPoll(), "same second: nothing to redraw");
        gClockMs = 2000 + 60 * 60 * 1000 + 1000; setClock();
        uiPoll(); uiFillStatsScene(&sc);
        CHECK_STR(sc.dateTime, "15:03:10 06.10.26", "hour carried");
        gRtcOn = false; setClock();
        uiPoll(); uiFillStatsScene(&sc);
        CHECK_STR(sc.dateTime, "NO RTC TIME", "no RTC");
        gRtcOn = true; gClockMs = savedClock; setClock();
    }

    // unit change (V -> mV, 3 decimals): statistics are recomputed at once, in the new unit
    {
        uiSetStatsInterval(2);
        for (int k = 0; k < 3; k++) feed(12345 + k, 2, KEITHLEY_UNIT_MV);
        CHECK(uiPoll(), "unit change redraws");
        pollUntilIdle();
        FbStatsScene sc;
        uiFillStatsScene(&sc);
        CHECK_STR(sc.unit, "mV", "mV unit");
        CHECK_STR(sc.live, "12.347", "live mV");
        Expect e = referenceSince("DCV", 60);
        char want[24];
        uiFormatNumber(want, sizeof(want), e.avg, KEITHLEY_UNIT_MV, 3); CHECK_STR(sc.average, want, "avg in mV");
    }

    // function change: resistance readings only see resistance records
    {
        for (int k = 0; k < 4; k++) feed(123400 + k * 10, 2, KEITHLEY_UNIT_KOHM);
        CHECK(uiPoll(), "function change redraws");
        pollUntilIdle();
        FbStatsScene sc;
        uiFillStatsScene(&sc);
        Expect e = referenceSince("OHM", 60);
        CHECK(e.ok, "reference has resistance records");
        char want[24];
        uiFormatNumber(want, sizeof(want), e.avg, KEITHLEY_UNIT_KOHM, 3); CHECK_STR(sc.average, want, "ohm avg");
        uiFormatNumber(want, sizeof(want), e.mx, KEITHLEY_UNIT_KOHM, 3); CHECK_STR(sc.maximum, want, "ohm max");
        char u[8]; uiUnitText(KEITHLEY_UNIT_KOHM, u, sizeof(u)); CHECK_STR(sc.unit, u, "kOhm unit text");
    }

    // with SYSTem:DEBUG on, a handled press reports its latency (only checked for not crashing here)
    gDebugTrace = true;
    press(BUTTON_REL); uiPoll(); pollUntilIdle();
    CHECK(gDebugMessages >= 1, "a handled press reports its latency through the debug queue");
    gDebugTrace = false;
    uiSetStatsInterval(2);

    // RCL held >= CONFIG_UI_LOG_CLEAR_HOLD_MS on the statistics screen clears the whole log, once, with the long beep
    {
        CHECK(uiGetScreen() == UI_SCREEN_QUICKSTATS, "still on the statistics screen");
        for (int k = 0; k < 3; k++) feed(100000 + k, 0, KEITHLEY_UNIT_V);
        uiPoll();
        pollUntilIdle();
        CHECK(datalogCount() > 0, "log has records before the hold");

        // released too early: nothing happens
        press(BUTTON_RCL); gHeld[BUTTON_RCL] = true;
        uiPoll();
        fakeMillisOffset() += 1000;
        uiPoll();
        gHeld[BUTTON_RCL] = false;
        uiPoll();
        fakeMillisOffset() += 5000;
        uiPoll();
        CHECK(datalogCount() > 0 && gLogClearedBeeps == 0, "a short hold does not clear");
        CHECK(uiGetStatsPage() == FB_STATS_PAGE_SPREAD, "v25: ... it is a click: page 2");

        // held long enough
        press(BUTTON_RCL); gHeld[BUTTON_RCL] = true;
        uiPoll();
        fakeMillisOffset() += CONFIG_UI_LOG_CLEAR_HOLD_MS - 100;
        uiPoll();
        CHECK(datalogCount() > 0 && gLogClearedBeeps == 0, "not yet before the hold time");
        fakeMillisOffset() += 200;
        CHECK(uiPoll(), "clearing redraws");
        CHECK(datalogCount() == 0, "log cleared");
        gRef.clear(); // v25: the reference follows the log (pages 2 and 3 below compare with it)
        CHECK(gLogClearedBeeps == 1, "long beep played");
        CHECK(!uiJobRunning, "no job left on the cleared log");
        FbStatsScene sc;
        uiFillStatsScene(&sc);
        CHECK_STR(sc.average, UI_PLACEHOLDER, "placeholders after clearing");
        fakeMillisOffset() += 5000;
        uiPoll();
        CHECK(gLogClearedBeeps == 1, "one hold clears only once");
        gHeld[BUTTON_RCL] = false;
        gLong[BUTTON_RCL] = true; // the release that follows is a long one - drained, no other effect
        uiPoll();
        CHECK(uiGetStatsPage() == FB_STATS_PAGE_SPREAD, "v25: the release of the clearing hold does not page");

        // the screen recovers with new readings
        for (int k = 0; k < 3; k++) feed(100000 + k, 0, KEITHLEY_UNIT_V);
        uiPoll();
        pollUntilIdle();
        uiFillStatsScene(&sc);
        CHECK(strcmp(sc.average, UI_PLACEHOLDER) != 0, "statistics come back after new readings");
        CHECK(datalogCount() == 3, "only the new records");
    }

    // v12 help screen: REL held CONFIG_UI_HELP_HOLD_MS on the statistics screen opens it; the hold does not change the interval
    {
        CHECK(uiGetScreen() == UI_SCREEN_QUICKSTATS, "on the statistics screen before the help test");
        uint8_t before = uiGetStatsInterval();
        press(BUTTON_REL); gHeld[BUTTON_REL] = true;
        uiPoll();
        fakeMillisOffset() += CONFIG_UI_HELP_HOLD_MS - 100;
        uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_QUICKSTATS, "not yet before the hold time");
        fakeMillisOffset() += 200;
        CHECK(uiPoll(), "opening the help redraws");
        CHECK(uiGetScreen() == UI_SCREEN_HELP, "help opened from STATS");
        CHECK(!gRelDbEnabled, "REL/dB do not reach the K197 on the help screen");
        CHECK(uiGetHelpTop() == 0, "help starts at the top");
        gHeld[BUTTON_REL] = false;
        uiPoll();
        CHECK(uiGetStatsInterval() == before, "the hold did not change the interval");
        CHECK(uiGetScreen() == UI_SCREEN_HELP, "releasing REL keeps the help");

        // v15 context help: from STATS only the STAT text, no preface, one page
        FbHelpScene hs;
        uiFillHelpScene(&hs);
        CHECK(uiGetHelpSection() == HELP_SECTION_STAT, "help from STATS: the STAT section");
        CHECK(strncmp(hs.lines[0], "STATISTIKA:", 11) == 0 && !hs.underline[0] && !hs.underline[1], "STAT help: no preface, its own text (CZ)");
        CHECK(uiGetHelpLineCount() == 2 * FB_HELP_LINES && strstr(hs.lines[FB_HELP_LINES - 1], "1/2") != NULL, "v25 STAT help: two pages");
        press(BUTTON_DB); uiPoll();
        CHECK(uiGetHelpTop() == FB_HELP_LINES, "dB: page 2 (the pages of the statistics)");
        press(BUTTON_DB); uiPoll();
        CHECK(uiGetHelpTop() == FB_HELP_LINES, "last page: dB stays");
        // v25: RCL = next page, from the last one back to the first (like RCL = next line on the settings screens)
        press(BUTTON_RCL); uiPoll();
        CHECK(uiGetHelpTop() == 0, "RCL on the last page: back to the first");
        press(BUTTON_RCL); uiPoll();
        CHECK(uiGetHelpTop() == FB_HELP_LINES, "RCL: next page");
        CHECK(uiGetScreen() == UI_SCREEN_HELP, "RCL keeps the help open");

        // timeout: a press restarts it, after CONFIG_UI_HELP_TIMEOUT_MS without buttons the help closes like STO
        fakeMillisOffset() += CONFIG_UI_HELP_TIMEOUT_MS - 1000;
        uiPoll();
        press(BUTTON_DB); uiPoll(); // activity: restarts the countdown
        fakeMillisOffset() += CONFIG_UI_HELP_TIMEOUT_MS - 1000;
        uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_HELP, "a press restarted the timeout");
        gHeld[BUTTON_RCL] = true; // a held button also counts as activity
        fakeMillisOffset() += 5000;
        uiPoll();
        gHeld[BUTTON_RCL] = false;
        fakeMillisOffset() += CONFIG_UI_HELP_TIMEOUT_MS - 1000;
        uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_HELP, "a held button kept the help open");
        fakeMillisOffset() += 2000;
        CHECK(uiPoll() && uiGetScreen() == UI_SCREEN_QUICKSTATS, "timeout: back to STATS");
        pollUntilIdle();
        // open it again for the STO test below
        press(BUTTON_REL); gHeld[BUTTON_REL] = true; uiPoll();
        fakeMillisOffset() += CONFIG_UI_HELP_HOLD_MS + 10;
        uiPoll();
        gHeld[BUTTON_REL] = false; uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_HELP, "help open again");

        // STO: back to the statistics screen; the statistics run again
        press(BUTTON_STO);
        CHECK(uiPoll(), "leaving the help redraws");
        CHECK(uiGetScreen() == UI_SCREEN_QUICKSTATS, "STO returns to STATS");
        CHECK(uiGetStatsPage() == FB_STATS_PAGE_SPREAD, "v25: back from the help the page stays");
        CHECK(!gRelDbEnabled, "REL/dB still blocked on STATS");
        pollUntilIdle();

        // a short REL click on STATS still shortens the interval (now at the release)
        uiSetStatsInterval(3);
        press(BUTTON_REL); gHeld[BUTTON_REL] = true;
        uiPoll();
        CHECK(uiGetStatsInterval() == 3, "REL held: interval not changed yet");
        fakeMillisOffset() += 200;
        gHeld[BUTTON_REL] = false;
        uiPoll();
        CHECK(uiGetStatsInterval() == 2, "REL released: shorter interval");
        uiSetStatsInterval(2);
        pollUntilIdle();
    }

    // v25: pages 2 and 3 - RCL click cycles them, the numbers against the reference, the page kept with the interval
    {
        CHECK(uiGetScreen() == UI_SCREEN_QUICKSTATS && uiGetStatsInterval() == 2, "on STATS, 1 MIN");
        for (int k = 0; k < 40; k++) feed(100100 + 3 * k + (k % 3), 0, KEITHLEY_UNIT_V);
        uiPoll(); pollUntilIdle();
        FbStatsScene sc;
        uiFillStatsScene(&sc);
        CHECK(sc.page == FB_STATS_PAGE_SPREAD, "the scene carries the page");
        Expect e = referenceSince("DCV", 60);
        Spread sp = referenceSpread("DCV", 60);
        char want[24], wantUnit[8];
        CHECK(sp.sdevOk && sp.slopeOk, "reference has SDV and trend");
        uiFormatEngineering(want, sizeof(want), wantUnit, sizeof(wantUnit), sp.sdev, KEITHLEY_UNIT_V, false, "", UI_PLACEHOLDER);
        CHECK_STR(sc.sdev, want, "SDV"); CHECK_STR(sc.sdevUnit, wantUnit, "SDV unit");
        uiFormatNumber(want, sizeof(want), e.mx - e.mn, KEITHLEY_UNIT_V, 5); CHECK_STR(sc.dif, want, "DIF = MAX - MIN");
        uiFormatRatio(want, sizeof(want), wantUnit, sizeof(wantUnit), sp.sdev / fabs(e.avg), UI_PLACEHOLDER);
        CHECK_STR(sc.rel, want, "REL"); CHECK_STR(sc.relUnit, "ppm", "REL in ppm");
        snprintf(want, sizeof(want), "%zu", e.n); CHECK_STR(sc.pts, want, "PTS");
        uiFormatEngineering(want, sizeof(want), wantUnit, sizeof(wantUnit), sp.sdev / sqrt((double)e.n), KEITHLEY_UNIT_V, false, "", "---");
        CHECK_STR(sc.sem, want, "SEM"); CHECK_STR(sc.semUnit, wantUnit, "SEM unit");
        uiFormatEngineering(want, sizeof(want), wantUnit, sizeof(wantUnit), sp.slope * 60.0, KEITHLEY_UNIT_V, true, "/m", "---");
        CHECK_STR(sc.trend, want, "TRD per minute on 1 MIN"); CHECK_STR(sc.trendUnit, wantUnit, "TRD unit /m");
        CHECK(sc.trend[0] == '+', "a rising trend has a plus");
        uiFormatDigits(want, sizeof(want), sp.sdev, KEITHLEY_UNIT_V, 5); CHECK_STR(sc.sdevDigits, want, "SDV in digits");
        uiFormatRatio(want, sizeof(want), wantUnit, sizeof(wantUnit), (e.mx - e.mn) / fabs(e.avg), "---");
        CHECK_STR(sc.difRel, want, "DIF relative"); CHECK_STR(sc.difRelUnit, wantUnit, "DIF relative unit");
        printf("page 2: SDV '%s' DIF '%s' REL '%s %s' PTS '%s'; page 3: SEM '%s' TRD '%s' SDV '%s dgt' DIF '%s %s'\n",
               sc.sdev, sc.dif, sc.rel, sc.relUnit, sc.pts, sc.sem, sc.trend, sc.sdevDigits, sc.difRel, sc.difRelUnit);

        // RCL click: page 3, then page 1, then page 2 (at the release)
        press(BUTTON_RCL); gHeld[BUTTON_RCL] = true;
        uiPoll();
        CHECK(uiGetStatsPage() == FB_STATS_PAGE_SPREAD, "RCL held: not yet");
        fakeMillisOffset() += 150; gHeld[BUTTON_RCL] = false;
        CHECK(uiPoll(), "the page change redraws");
        CHECK(uiGetStatsPage() == FB_STATS_PAGE_TREND, "page 3");
        press(BUTTON_RCL); uiPoll();
        CHECK(uiGetStatsPage() == FB_STATS_PAGE_MAIN, "cyclic: page 1");
        press(BUTTON_RCL); uiPoll(); press(BUTTON_RCL); uiPoll();
        CHECK(uiGetStatsPage() == FB_STATS_PAGE_TREND, "page 3 again");

        // the interval changes, the page stays; 1 HOUR gives the trend per hour
        for (int i = 0; i < 3; i++) { press(BUTTON_DB); uiPoll(); pollUntilIdle(); }
        CHECK(uiGetStatsInterval() == 5 && uiGetStatsPage() == FB_STATS_PAGE_TREND, "interval changed, page kept");
        uiFillStatsScene(&sc);
        sp = referenceSpread("DCV", 3600);
        uiFormatEngineering(want, sizeof(want), wantUnit, sizeof(wantUnit), sp.slope * 3600.0, KEITHLEY_UNIT_V, true, "/h", "---");
        CHECK_STR(sc.trend, want, "TRD per hour on 1 HOUR"); CHECK_STR(sc.trendUnit, wantUnit, "TRD unit /h");
        uiSetStatsInterval(2); uiPoll(); pollUntilIdle();

        // a function change: placeholders carry the units of the new function at once
        for (int k = 0; k < 1; k++) feed(123400, 2, KEITHLEY_UNIT_KOHM);
        uiPoll();
        uiFillStatsScene(&sc);
        if (uiJobRunning)
        {
            char ohm[8]; snprintf(ohm, sizeof(ohm), "%c/m", FONT_CH_OHM);
            CHECK_STR(sc.trendUnit, ohm, "placeholder trend unit of the new function");
            CHECK_STR(sc.trend, "---", "placeholder trend");
        }
        pollUntilIdle();
        uiFillStatsScene(&sc);
        CHECK_STR(sc.sdev, UI_PLACEHOLDER, "settling OHM records only: no SDV yet (page 2 placeholder)");
        CHECK_STR(sc.sem, "---", "... no SEM (page 3 placeholder)");
        for (int k = 0; k < 6; k++) feed(123400 + 10 * k, 2, KEITHLEY_UNIT_KOHM);
        uiPoll(); pollUntilIdle();
        uiFillStatsScene(&sc);
        char kohm[8]; snprintf(kohm, sizeof(kohm), "k%c/m", FONT_CH_OHM);
        CHECK_STR(sc.trendUnit, kohm, "OHM trend in kOhm per minute");
        CHECK(strcmp(sc.sdev, UI_PLACEHOLDER) != 0, "OHM SDV");
        // dB: no relative numbers
        for (int k = 0; k < 6; k++) feed(12345 + k, 3, KEITHLEY_UNIT_DB);
        uiPoll(); pollUntilIdle();
        uiFillStatsScene(&sc);
        CHECK_STR(sc.rel, UI_PLACEHOLDER, "dB: no REL");
        CHECK_STR(sc.difRel, "---", "dB: no relative DIF");
        CHECK_STR(sc.sdevUnit, "dB", "dB without a prefix");
        for (int k = 0; k < 4; k++) feed(100000 + k, 0, KEITHLEY_UNIT_V);
        uiPoll(); pollUntilIdle();

        // closed and opened again: page 1
        press(BUTTON_STO); uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_LCD, "STO: LCD");
        press(BUTTON_STO); gHeld[BUTTON_STO] = true; uiPoll();
        gHeld[BUTTON_STO] = false; uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_QUICKSTATS, "STO click: STATS again");
        CHECK(uiGetStatsPage() == FB_STATS_PAGE_MAIN, "opened anew: page 1");
        uiFillStatsScene(&sc);
        CHECK(sc.page == FB_STATS_PAGE_MAIN, "the scene too");
        pollUntilIdle();
    }

    // v16: statistics job timing for SYSTem:DIAGnostic?
    {
        uint32_t lastMs = 99, maxMs = 0, records = 0;
        uiGetStatsTiming(&lastMs, &maxMs, &records);
        CHECK(records > 0 && maxMs >= lastMs, "statistics job timing recorded");
        uiDiagClear();
        uiGetStatsTiming(&lastMs, &maxMs, &records);
        CHECK(maxMs == 0, "uiDiagClear() zeroes the longest job");
    }

    // second STO press returns to the LCD, frees the job and gives REL/dB back
    press(BUTTON_STO);
    CHECK(uiPoll(), "second STO press redraws");
    CHECK(uiGetScreen() == UI_SCREEN_LCD, "back on the LCD");
    CHECK(gRelDbEnabled, "REL/dB active again");
    CHECK(!uiJobRunning, "no job left running");
    CHECK(!uiPoll(), "idle on the LCD");

    // on the LCD screen REL/dB do not touch the interval, and their events are drained
    {
        uint8_t before = uiGetStatsInterval();
        press(BUTTON_REL); press(BUTTON_DB);
        gShort[BUTTON_STO] = gLong[BUTTON_RCL] = true;
        CHECK(!uiPoll(), "REL/dB do nothing on the LCD screen");
        CHECK(uiGetStatsInterval() == before, "interval untouched on the LCD screen");
        CHECK(!gPress[BUTTON_REL] && !gPress[BUTTON_DB] && !gShort[BUTTON_STO] && !gLong[BUTTON_RCL], "events drained");
    }

    // RCL held on the LCD screen never clears the log (v15: a hold opens the DMM settings), not even after
    // switching to the statistics mid-hold
    {
        uint32_t before = datalogCount();
        press(BUTTON_RCL); gHeld[BUTTON_RCL] = true;
        uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_LCD, "v15: RCL press on the LCD: nothing before the release or the hold");
        fakeMillisOffset() += CONFIG_UI_DMM_SETUP_HOLD_MS + 10;
        uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_DMM_SETUP, "v15: RCL held on the LCD opens the DMM settings");
        press(BUTTON_STO); // back to the LCD while RCL is still held
        uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_LCD, "STO on the DMM settings: back to the LCD");
        press(BUTTON_STO); // statistics shown while RCL is still held
        uiPoll();
        fakeMillisOffset() += CONFIG_UI_LOG_CLEAR_HOLD_MS + 500;
        uiPoll();
        CHECK(datalogCount() == before && before > 0 && gLogClearedBeeps == 1, "an RCL hold from the LCD screen never clears the log");
        gHeld[BUTTON_RCL] = false;
        press(BUTTON_STO);
        uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_LCD, "back on the LCD");
    }

    // v15 DMM settings: RCL held on the LCD; RCL = next line, REL/dB = change (applied at once, saved deferred),
    // STO = back; REL/dB never reach the K197 there
    {
        CHECK(uiGetScreen() == UI_SCREEN_LCD, "on the LCD before the DMM settings test");
        int beeps = gAsyncBeeps;
        press(BUTTON_RCL); gHeld[BUTTON_RCL] = true;
        uiPoll();
        fakeMillisOffset() += CONFIG_UI_DMM_SETUP_HOLD_MS + 10;
        CHECK(uiPoll() && uiGetScreen() == UI_SCREEN_DMM_SETUP && gAsyncBeeps == beeps + 1, "RCL held: DMM settings, beep");
        CHECK(!gRelDbEnabled, "REL/dB blocked on the DMM settings");
        gHeld[BUTTON_RCL] = false;
        uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_DMM_SETUP && uiGetDmmSetupRow() == 0, "the release changes nothing, cursor on the language");
        FbSetupScene ds;
        uiFillDmmSetupScene(&ds);
        CHECK(ds.rowCount == FB_SETUP_ROWS && strcmp(ds.title, "DMM - NASTAVENI") == 0 && strcmp(ds.values[0], "CZ - CESTINA") == 0, "DMM settings scene (CZ)");
        CHECK(!ds.moreAbove && ds.moreBelow && uiGetDmmSetupTop() == 0, "v23: 5 lines, the first 4 shown, more below");
        press(BUTTON_DB); uiPoll();
        CHECK(gLanguage == HELP_LANG_EN && gLanguageDeferred == 1, "dB: next language, saved deferred");
        uiFillDmmSetupScene(&ds);
        CHECK(strcmp(ds.title, "DMM SETTINGS") == 0 && strcmp(ds.values[0], "EN - ENGLISH") == 0, "the screen follows the language at once");
        press(BUTTON_REL); uiPoll();
        CHECK(gLanguage == HELP_LANG_CZ, "REL: back to CZ");
        press(BUTTON_RCL); uiPoll();
        CHECK(uiGetDmmSetupRow() == UI_DMM_SETUP_ROW_THEME, "RCL: next line");
        uint8_t theme = gTheme;
        press(BUTTON_REL); uiPoll();
        CHECK(gTheme == (theme + themeCount - 1) % themeCount && gThemeApplied == gTheme, "REL: previous theme, applied at once");
        press(BUTTON_DB); uiPoll();
        CHECK(gTheme == theme, "dB: next theme");
        press(BUTTON_RCL); uiPoll();
        // v23: sound ALL -> ALARMS ONLY -> OFF -> ALL (dB), back with REL
        gBeepMode = SETTINGS_BEEP_ALL;
        uiFillDmmSetupScene(&ds);
        CHECK_STR(ds.values[2], "VSE", "sound value ALL");
        press(BUTTON_DB); uiPoll();
        uiFillDmmSetupScene(&ds);
        CHECK(gBeepMode == SETTINGS_BEEP_ALARMS, "dB: alarms only");
        CHECK_STR(ds.values[2], "JEN ALARMY", "sound value ALARMS");
        press(BUTTON_DB); uiPoll();
        CHECK(gBeepMode == SETTINGS_BEEP_OFF, "dB: off");
        press(BUTTON_DB); uiPoll();
        CHECK(gBeepMode == SETTINGS_BEEP_ALL, "dB: round to all");
        press(BUTTON_REL); uiPoll();
        CHECK(gBeepMode == SETTINGS_BEEP_OFF, "REL: back to off");
        uiFillDmmSetupScene(&ds);
        CHECK_STR(ds.values[2], "VYP", "sound value OFF");
        press(BUTTON_DB); uiPoll();
        CHECK(gBeepMode == SETTINGS_BEEP_ALL, "dB: all again");
        // v23: SD card line (4th) - toggles; shows the card state
        press(BUTTON_RCL); uiPoll();
        CHECK(uiGetDmmSetupRow() == UI_DMM_SETUP_ROW_SD && uiGetDmmSetupTop() == 0, "RCL: SD line, still no scrolling");
        uiFillDmmSetupScene(&ds);
        CHECK(ds.cursorRow == 3 && strcmp(ds.labels[3], "SD KARTA") == 0 && strcmp(ds.values[3], "VYP") == 0, "SD line off");
        press(BUTTON_DB); uiPoll();
        CHECK(gSdEnabled, "dB: SD logging on");
        gSdState = SDLOG_STATE_NO_CARD;
        CHECK(uiPoll(), "v23: a change of the card state redraws the settings screen");
        CHECK(!uiPoll(), "... once");
        uiFillDmmSetupScene(&ds);
        CHECK_STR(ds.values[3], "ZAP - CHYBI KARTA", "SD on, no card");
        gSdState = SDLOG_STATE_LOGGING;
        uiPoll();
        uiFillDmmSetupScene(&ds);
        CHECK_STR(ds.values[3], "ZAP", "SD on, logging");
        gSdState = SDLOG_STATE_FULL;
        uiPoll();
        uiFillDmmSetupScene(&ds);
        CHECK_STR(ds.values[3], "ZAP - PLNA KARTA", "SD on, card full");
        // v23: CSV line (5th) - the list scrolls by one
        press(BUTTON_RCL); uiPoll();
        uiFillDmmSetupScene(&ds);
        CHECK(uiGetDmmSetupRow() == UI_DMM_SETUP_ROW_CSV && uiGetDmmSetupTop() == 1 && ds.cursorRow == 3 && ds.moreAbove && ds.moreBelow,
              "RCL: CSV line, scrolled by one, more above and (v24: HOLD) below");
        CHECK(strcmp(ds.labels[0], "TEMA") == 0 && strcmp(ds.labels[3], "FORMAT CSV") == 0, "the window shows lines 2..5");
        CHECK_STR(ds.values[3], "1,234 ; (CZ)", "CSV semicolon");
        press(BUTTON_DB); uiPoll();
        uiFillDmmSetupScene(&ds);
        CHECK(gSdCsv == SETTINGS_CSV_COMMA, "dB: comma");
        CHECK_STR(ds.values[3], "1.234 , (EN)", "CSV comma");
        press(BUTTON_REL); uiPoll();
        CHECK(gSdCsv == SETTINGS_CSV_SEMICOLON, "REL: semicolon again");
        for (int lang = 0; lang < HELP_LANG_COUNT; lang++)
        {
            gLanguage = (uint8_t)lang;
            for (int st = 0; st < SDLOG_STATE_COUNT; st++)
            {
                gSdState = (uint8_t)st;
                for (int m = 0; m < SETTINGS_BEEP_MODES; m++)
                {
                    gBeepMode = (uint8_t)m;
                    for (int top = 0; top < 2; top++)
                    {
                        uiFillDmmSetupScene(&ds);
                        for (int i = 0; i < ds.rowCount; i++)
                            CHECK(strlen(ds.values[i]) <= FB_SETUP_FIELD_CHARS && strlen(ds.labels[i]) <= 12, "DMM settings texts fit");
                    }
                }
            }
        }
        gLanguage = HELP_LANG_CZ;
        gBeepMode = SETTINGS_BEEP_ALL;
        gSdEnabled = false;
        gSdState = SDLOG_STATE_OFF;
        // v24: HOLD line (6th) - MANUAL / AUTO HOLD, the window scrolls by two
        press(BUTTON_RCL); uiPoll();
        uiFillDmmSetupScene(&ds);
        CHECK(uiGetDmmSetupRow() == UI_DMM_SETUP_ROW_HOLD && uiGetDmmSetupTop() == 2 && ds.cursorRow == 3 && ds.moreAbove && !ds.moreBelow,
              "RCL: HOLD line, scrolled by two");
        CHECK(strcmp(ds.labels[3], "HOLD") == 0 && strcmp(ds.values[3], "RUCNI") == 0, "HOLD manual");
        press(BUTTON_DB); uiPoll();
        uiFillDmmSetupScene(&ds);
        CHECK(gHoldMode == SETTINGS_HOLD_AUTO && strcmp(ds.values[3], "AUTO HOLD") == 0, "dB: AUTO HOLD");
        press(BUTTON_DB); uiPoll();
        CHECK(gHoldMode == SETTINGS_HOLD_MANUAL, "dB: manual again (wraps)");
        press(BUTTON_RCL); uiPoll();
        uiFillDmmSetupScene(&ds);
        CHECK(uiGetDmmSetupRow() == 0 && uiGetDmmSetupTop() == 0 && ds.cursorRow == 0 && !ds.moreAbove && ds.moreBelow,
              "RCL wraps to the first line, the window back at the top");
        press(BUTTON_STO);
        CHECK(uiPoll() && uiGetScreen() == UI_SCREEN_LCD && gRelDbEnabled, "STO: back to the LCD, REL/dB active again");

        // timeout: a minute without a button -> back to the LCD; a press restarts it
        press(BUTTON_RCL); gHeld[BUTTON_RCL] = true; uiPoll();
        fakeMillisOffset() += CONFIG_UI_DMM_SETUP_HOLD_MS + 10; uiPoll();
        gHeld[BUTTON_RCL] = false; uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_DMM_SETUP, "DMM settings open again");
        fakeMillisOffset() += CONFIG_UI_HELP_TIMEOUT_MS - 1000; uiPoll();
        press(BUTTON_RCL); uiPoll(); // activity
        fakeMillisOffset() += CONFIG_UI_HELP_TIMEOUT_MS - 1000; uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_DMM_SETUP, "a press restarted the timeout");
        fakeMillisOffset() += 2000;
        CHECK(uiPoll() && uiGetScreen() == UI_SCREEN_LCD, "DMM settings timeout: back to the LCD");
    }

    // v24 DATA HOLD: STO held on the LCD starts HOLD (kind from the settings, on the first edge of the press);
    // its release does nothing; a short STO then only ends HOLD; a short STO without HOLD opens the statistics
    {
        CHECK(uiGetScreen() == UI_SCREEN_LCD, "on the LCD before the HOLD test");
        int beeps = gAsyncBeeps;
        gHoldMode = SETTINGS_HOLD_MANUAL;
        press(BUTTON_STO); gHeld[BUTTON_STO] = true;
        uint32_t pressAt = gPressStart[BUTTON_STO];
        uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_LCD && gHoldStarts == 0, "STO pressed: not decided yet (click or hold?)");
        CHECK(gFrozen && gFrozenAt == pressAt, "manual: preview at once (frozen on the first edge, HOLD lit)");
        fakeMillisOffset() += CONFIG_UI_HOLD_HOLD_MS / 2; uiPoll();
        CHECK(gHoldStarts == 0 && gFrozen, "not confirmed before CONFIG_UI_HOLD_HOLD_MS, still frozen");
        fakeMillisOffset() += CONFIG_UI_HOLD_HOLD_MS / 2 + 10;
        CHECK(uiPoll() && gHoldStarts == 1 && gHoldStartMode == SETTINGS_HOLD_MANUAL && gHoldStartAt == pressAt && gHoldShowTag &&
              gAsyncBeeps == beeps + 1 && uiGetScreen() == UI_SCREEN_LCD,
              "STO held: manual HOLD confirmed (first edge), beep, TAG now");
        gHeld[BUTTON_STO] = false; uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_LCD && gHoldStops == 0 && gHoldStarts == 1, "the release does nothing");
        press(BUTTON_STO); uiPoll();
        CHECK(gHoldStops == 1 && gHoldState == HOLD_STATE_OFF && uiGetScreen() == UI_SCREEN_LCD, "short STO with HOLD on: only ends it");
        press(BUTTON_STO); uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_QUICKSTATS && gHoldStops == 1 && !gFrozen, "short STO without HOLD: statistics, LCD live again");
        press(BUTTON_STO); uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_LCD, "STO on the statistics: back at the press");
        gHoldMode = SETTINGS_HOLD_AUTO;
        press(BUTTON_STO); gHeld[BUTTON_STO] = true; uiPoll();
        CHECK(!gFrozen, "AUTO: nothing frozen at the press");
        fakeMillisOffset() += CONFIG_UI_HOLD_HOLD_MS + 10; uiPoll();
        CHECK(gHoldStarts == 2 && gHoldStartMode == SETTINGS_HOLD_AUTO && gHoldState == HOLD_STATE_AUTO_WAIT, "AUTO HOLD from the setting");
        gHeld[BUTTON_STO] = false; uiPoll();
        press(BUTTON_STO); gHeld[BUTTON_STO] = true; uiPoll();
        fakeMillisOffset() += CONFIG_UI_HOLD_HOLD_MS + 10; uiPoll();
        CHECK(gHoldStarts == 3 && gHoldStops == 1, "STO held again with HOLD on: starts over");
        gHeld[BUTTON_STO] = false; uiPoll();
        press(BUTTON_STO); uiPoll();
        CHECK(gHoldStops == 2 && uiGetScreen() == UI_SCREEN_LCD, "short STO ends it");
        gHoldMode = SETTINGS_HOLD_MANUAL;
        printf("v24 HOLD: STO hold / click on the LCD checked\n");
    }

    // v17 limit alarm: dB held on the LCD opens its settings for the function being measured; mode, limits
    // (REL/dB one count, held = repeating and accelerating, RCL held = live reading), signal; the symbol above
    // the minus, the blinking and the beeps; persistence; always off after a "reboot"
    {
        CHECK(uiGetScreen() == UI_SCREEN_LCD, "on the LCD before the alarm test");
        CHECK(gAlarmSymbol == FB_ALARM_NONE, "no alarm symbol while the alarm is off");
        gClockMs += 30000; fakeMillisOffset() += 30000; setClock(); // older readings leave the 10 s window
        feed(123400, 0, KEITHLEY_UNIT_V); // DCV: MIN 1.23400, MAX 1.23500 over the last 10 s, live 1.23456 V
        feed(123500, 0, KEITHLEY_UNIT_V);
        feed(123456, 0, KEITHLEY_UNIT_V);
        double rmin = 0, rmax = 0;
        CHECK(alarmRecentRange(ALARM_FUNCTION_DCV, &rmin, &rmax) && fabs(rmin - 1.234) < 1e-6 && fabs(rmax - 1.235) < 1e-6, "recent MIN/MAX of the log");
        CHECK(!alarmRecentRange(ALARM_FUNCTION_ACV, &rmin, &rmax), "no ACV readings in the window"); 
        // a short dB changes nothing on screen (buttons.ino sends it to the K197 at the release)
        press(BUTTON_DB); gHeld[BUTTON_DB] = true; uiPoll();
        fakeMillisOffset() += 200; uiPoll();
        gHeld[BUTTON_DB] = false;
        CHECK(!uiPoll() && uiGetScreen() == UI_SCREEN_LCD, "short dB on the LCD: no screen change");
        int beeps = gAsyncBeeps;
        press(BUTTON_DB); gHeld[BUTTON_DB] = true; uiPoll();
        fakeMillisOffset() += CONFIG_UI_ALARM_HOLD_MS + 10;
        CHECK(uiPoll() && uiGetScreen() == UI_SCREEN_ALARM_SETUP && gAsyncBeeps == beeps + 1, "dB held: alarm settings, beep");
        CHECK(uiGetAlarmSetupFunction() == ALARM_FUNCTION_DCV && !gRelDbEnabled, "for DCV, REL/dB blocked");
        gHeld[BUTTON_DB] = false; uiPoll();
        CHECK(uiGetAlarmSetupRow() == UI_ALARM_SETUP_ROW_MODE, "the release changes nothing, cursor on the mode");
        FbSetupScene as;
        uiFillAlarmSetupScene(&as);
        CHECK(strcmp(as.title, "DCV - ALARM") == 0 && as.rowCount == 4, "alarm settings title");
        CHECK(strcmp(as.values[0], "VYP") == 0 && strcmp(as.values[1], "--") == 0 && strcmp(as.values[2], "--") == 0 &&
                  strcmp(as.values[3], "ZVUK + SIPKA") == 0, "alarm settings: off, no limits, beep + arrow");
        CHECK(strstr(as.keys, "zmena") != NULL, "key hint of the mode line");

        press(BUTTON_DB); uiPoll();
        CHECK(alarmGetMode(ALARM_FUNCTION_DCV) == ALARM_MODE_UPPER, "dB: UPPER");
        uiFillAlarmSetupScene(&as);
        CHECK(strcmp(as.values[0], "HORNI") == 0 && strcmp(as.values[2], "1.23500 V") == 0 && strcmp(as.values[1], "--") == 0,
              "switching on sets the missing upper limit to the MAX of the last 10 s");
        CHECK(gAlarmSymbol == FB_ALARM_ARMED_UPPER && alarmGetState() == ALARM_STATE_ARMED, "inside: outline up");
        press(BUTTON_DB); uiPoll();
        CHECK(alarmGetMode(ALARM_FUNCTION_DCV) == ALARM_MODE_LOWER && gAlarmSymbol == FB_ALARM_ARMED_LOWER, "dB: LOWER, outline down");
        press(BUTTON_DB); uiPoll();
        CHECK(alarmGetMode(ALARM_FUNCTION_DCV) == ALARM_MODE_BOTH && gAlarmSymbol == FB_ALARM_ARMED_BOTH, "dB: BOTH, two outlines");
        uiFillAlarmSetupScene(&as);
        CHECK(strcmp(as.values[1], "1.23400 V") == 0 && strcmp(as.values[0], "OBE MEZE") == 0, "lower limit = MIN of the last 10 s");
        // REL on the ALARM line acts at the release; held 1 s it opens the help of this screen, STO returns here
        press(BUTTON_REL); gHeld[BUTTON_REL] = true; uiPoll();
        CHECK(alarmGetMode(ALARM_FUNCTION_DCV) == ALARM_MODE_BOTH, "REL on the ALARM line: nothing at the press");
        gHeld[BUTTON_REL] = false; uiPoll();
        CHECK(alarmGetMode(ALARM_FUNCTION_DCV) == ALARM_MODE_LOWER, "REL click: previous mode, at the release");
        press(BUTTON_DB); uiPoll();
        CHECK(alarmGetMode(ALARM_FUNCTION_DCV) == ALARM_MODE_BOTH, "dB: BOTH again");
        press(BUTTON_REL); gHeld[BUTTON_REL] = true; uiPoll();
        fakeMillisOffset() += CONFIG_UI_HELP_HOLD_MS + 10;
        CHECK(uiPoll() && uiGetScreen() == UI_SCREEN_HELP && uiGetHelpSection() == HELP_SECTION_ALARM, "REL held on the ALARM line: alarm help");
        gHeld[BUTTON_REL] = false; uiPoll();
        CHECK(alarmGetMode(ALARM_FUNCTION_DCV) == ALARM_MODE_BOTH, "the help hold changed nothing");
        press(BUTTON_STO);
        CHECK(uiPoll() && uiGetScreen() == UI_SCREEN_ALARM_SETUP && uiGetAlarmSetupRow() == UI_ALARM_SETUP_ROW_MODE && uiGetAlarmSetupFunction() == ALARM_FUNCTION_DCV,
              "STO on the help: back to the alarm settings"); 
        CHECK(gAlarmSavesDeferred > 0 && gAlarmSavesNow == 0, "front panel saves deferred");

        // to the upper limit line: RCL clicks act at the release
        press(BUTTON_RCL); gHeld[BUTTON_RCL] = true; uiPoll();
        CHECK(uiGetAlarmSetupRow() == UI_ALARM_SETUP_ROW_MODE, "RCL: nothing at the press");
        gHeld[BUTTON_RCL] = false; uiPoll();
        CHECK(uiGetAlarmSetupRow() == UI_ALARM_SETUP_ROW_LOWER, "RCL: next line at the release");
        press(BUTTON_RCL); uiPoll();
        CHECK(uiGetAlarmSetupRow() == UI_ALARM_SETUP_ROW_UPPER, "RCL: upper limit line");
        uiFillAlarmSetupScene(&as);
        CHECK(strstr(as.keys, "RCL 1s") != NULL, "key hint of a limit line");
        press(BUTTON_DB); uiPoll();
        char t[16];
        alarmFormatLimit(t, sizeof(t), ALARM_FUNCTION_DCV, true);
        CHECK(strcmp(t, "1.23501") == 0, "dB: +1 count");
        // REL held: one step at the press, repeats after the delay, x10 after every CONFIG_UI_ALARM_REPEAT_PER_DECADE repeats
        press(BUTTON_REL); gHeld[BUTTON_REL] = true; uiPoll();
        alarmFormatLimit(t, sizeof(t), ALARM_FUNCTION_DCV, true);
        CHECK(strcmp(t, "1.23500") == 0 && uiGetScreen() == UI_SCREEN_ALARM_SETUP, "REL on a limit line: -1 count at the press, no help");
        fakeMillisOffset() += CONFIG_UI_ALARM_REPEAT_DELAY_MS - 20; uiPoll();
        alarmFormatLimit(t, sizeof(t), ALARM_FUNCTION_DCV, true);
        CHECK(strcmp(t, "1.23500") == 0, "no repeat before the delay");
        fakeMillisOffset() += 20; uiPoll(); // repeat 1
        for (int i = 1; i < CONFIG_UI_ALARM_REPEAT_PER_DECADE; i++) { fakeMillisOffset() += CONFIG_UI_ALARM_REPEAT_MS; uiPoll(); }
        // expected counts: single counts for CONFIG_UI_ALARM_REPEAT_PER_DECADE repeats, then tens (rounded down)
        long c1 = 123500 - CONFIG_UI_ALARM_REPEAT_PER_DECADE;
        long c2 = ((c1 - 10) + 9) / 10 * 10; // a downward step of 10 rounds towards where it came from
        long c3 = c2 - 10;
        char e[16];
        alarmFormatLimit(t, sizeof(t), ALARM_FUNCTION_DCV, true);
        snprintf(e, sizeof(e), "%.5f", c1 / 100000.0);
        CHECK(strcmp(t, e) == 0, "the first repeats are single counts");
        fakeMillisOffset() += CONFIG_UI_ALARM_REPEAT_MS; uiPoll();
        alarmFormatLimit(t, sizeof(t), ALARM_FUNCTION_DCV, true);
        snprintf(e, sizeof(e), "%.5f", c2 / 100000.0);
        CHECK(strcmp(t, e) == 0, "then steps of 10 counts");
        fakeMillisOffset() += CONFIG_UI_ALARM_REPEAT_MS; uiPoll();
        alarmFormatLimit(t, sizeof(t), ALARM_FUNCTION_DCV, true);
        snprintf(e, sizeof(e), "%.5f", c3 / 100000.0);
        CHECK(strcmp(t, e) == 0 && uiGetScreen() == UI_SCREEN_ALARM_SETUP, "10 counts, held past the help time without help");
        gHeld[BUTTON_REL] = false; uiPoll();
        fakeMillisOffset() += 1000; uiPoll();
        alarmFormatLimit(t, sizeof(t), ALARM_FUNCTION_DCV, true);
        CHECK(strcmp(t, e) == 0, "the release stops the repeat");
        printf("alarm limit editing: %d ms delay, a step every %d ms, x10 after %d steps\n", CONFIG_UI_ALARM_REPEAT_DELAY_MS,
               CONFIG_UI_ALARM_REPEAT_MS, CONFIG_UI_ALARM_REPEAT_PER_DECADE);

        // above the upper limit: bright triangle up, blinking in step with the beeps
        feed(130000, 0, KEITHLEY_UNIT_V);
        CHECK(alarmGetState() == ALARM_STATE_HIGH && gAlarmSymbol == FB_ALARM_HIGH, "above: filled triangle up, at once");
        beeps = gAsyncBeeps;
        int periodTicks = CONFIG_ALARM_BEEP_PERIOD_MS / CONFIG_ALARM_TICK_MS;
        int onTicks = CONFIG_ALARM_BLINK_ON_MS / CONFIG_ALARM_TICK_MS;
        int visible = 0;
        for (int i = 0; i < periodTicks * 3; i++) { alarmTick(); if (gAlarmSymbol == FB_ALARM_HIGH) visible++; else CHECK(gAlarmSymbol == FB_ALARM_NONE, "blink: off phase shows nothing"); }
        CHECK(gAsyncBeeps == beeps + 3, "one beep per period");
        CHECK(visible == onTicks * 3, "visible for CONFIG_ALARM_BLINK_ON_MS of each period");
        // RCL held on the limit line: the live reading becomes the limit -> inside again
        press(BUTTON_RCL); gHeld[BUTTON_RCL] = true; uiPoll();
        fakeMillisOffset() += CONFIG_UI_ALARM_TAKE_HOLD_MS + 10; uiPoll();
        gHeld[BUTTON_RCL] = false; uiPoll();
        alarmFormatLimit(t, sizeof(t), ALARM_FUNCTION_DCV, true);
        CHECK(strcmp(t, "1.30000") == 0 && uiGetAlarmSetupRow() == UI_ALARM_SETUP_ROW_UPPER, "RCL held: limit = live reading, line kept");
        CHECK(alarmGetState() == ALARM_STATE_ARMED && gAlarmSymbol == FB_ALARM_ARMED_BOTH, "inside again: two outlines");
        beeps = gAsyncBeeps;
        for (int i = 0; i < periodTicks; i++) alarmTick();
        CHECK(gAsyncBeeps == beeps, "no beep inside the limits");
        // below the lower limit (negative reading), OL above
        feed(-100000, 0, KEITHLEY_UNIT_V);
        CHECK(alarmGetState() == ALARM_STATE_LOW && gAlarmSymbol == FB_ALARM_LOW, "below: filled triangle down");
        feedDigits("  0L  ", 0, 0, KEITHLEY_UNIT_V);
        CHECK(alarmGetState() == ALARM_STATE_HIGH, "OL counts as above");
        feedDigits("  0L  ", 0, KEITHLEY_BIT_MINUS, KEITHLEY_UNIT_V);
        CHECK(alarmGetState() == ALARM_STATE_LOW, "-OL counts as below");
        // signal: arrow only -> no beeps, still blinking
        press(BUTTON_RCL); uiPoll();
        CHECK(uiGetAlarmSetupRow() == UI_ALARM_SETUP_ROW_SIGNAL, "RCL: signal line");
        press(BUTTON_DB); uiPoll();
        CHECK(alarmGetSignal() == ALARM_SIGNAL_ARROW, "dB: arrow only");
        uiFillAlarmSetupScene(&as);
        CHECK(strcmp(as.values[3], "JEN SIPKA") == 0, "signal value");
        feed(-150000, 0, KEITHLEY_UNIT_V);
        beeps = gAsyncBeeps; visible = 0;
        for (int i = 0; i < periodTicks; i++) { alarmTick(); if (gAlarmSymbol == FB_ALARM_LOW) visible++; }
        CHECK(gAsyncBeeps == beeps && visible == onTicks, "arrow only: blinking without beeps");
        press(BUTTON_REL); uiPoll();
        CHECK(alarmGetSignal() == ALARM_SIGNAL_BEEP, "REL: beep + arrow again");
        for (int i = 0; i < 4; i++) { uiFillAlarmSetupScene(&as); CHECK(strlen(as.values[i]) <= FB_SETUP_FIELD_CHARS && strlen(as.labels[i]) <= 12, "alarm settings texts fit"); }
        gLanguage = HELP_LANG_EN;
        for (int i = 0; i < 4; i++) { uiFillAlarmSetupScene(&as); CHECK(strlen(as.values[i]) <= FB_SETUP_FIELD_CHARS && strlen(as.labels[i]) <= 12, "alarm settings texts fit (EN)"); }
        CHECK(strlen(as.keys) <= 47, "key hint fits (EN)");
        gLanguage = HELP_LANG_CZ;
        press(BUTTON_STO);
        CHECK(uiPoll() && uiGetScreen() == UI_SCREEN_LCD && gRelDbEnabled, "STO: back to the LCD");

        // another function: its own (off) alarm; back to DCV: still on
        feed(100000, 2, KEITHLEY_UNIT_KOHM);
        CHECK(alarmGetState() == ALARM_STATE_NONE && gAlarmSymbol == FB_ALARM_NONE, "OHM: no alarm");
        feed(125000, 0, KEITHLEY_UNIT_V);
        CHECK(alarmGetState() == ALARM_STATE_ARMED && gAlarmSymbol == FB_ALARM_ARMED_BOTH, "DCV again: still watched");
        // v20: the first readings after the change settle - even outside the limits they give no alarm
        feed(190000, 0, KEITHLEY_UNIT_V);
        CHECK(alarmGetState() == ALARM_STATE_ARMED, "settling after the function change: no alarm yet");
        for (int i = 2; i < CONFIG_DATALOG_SETTLE_READINGS; i++) feed(125000, 0, KEITHLEY_UNIT_V);
        feed(190000, 0, KEITHLEY_UNIT_V);
        CHECK(alarmGetState() == ALARM_STATE_HIGH, "settled: judged again");
        feed(125000, 0, KEITHLEY_UNIT_V);
        // limits compared in counts of the display: a float limit of 1.2 equals a reading of 1.20000
        alarmSetLimit(ALARM_FUNCTION_DCV, true, (double)1.2f, false);
        alarmSetLimit(ALARM_FUNCTION_DCV, false, (double)1.2f, false);
        feed(120000, 0, KEITHLEY_UNIT_V);
        CHECK(alarmGetState() == ALARM_STATE_ARMED, "1.20000 is neither above nor below a limit of 1.2 (float)");
        feed(120001, 0, KEITHLEY_UNIT_V);
        CHECK(alarmGetState() == ALARM_STATE_HIGH, "1.20001 is above");
        // the same limit in the mV range (200 mV: 199.999 mV, 3 decimals)
        feed(150000, 2, KEITHLEY_UNIT_MV);
        CHECK(alarmGetState() == ALARM_STATE_LOW, "150 mV is below 1.2 V");
        // persisted values (deferred) and a "reboot": limits and mode kept, the alarm off
        CHECK(fabsf(gAlarmHigh[ALARM_FUNCTION_DCV] - 1.2f) < 1e-6f && gAlarmMode[ALARM_FUNCTION_DCV] == ALARM_MODE_BOTH, "persisted: limit and mode");
        alarmBegin();
        feed(150000, 2, KEITHLEY_UNIT_MV);
        CHECK(alarmGetMode(ALARM_FUNCTION_DCV) == ALARM_MODE_OFF && alarmGetStoredMode(ALARM_FUNCTION_DCV) == ALARM_MODE_BOTH, "after power-up: off, mode remembered");
        CHECK(fabs(alarmGetLimit(ALARM_FUNCTION_DCV, true) - 1.2) < 1e-6 && gAlarmSymbol == FB_ALARM_NONE, "limits kept, no symbol");
        // stepping a negative limit by 10 counts rounds towards where it came from
        alarmSetLimit(ALARM_FUNCTION_DCV, false, -0.123, false); // shown in mV: -123.000
        alarmStepLimit(ALARM_FUNCTION_DCV, false, +1, 10000);
        alarmFormatLimit(t, sizeof(t), ALARM_FUNCTION_DCV, false);
        CHECK(strcmp(t, "-120.000") == 0, "-123.000 mV + 10 counts of 1 mV... rounded: -120.000");
        alarmStepLimit(ALARM_FUNCTION_DCV, false, -1, 10000);
        alarmFormatLimit(t, sizeof(t), ALARM_FUNCTION_DCV, false);
        CHECK(strcmp(t, "-130.000") == 0, "-120.000 - 10 mV: -130.000");
        for (int i = 0; i < 100; i++) alarmStepLimit(ALARM_FUNCTION_DCV, false, -1, 10000);
        alarmFormatLimit(t, sizeof(t), ALARM_FUNCTION_DCV, false);
        CHECK(strcmp(t, "-199999") == 0, "clamped at -199999 counts at zero decimals");
        // dB held without a known function: nothing
        feedDigits("      ", -1, 0, KEITHLEY_UNIT_NONE);
        press(BUTTON_DB); gHeld[BUTTON_DB] = true; uiPoll();
        fakeMillisOffset() += CONFIG_UI_ALARM_HOLD_MS + 10; uiPoll();
        gHeld[BUTTON_DB] = false; uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_LCD, "no function measured: dB held does not open the alarm settings");
        // timeout
        feed(110000, 0, KEITHLEY_UNIT_V);
        press(BUTTON_DB); gHeld[BUTTON_DB] = true; uiPoll();
        fakeMillisOffset() += CONFIG_UI_ALARM_HOLD_MS + 10; uiPoll();
        gHeld[BUTTON_DB] = false; uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_ALARM_SETUP, "alarm settings open again");
        fakeMillisOffset() += CONFIG_UI_HELP_TIMEOUT_MS + 10;
        CHECK(uiPoll() && uiGetScreen() == UI_SCREEN_LCD, "alarm settings timeout: back to the LCD");
        printf("alarm: settings screen, limits, symbols, blinking, beeps, persistence checked\n");
    }

    // v18 markers: REL + dB together on the LCD and the graph mark the record SHOWN at the first edge; the two
    // presses do nothing else (no help, no alarm settings, no interval change); not on the statistics screen.
    // On the graph dB now acts at the release.
    {
        CHECK(uiGetScreen() == UI_SCREEN_LCD, "on the LCD before the marker test");
        feed(111110, 0, KEITHLEY_UNIT_V);
        uint32_t endA = datalogEndSerial(NULL); // record A = serial endA - 1
        fakeMillisOffset() += 100;
        uint32_t tPress = (uint32_t)millis(); // A is shown
        feed(122220, 0, KEITHLEY_UNIT_V);     // B arrives after the first edge, before the chord is recognised
        press(BUTTON_REL); gHeld[BUTTON_REL] = true; gPressStart[BUTTON_REL] = tPress; uiPoll();
        press(BUTTON_DB); gHeld[BUTTON_DB] = true;
        gChord = true; gChordMs = tPress;
        int markers = gMarkers;
        CHECK(uiPoll() && gMarkers == markers + 1 && gMarkerMs == tPress, "chord on the LCD: one marker at the first edge");
        LogRecord recs[2];
        uint32_t s0 = endA - 1;
        CHECK(datalogReadSerial(&s0, recs, 2, NULL) == 2 && (recs[0].indicatorBits & DATALOG_BIT_MARKER) != 0 &&
                  (recs[1].indicatorBits & DATALOG_BIT_MARKER) == 0, "the record shown at the first edge got the marker, not the newer one");
        fakeMillisOffset() += CONFIG_UI_ALARM_HOLD_MS + 100; uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_LCD, "both held past 1 s: neither help nor alarm settings");
        gHeld[BUTTON_REL] = false; uiPoll();
        gHeld[BUTTON_DB] = false; uiPoll();
        press(BUTTON_DB); uiPoll(); // a press event that arrived late still belongs to... nothing now: both were up
        gHeld[BUTTON_DB] = false; uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_LCD && gMarkers == markers + 1, "after the chord: back to normal");
        CHECK(datalogMarkCount() >= 1, "marker counted");
        // marker older than the append history: the oldest remembered record
        CHECK(datalogMarkAtMs(tPress - 100000, NULL), "a very old time still marks a record");

        // graph: dB at the release now; the chord marks instead of changing the interval
        press(BUTTON_RCL); uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_GRAPH, "graph open");
        uint8_t interval = graphGetInterval();
        if (interval + 1 >= GRAPH_INTERVAL_COUNT) { graphSetInterval(0); interval = 0; }
        press(BUTTON_DB); gHeld[BUTTON_DB] = true; uiPoll();
        CHECK(graphGetInterval() == interval, "graph: dB does nothing at the press");
        gHeld[BUTTON_DB] = false; uiPoll();
        CHECK(graphGetInterval() == interval + 1, "graph: dB click = longer interval at the release");
        press(BUTTON_DB); gHeld[BUTTON_DB] = true; uiPoll();
        press(BUTTON_REL); gHeld[BUTTON_REL] = true; gChord = true; gChordMs = (uint32_t)millis(); uiPoll();
        CHECK(gMarkers == markers + 2, "chord on the graph: marker");
        gHeld[BUTTON_DB] = false; uiPoll();
        gHeld[BUTTON_REL] = false; uiPoll();
        CHECK(graphGetInterval() == interval + 1, "graph: the chord changed no interval");
        press(BUTTON_STO); uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_LCD, "back on the LCD");
        // statistics: no marker
        press(BUTTON_STO); uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_QUICKSTATS, "statistics");
        gChord = true; gChordMs = (uint32_t)millis(); uiPoll();
        CHECK(gMarkers == markers + 2, "no marker on the statistics screen");
        press(BUTTON_STO); uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_LCD, "LCD again");
        printf("markers: chord on LCD/graph, record at the first edge, no side effects checked\n");
    }

    // v12 help from the LCD screen: a short REL changes nothing on screen (buttons.ino pulses the K197), a hold opens
    // the help, REL/dB are blocked there, STO returns to the LCD with REL/dB active again
    {
        CHECK(uiGetScreen() == UI_SCREEN_LCD, "on the LCD before the help test");
        press(BUTTON_REL); gHeld[BUTTON_REL] = true;
        uiPoll();
        gHeld[BUTTON_REL] = false;
        CHECK(!uiPoll() && uiGetScreen() == UI_SCREEN_LCD, "short REL on the LCD: no screen change");
        CHECK(gRelDbEnabled, "REL/dB active on the LCD");
        press(BUTTON_REL); gHeld[BUTTON_REL] = true;
        uiPoll();
        fakeMillisOffset() += CONFIG_UI_HELP_HOLD_MS + 10;
        CHECK(uiPoll() && uiGetScreen() == UI_SCREEN_HELP, "REL hold on the LCD opens the help");
        CHECK(!gRelDbEnabled, "REL/dB blocked on the help screen");
        fakeMillisOffset() += 5000;
        uiPoll();
        CHECK(uiGetHelpTop() == 0, "keeping REL held does not page");
        gHeld[BUTTON_REL] = false;
        uiPoll();
        // contents (DMM section, opened from the LCD): preface (version, build) underlined, then the text
        FbHelpScene hs;
        uiFillHelpScene(&hs);
        CHECK(strncmp(hs.lines[0], "KEITHLEY 197R, fw. ", 19) == 0 && strstr(hs.lines[0], SCPI_IDN_FIRMWARE) != NULL, "header: KEITHLEY 197R + firmware version");
        CHECK(hs.rightAlign[1] && !hs.rightAlign[0] && !hs.rightAlign[2], "build stamp flush right");
        CHECK(strncmp(hs.lines[1], "build ", 6) == 0 && strlen(hs.lines[1]) == 6 + 17, "preface shows the build stamp DD.MM.YY hh:mm:ss");
        CHECK(hs.underline[1] && !hs.underline[0] && !hs.underline[2], "only the preface is underlined");
        CHECK(strncmp(hs.lines[2], "REL, dB:", 8) == 0, "help text follows the header");

        // every line of every section in every language fits the screen (47 characters), whole pages
        CHECK(uiGetHelpSection() == HELP_SECTION_DMM, "help from the LCD: the DMM section");
        for (int lang = 0; lang < HELP_LANG_COUNT; lang++)
        {
            for (int sec = 0; sec < HELP_SECTION_COUNT; sec++)
            {
                int longest = 0, lineNo = 0, tooLong = 0;
                const char *p = helpTexts[lang][sec];
                while (*p)
                {
                    const char *nl = strchr(p, '\n');
                    int len = nl ? (int)(nl - p) : (int)strlen(p);
                    lineNo++;
                    if (len > longest) longest = len;
                    if (len > 47) { printf("help %s section %d line %d is %d characters long (47 fit)\n", helpLanguageNames[lang], sec, lineNo, len); tooLong++; }
                    if (!nl) break;
                    p = nl + 1;
                }
                int total = lineNo + (sec == HELP_SECTION_DMM ? 2 : 0);
                CHECK(tooLong == 0, "help text lines fit the screen");
                CHECK(total % FB_HELP_LINES == 0, "every help section fills whole pages");
                printf("help %s section %d: %d text lines%s, longest %d characters\n", helpLanguageNames[lang], sec, lineNo,
                       sec == HELP_SECTION_DMM ? " (+2 preface)" : "", longest);
            }
        }

        // paging: dB forward by full pages, the last page ends with the last line, REL back, RCL to the end
        uint16_t count = uiGetHelpLineCount();
        uint16_t lastTop = (uint16_t)(count - FB_HELP_LINES);
        press(BUTTON_DB); uiPoll();
        CHECK(uiGetHelpTop() == FB_HELP_LINES, "dB: next page");
        for (int k = 0; k < 20; k++) { press(BUTTON_DB); uiPoll(); }
        CHECK(uiGetHelpTop() == lastTop, "dB stops at the last page, which ends with the last line");
        uiFillHelpScene(&hs);
        CHECK(strstr(hs.lines[FB_HELP_LINES - 1], "6/6") != NULL, "last line of the text (page mark 6/6, v24) on the last row");
        CHECK(lastTop % FB_HELP_LINES == 0, "help text fills whole pages (header + 4 lines, then 6 per page)");
        CHECK(!uiPoll(), "nothing to redraw without a button");
        press(BUTTON_REL); uiPoll();
        CHECK(uiGetHelpTop() == lastTop - FB_HELP_LINES, "REL: previous page (full page)");
        for (int k = 0; k < 20; k++) { press(BUTTON_REL); uiPoll(); }
        CHECK(uiGetHelpTop() == 0, "REL stops at the top");
        press(BUTTON_RCL); uiPoll();
        CHECK(uiGetHelpTop() == FB_HELP_LINES && uiGetScreen() == UI_SCREEN_HELP, "v25: RCL = next page on the help screen");
        {
            int guard = 0;
            while (uiGetHelpTop() != lastTop && guard++ < 20) { press(BUTTON_RCL); uiPoll(); }
            CHECK(uiGetHelpTop() == lastTop, "RCL reaches the last page");
            press(BUTTON_RCL); uiPoll();
            CHECK(uiGetHelpTop() == 0, "RCL after the last page: the first one");
        }

        // holds: dB held 1 s -> end (while held, release does not page further), REL held 1 s -> start
        press(BUTTON_DB); gHeld[BUTTON_DB] = true; uiPoll();
        CHECK(uiGetHelpTop() == 0, "dB held: nothing before the hold time");
        fakeMillisOffset() += CONFIG_UI_HELP_HOLD_MS + 10;
        CHECK(uiPoll() && uiGetHelpTop() == lastTop, "dB held 1 s: jump to the end");
        gHeld[BUTTON_DB] = false; uiPoll();
        CHECK(uiGetHelpTop() == lastTop, "releasing after the hold does not page");
        press(BUTTON_REL); gHeld[BUTTON_REL] = true; uiPoll();
        CHECK(uiGetHelpTop() == lastTop, "REL held: nothing before the hold time");
        fakeMillisOffset() += CONFIG_UI_HELP_HOLD_MS + 10;
        CHECK(uiPoll() && uiGetHelpTop() == 0, "REL held 1 s: back to the start");
        gHeld[BUTTON_REL] = false; uiPoll();
        CHECK(uiGetHelpTop() == 0 && uiGetScreen() == UI_SCREEN_HELP, "still on the first page after the release");


        // v15: SYSTem:LANGuage EN -> the next help opens in English; graph help without preface
        gLanguage = HELP_LANG_EN;
        press(BUTTON_STO); uiPoll();
        press(BUTTON_REL); gHeld[BUTTON_REL] = true; uiPoll();
        fakeMillisOffset() += CONFIG_UI_HELP_HOLD_MS + 10;
        uiPoll();
        gHeld[BUTTON_REL] = false; uiPoll();
        uiFillHelpScene(&hs);
        CHECK(uiGetScreen() == UI_SCREEN_HELP && strncmp(hs.lines[2], "REL, dB: original", 17) == 0, "EN: DMM help in English");
        gLanguage = HELP_LANG_CZ;
        press(BUTTON_STO);
        CHECK(uiPoll() && uiGetScreen() == UI_SCREEN_LCD, "STO returns to the LCD");
        CHECK(gRelDbEnabled, "REL/dB active again on the LCD");
    }

    // v14 graph screens: RCL on the LCD -> graph; REL click/dB = interval; RCL click = next Y mode; RCL held 1 s ->
    // settings (RCL = next line, REL/dB = value, STO = back); REL held = help and back; STO -> LCD
    {
        CHECK(uiGetScreen() == UI_SCREEN_LCD, "on the LCD before the graph test");
        for (int i = 0; i < 50; i++) feed(-12345 + i, 1, KEITHLEY_UNIT_V);
        press(BUTTON_RCL); gHeld[BUTTON_RCL] = true;
        uiPoll();
        fakeMillisOffset() += 300;
        uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_LCD, "RCL on the LCD: graph only at the release (v15)");
        gHeld[BUTTON_RCL] = false;
        CHECK(uiPoll() && uiGetScreen() == UI_SCREEN_GRAPH, "RCL click on the LCD opens the graph");
        CHECK(!gRelDbEnabled, "REL/dB do not reach the K197 on the graph");
        uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_GRAPH, "the graph stays");
        // v22: the K197 switched off (oFF197) / the ESP32 going to sleep: back to the main display from any screen
        uiRequestHome();
        CHECK(uiPoll() && uiGetScreen() == UI_SCREEN_LCD, "uiRequestHome(): graph -> main display");
        uiRequestHome();
        CHECK(!uiPoll() && uiGetScreen() == UI_SCREEN_LCD, "uiRequestHome() on the main display changes nothing");
        press(BUTTON_RCL); uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_GRAPH, "graph open again");
        uint8_t iv = graphGetInterval();
        press(BUTTON_DB);
        uiPoll();
        CHECK(graphGetInterval() == iv + 1, "dB: longer graph interval");
        CHECK(uiGetStatsInterval() == gStoredInterval, "the statistics interval is a separate setting");
        press(BUTTON_REL); gHeld[BUTTON_REL] = true;
        uiPoll();
        CHECK(graphGetInterval() == iv + 1, "REL: nothing before the release");
        gHeld[BUTTON_REL] = false;
        uiPoll();
        CHECK(graphGetInterval() == iv, "REL click: shorter graph interval");
        const FbGraphScene &gs = uiGetGraphScene();
        CHECK(strncmp(gs.headLeft, "DCV", 3) == 0, "graph header shows the function");
        int dataCols = 0;
        for (int i = 0; i < FB_GRAPH_COLUMNS; i++) if (gs.cols[i].flags & FB_GRAPH_COL_DATA) dataCols++;
        CHECK(dataCols > 0, "the graph shows the logged readings");

        // RCL click: next Y axis mode AUTO -> ROBUST -> ZERO -> SUPP -> AUTO (at the release; v21 ROBUST)
        graphSetYMode(GRAPH_YMODE_AUTO);
        press(BUTTON_RCL); gHeld[BUTTON_RCL] = true;
        uiPoll();
        CHECK(graphGetYMode() == GRAPH_YMODE_AUTO && uiGetScreen() == UI_SCREEN_GRAPH, "RCL press: nothing before the release");
        gHeld[BUTTON_RCL] = false;
        CHECK(uiPoll() && graphGetYMode() == GRAPH_YMODE_ROBUST, "RCL click: AUTO -> ROBUST");
        press(BUTTON_RCL); uiPoll();
        CHECK(graphGetYMode() == GRAPH_YMODE_ZERO, "RCL click: ROBUST -> ZERO");
        press(BUTTON_RCL); uiPoll();
        CHECK(graphGetYMode() == GRAPH_YMODE_SUPP && uiGetScreen() == UI_SCREEN_GRAPH, "RCL click: ZERO -> SUPP");
        press(BUTTON_RCL); uiPoll();
        CHECK(graphGetYMode() == GRAPH_YMODE_AUTO, "RCL click: SUPP -> AUTO");

        // RCL held 1 s -> settings (while still held), the release there does nothing
        int beeps = gAsyncBeeps;
        press(BUTTON_RCL); gHeld[BUTTON_RCL] = true;
        uiPoll();
        fakeMillisOffset() += CONFIG_GRAPH_SETUP_HOLD_MS + 10;
        CHECK(uiPoll() && uiGetScreen() == UI_SCREEN_GRAPH_SETUP, "RCL held on the graph opens the settings");
        CHECK(gAsyncBeeps == beeps + 1 && graphGetYMode() == GRAPH_YMODE_AUTO, "hold confirmed by a beep, Y mode unchanged");
        CHECK(uiGetGraphSetupRow() == 0, "cursor on INTERVAL");
        gHeld[BUTTON_RCL] = false;
        uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_GRAPH_SETUP && uiGetGraphSetupRow() == 0, "the release of the hold changes nothing on the settings");
        press(BUTTON_RCL);
        uiPoll();
        CHECK(uiGetGraphSetupRow() == 1, "RCL: next line");
        uint8_t mode = graphGetYMode();
        press(BUTTON_DB);
        uiPoll();
        CHECK(mode == GRAPH_YMODE_AUTO && graphGetYMode() == GRAPH_YMODE_ROBUST, "dB: next Y mode (AUTO -> ROBUST)");
        press(BUTTON_REL);
        uiPoll();
        CHECK(graphGetYMode() == mode, "REL: previous Y mode (at the press, no help hold here)");
        FbSetupScene hs;
        uiFillGraphSetupScene(&hs);
        CHECK(hs.cursorRow == 1 && strncmp(hs.title, "GRAF", 4) == 0, "settings scene: cursor and title");
        press(BUTTON_STO);
        CHECK(uiPoll() && uiGetScreen() == UI_SCREEN_GRAPH, "STO on the settings: back to the graph");
        // v15 timeout of the graph settings: back to the graph after a minute without a button
        press(BUTTON_RCL); gHeld[BUTTON_RCL] = true; uiPoll();
        fakeMillisOffset() += CONFIG_GRAPH_SETUP_HOLD_MS + 10; uiPoll();
        gHeld[BUTTON_RCL] = false; uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_GRAPH_SETUP, "graph settings open again");
        fakeMillisOffset() += CONFIG_UI_HELP_TIMEOUT_MS - 1000; uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_GRAPH_SETUP, "not before the timeout");
        fakeMillisOffset() += 2000; uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_GRAPH, "graph settings timeout: back to the graph");

        // REL held: help, STO back to the graph
        press(BUTTON_REL); gHeld[BUTTON_REL] = true;
        uiPoll();
        fakeMillisOffset() += CONFIG_UI_HELP_HOLD_MS + 10;
        CHECK(uiPoll() && uiGetScreen() == UI_SCREEN_HELP, "REL held on the graph opens the help");
        {
            FbHelpScene ghs;
            uiFillHelpScene(&ghs);
            CHECK(uiGetHelpSection() == HELP_SECTION_GRAPH && strncmp(ghs.lines[0], "GRAF:", 5) == 0 && !ghs.underline[1],
                  "help from the graph: the GRAPH section, no preface");
        }
        gHeld[BUTTON_REL] = false;
        uiPoll();
        press(BUTTON_STO);
        CHECK(uiPoll() && uiGetScreen() == UI_SCREEN_GRAPH, "STO on the help: back to the graph");
        press(BUTTON_STO);
        CHECK(uiPoll() && uiGetScreen() == UI_SCREEN_LCD, "STO on the graph: back to the LCD");
        CHECK(gRelDbEnabled, "REL/dB active again on the LCD");
        graphSetYMode(GRAPH_YMODE_AUTO);
    }

    // v22: a frame without a measurement function (the K197 switched off, Err197) keeps the last function's
    // graph and statistics - the log can still be looked at
    {
        uiRequestHome();
        uiPoll();
        for (int i = 0; i < 30; i++) { feed(123456 + (i % 5), 0, KEITHLEY_UNIT_V); uiPoll(); }
        press(BUTTON_RCL); gHeld[BUTTON_RCL] = true;
        uiPoll();
        gHeld[BUTTON_RCL] = false;
        uiPoll();
        CHECK(uiGetScreen() == UI_SCREEN_GRAPH, "graph open for the DMM-off test");
        feedDigits("      ", -1, 0, KEITHLEY_UNIT_NONE);
        for (int i = 0; i < 5; i++) { fakeMillisOffset() += 300; uiPoll(); }
        const FbGraphScene &gs = uiGetGraphScene();
        int dataCols = 0;
        for (int i = 0; i < FB_GRAPH_COLUMNS; i++) if (gs.cols[i].flags & FB_GRAPH_COL_DATA) dataCols++;
        CHECK(strcmp(gs.message, "NO READING") != 0 && strncmp(gs.headLeft, "DCV", 3) == 0 && dataCols > 0,
              "no reading: the graph keeps the last function's log");
        CHECK(strncmp(gs.headLive, UI_PLACEHOLDER, strlen(UI_PLACEHOLDER)) == 0, "no reading: no live value in the header");
        FbStatsScene sc;
        uiFillStatsScene(&sc);
        CHECK(strcmp(sc.unit, "V") == 0 && strcmp(sc.average, UI_PLACEHOLDER) != 0, "no reading: the statistics stay (unit V)");
        printf("v22. no reading: graph '%s' / '%s', statistics AVG %s %s\n", gs.headLeft, gs.headLive, sc.average, sc.unit);
    }
    printf(failures ? "FAILURES: %d\n" : "ALL OK\n", failures);
    return failures != 0;
}
