// Host test for sdlog.ino (v23): SD card logging with the real datalog.ino / reading_math.ino and an in-memory
// card (stubs/SD_MMC.h). sdlogStep() (the SD task's pass) and sdlogTick() (blink / alarm beep) are called by
// hand; time is pushed forward with fakeMillisOffset().
#include <Arduino.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <vector>
#include <string>
#include <math.h>
#include "config.h"
#include "display.h"
#include "datalog.h"
#include "reading_math.h"
#include "settings.h"
#include "sdlog.h"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)
#define CHECK_STR(a, b, msg) do { if (std::string(a) != std::string(b)) { printf("FAIL: %s: got '%s' expected '%s' (line %d)\n", msg, std::string(a).c_str(), std::string(b).c_str(), __LINE__); failures++; } } while (0)

// ---- stand-ins ----
uint16_t settingsGetAutoIntervalSeconds() { return 0; }
bool settingsGetDebugEnabled() { return false; }
static bool gSdEnabled = false;
static uint8_t gCsv = SETTINGS_CSV_SEMICOLON;
static uint8_t gBeepMode = SETTINGS_BEEP_ALL;
bool settingsGetSdEnabled() { return gSdEnabled; }
uint8_t settingsGetSdCsvFormat() { return gCsv; }
uint8_t settingsGetBeepMode() { return gBeepMode; }
static bool gDmmOff = false;
bool dmmIsOff() { return gDmmOff; }
static bool gSdLit = false;
static int gSdLitChanges = 0;
void setSdIndicator(bool lit) { if (lit != gSdLit) gSdLitChanges++; gSdLit = lit; }
void displayWakeUi() {}
static int gAlarmBeeps = 0;
void beeperAlarmAsync(uint16_t) { gAlarmBeeps++; }
static bool gSwallowArmed = false;
static uint32_t gSwallowed = 0;
void buttonsArmSwallow(bool armed) { gSwallowArmed = armed; }
uint32_t buttonsSwallowedCount() { return gSwallowed; }
void debugLogf(const char *, ...) {}
void debugLogAlways(const char *, ...) {}

// ---- the real files under test ----
#include "reading_math.ino"
#undef CONFIG_DATALOG_TEST_CAPACITY
#define CONFIG_DATALOG_TEST_CAPACITY 0
#include "datalog.ino"
#include "sdlog.ino"

// ---- helpers ----
static const uint32_t T0 = 845028000UL; // 2026-10-11 10:00:00 (seconds since 2000)
static uint32_t gReadings = 0;

static void feed(uint8_t unit, uint16_t indicators = 0)
{
    fakeMillisOffset() += 300;
    gReadings++;
    fakeRtcSeconds() = T0 + gReadings / 3;
    fakeRtcCentis() = (uint8_t)((gReadings % 3) * 33);
    char d[8];
    snprintf(d, sizeof(d), "%06u", (unsigned)(100000 + gReadings % 90000));
    datalogUpdateReading(d, 1, indicators, unit);
}
static void feedN(int n, uint8_t unit)
{
    for (int i = 0; i < n; i++)
        feed(unit);
}
static void advance(uint32_t ms) { fakeMillisOffset() += ms; }

static std::vector<std::string> lines(const std::string &text)
{
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t e = text.find("\r\n", pos);
        if (e == std::string::npos)
        {
            out.push_back(text.substr(pos));
            break;
        }
        out.push_back(text.substr(pos, e - pos));
        pos = e + 2;
    }
    return out;
}
static int dataLines(const std::string &text)
{
    int n = 0;
    for (auto &l : lines(text))
        if (!l.empty() && l[0] >= '0' && l[0] <= '9')
            n++;
    return n;
}
static std::string onlyFileWith(const char *part)
{
    std::string found;
    for (auto &f : fakeSd().files)
        if (f.first.find(part) != std::string::npos)
            found = f.first;
    return found;
}
static int filesWith(const char *part)
{
    int n = 0;
    for (auto &f : fakeSd().files)
        if (f.first.find(part) != std::string::npos)
            n++;
    return n;
}

int main()
{
    ESP.freePsram = (size_t)(20000.0 * sizeof(LogRecord)) + DATALOG_PSRAM_RESERVE_BYTES + 4;
    datalogBegin();

    // 1. pure helpers: lines in both CSV formats, file names, timestamps
    {
        LogRecord r;
        memset(&r, 0, sizeof(r));
        r.timestampSeconds2000 = T0 + 3 * 3600 + 4 * 60 + 5; // 13:04:05
        r.timestampCentisecond = 7;
        r.value = 12.3456f;
        r.unit = KEITHLEY_UNIT_V;
        r.indicatorBits = KEITHLEY_BIT_AUT | DATALOG_BIT_MARKER;
        char line[SDLOG_LINE_MAX];
        sdlogFormatLine(line, sizeof(line), 42, r, SETTINGS_CSV_COMMA);
        CHECK_STR(line, "42,2026-10-11 13:04:05.07,N,DCV,+1.23456E+1,AUTO TAG\r\n", "EN line");
        sdlogFormatLine(line, sizeof(line), 42, r, SETTINGS_CSV_SEMICOLON);
        CHECK_STR(line, "42;2026-10-11 13:04:05,07;N;DCV;+1,23456E+1;AUTO TAG\r\n", "CZ line");
        r.unit = KEITHLEY_UNIT_DB;
        r.value = -3.5f;
        r.overload = false;
        r.indicatorBits = KEITHLEY_BIT_REL;
        sdlogFormatLine(line, sizeof(line), 1, r, SETTINGS_CSV_COMMA);
        CHECK_STR(line, "1,2026-10-11 13:04:05.07,Z,dB,-3.50000E+0,\r\n", "dB without the blank, REL = Z, no indicators");
        r.timestampSeconds2000 = 0;
        r.overload = true;
        sdlogFormatLine(line, sizeof(line), 3, r, SETTINGS_CSV_SEMICOLON);
        CHECK(strncmp(line, "3;NO-RTC-TIME;O;dB;", 19) == 0, "no RTC time, OL");
        char name[64];
        sdlogFileName(name, sizeof(name), T0 + 59, "DCV", 0);
        CHECK_STR(name, "/K197/2026-10/K197_2026-10-11_10-00-59_DCV.csv", "file name");
        sdlogFileName(name, sizeof(name), T0, "OHM", 3);
        CHECK_STR(name, "/K197/2026-10/K197_2026-10-11_10-00-00_OHM_3.csv", "file name with a suffix");
        sdlogFileName(name, sizeof(name), 0, "dB", 12);
        CHECK_STR(name, "/K197/NO-RTC/K197_0012_dB.csv", "file name without RTC");
        // leap day, year end
        char ts[32];
        sdlogFormatTimestamp(ts, sizeof(ts), 762480000UL + 86399, 99, false); // 2024-02-29 23:59:59
        CHECK_STR(ts, "2024-02-29 23:59:59.99", "leap day");
        sdlogFormatTimestamp(ts, sizeof(ts), 852076799UL, 0, true); // 2026-12-31 23:59:59
        CHECK_STR(ts, "2026-12-31 23:59:59,00", "year end, decimal comma");
        char sci[24];
        keithleyFormatScientific(sci, sizeof(sci), 0.000123, true);
        CHECK_STR(sci, " 1.23000E-4", "scientific, AC blank");
        printf("1. CSV lines (comma / semicolon), file names, timestamps ok\n");
    }

    // 2. off: nothing happens, no mount attempts
    {
        sdlogStep();
        sdlogTick();
        CHECK(sdlogState() == SDLOG_STATE_OFF && fakeSd().beginCalls == 0 && !gSdLit && gAlarmBeeps == 0, "off: idle");
    }

    // 3. on without a card: NOCARD, SD! blinks, the alarm beeps every period, a key silences it
    {
        gSdEnabled = true;
        sdlogStep();
        CHECK(sdlogState() == SDLOG_STATE_NO_CARD && fakeSd().beginCalls == 1, "no card: one mount attempt");
        sdlogTick();
        CHECK(sdlogProblem() && gSdLit && gAlarmBeeps == 1 && gSwallowArmed, "alarm: lit, first beep at once, swallow armed");
        int changes = gSdLitChanges;
        for (int i = 0; i < 10; i++) { advance(CONFIG_SD_BLINK_MS); sdlogTick(); }
        CHECK(gSdLitChanges == changes + 10 && gAlarmBeeps == 1, "blinks every tick, no second beep within the period");
        advance(CONFIG_SD_ALARM_PERIOD_MS); sdlogTick();
        CHECK(gAlarmBeeps == 2, "the next beep after the period");
        gSwallowed++; // a key was pressed (buttons.ino swallowed it)
        advance(CONFIG_SD_BLINK_MS); sdlogTick();
        CHECK(!gSwallowArmed, "silenced: no more swallowing");
        advance(3 * CONFIG_SD_ALARM_PERIOD_MS); sdlogTick();
        CHECK(gAlarmBeeps == 2 && sdlogProblem(), "silenced: no beeps, still a problem (blinking)");
        sdlogStep(); // due long ago: a new attempt
        int begins = fakeSd().beginCalls;
        advance(CONFIG_SD_RETRY_MS / 2); sdlogStep();
        CHECK(fakeSd().beginCalls == begins && begins == 2, "no new mount attempt before CONFIG_SD_RETRY_MS");
        advance(CONFIG_SD_RETRY_MS); sdlogStep();
        CHECK(fakeSd().beginCalls == begins + 1, "retried");
        // sound OFF: the alarm is silent and does not swallow keys
        gSdEnabled = false; sdlogStep(); sdlogTick();
        CHECK(!sdlogProblem() && !gSdLit, "logging off: no problem");
        gSdEnabled = true; sdlogStep();
        gBeepMode = SETTINGS_BEEP_OFF;
        int beeps = gAlarmBeeps;
        sdlogTick();
        CHECK(sdlogProblem() && !gSwallowArmed, "sound OFF: alarm shown, keys not swallowed");
        CHECK(gAlarmBeeps == beeps + 0, "sound OFF: beeperAlarmAsync not even asked");
        gBeepMode = SETTINGS_BEEP_ALL;
        // the K197 off: no alarm
        gDmmOff = true; advance(CONFIG_SD_BLINK_MS); sdlogTick();
        CHECK(!sdlogProblem() && !gSdLit, "K197 off: no SD alarm");
        gDmmOff = false;
        printf("3. no card: blinking, beep every %d ms, silenced by a key, sound OFF, K197 off ok\n", CONFIG_SD_ALARM_PERIOD_MS);
    }

    // 4. card inserted: mounted, a file opens with the first reading; written every CONFIG_SD_FLUSH_MS
    std::string file1;
    {
        fakeSd().present = true;
        advance(CONFIG_SD_RETRY_MS); sdlogStep();
        CHECK(sdlogState() == SDLOG_STATE_READY && fakeSd().mounted, "card mounted, ready");
        advance(CONFIG_SD_BLINK_MS); sdlogTick();
        CHECK(!sdlogProblem() && !gSdLit && !gSwallowArmed, "alarm over");
        feedN(30, KEITHLEY_UNIT_V);
        sdlogStep();
        file1 = onlyFileWith("_DCV");
        CHECK(sdlogState() == SDLOG_STATE_LOGGING && file1.find("/K197/2026-10/K197_2026-10-11_10-00-") == 0, "DCV file created");
        CHECK(fakeSd().files[file1].empty(), "nothing on the card before the flush interval");
        SdlogStats st;
        sdlogGetStats(&st);
        CHECK(st.fileRecords == 30 - CONFIG_SD_LAG_RECORDS, "the newest CONFIG_SD_LAG_RECORDS wait (markers)");
        advance(CONFIG_SD_QUIET_MS + 10); sdlogStep();
        sdlogGetStats(&st);
        CHECK(st.fileRecords == 30 && st.backlog == 0, "quiet log: all written to the buffer");
        advance(CONFIG_SD_FLUSH_MS); sdlogStep();
        std::string text = fakeSd().files[file1];
        auto ls = lines(text);
        CHECK(ls.size() >= 5 && ls[0].find("# KEITHLEY 197R firmware ") == 0 && ls[1] == "# function DCV unit V" &&
              ls[3] == "# CSV separator ';' decimal ','" && ls[4] == "record;timestamp;status;function;value;indicators",
              "header lines");
        CHECK(dataLines(text) == 30, "30 records synced after the flush interval");
        CHECK(ls[5].find("1;2026-10-11 10:00:") == 0 && ls[5].find(";N;DCV;+") != std::string::npos, "first record line");
        CHECK(ls[34].find("30;") == 0, "numbered 1..30 in the file");
        CHECK(text.find("SETTLE") != std::string::npos, "the settling readings carry SETTLE");
        printf("4. card in: %s, %d records after %d s\n", file1.c_str(), dataLines(text), CONFIG_SD_FLUSH_MS / 1000);
    }

    // 5. a marker: written and synced at once, up to and including the marked record; a late marker = comment line
    {
        feedN(20, KEITHLEY_UNIT_V);
        sdlogStep();
        int before = dataLines(fakeSd().files[file1]);
        uint32_t marked = 0;
        CHECK(datalogMarkAtMs((uint32_t)millis(), &marked), "marker set on the newest record");
        sdlogStep();
        std::string text = fakeSd().files[file1];
        CHECK(dataLines(text) == 50 && before == 30, "marker: synced at once, up to the marked (newest) record");
        auto ls = lines(text);
        CHECK(ls[54].find("50;") == 0 && ls[54].find("TAG") != std::string::npos, "the marked record carries TAG");
        datalogMarkAtMs((uint32_t)millis(), NULL); // the same record again: already written
        datalogMarkAtMs((uint32_t)millis() - 600, NULL); // an older one: 2 records back (record 48)
        feedN(2, KEITHLEY_UNIT_V);
        sdlogStep();
        advance(CONFIG_SD_FLUSH_MS); sdlogStep();
        text = fakeSd().files[file1];
        CHECK(text.find("# TAG: record 50\r\n") != std::string::npos, "late marker on a written record: comment line");
        CHECK(text.find("# TAG: record 48\r\n") != std::string::npos, "late marker 2 records back: comment line");
        printf("5. markers: immediate sync, late markers as comments ok\n");
    }

    // 6. function change: the DCV file is closed complete, an OHM file opens
    std::string file2;
    {
        feedN(5, KEITHLEY_UNIT_V);
        feedN(15, KEITHLEY_UNIT_KOHM);
        advance(CONFIG_SD_QUIET_MS + 10);
        sdlogStep();
        file2 = onlyFileWith("_OHM");
        CHECK(!file2.empty(), "OHM file opened");
        CHECK(dataLines(fakeSd().files[file1]) == 57, "DCV file closed with all its records (flushed at the change)");
        CHECK(lines(fakeSd().files[file1]).back().find("57;") == 0, "last DCV line");
        advance(CONFIG_SD_FLUSH_MS); sdlogStep();
        auto ls = lines(fakeSd().files[file2]);
        CHECK(ls.size() > 1 && ls[1] == "# function OHM unit Ohm" && dataLines(fakeSd().files[file2]) == 15, "OHM file: header + 15 records");
        printf("6. function change: %s (57 records) closed, %s opened\n", file1.c_str(), file2.c_str());
    }

    // 7. the K197 switched off: everything written, the file closed (sleep allowed); on again = a new file
    {
        feedN(10, KEITHLEY_UNIT_KOHM);
        sdlogStep();
        CHECK(!sdlogReadyForSleep(), "a file is open: no sleep");
        gDmmOff = true;
        sdlogStep();
        CHECK(dataLines(fakeSd().files[file2]) == 25 && sdlogReadyForSleep() && sdlogState() == SDLOG_STATE_READY,
              "K197 off: all written, closed, ready for sleep");
        int begins = fakeSd().beginCalls;
        advance(10 * CONFIG_SD_RETRY_MS); sdlogStep();
        CHECK(fakeSd().beginCalls == begins, "nothing else while off");
        gDmmOff = false;
        advance(1000); // a new second: a new file name
        gReadings += 30;
        feedN(12, KEITHLEY_UNIT_KOHM);
        advance(CONFIG_SD_QUIET_MS + 10);
        sdlogStep();
        CHECK(filesWith("_OHM") == 2, "K197 on again: a new OHM file");
        printf("7. K197 off/on: closed, new file ok\n");
    }

    // 8. card removed with unsynced records: they are written to the next card (new file), nothing lost
    {
        feedN(10, KEITHLEY_UNIT_KOHM);
        advance(CONFIG_SD_QUIET_MS + 10);
        sdlogStep(); // 22 records in the buffer, none synced yet
        std::string current;
        for (auto &f : fakeSd().files) if (f.first.find("_OHM") != std::string::npos && f.first != file2) current = f.first;
        CHECK(dataLines(fakeSd().files[current]) == 0, "nothing synced yet");
        FakeSdCard old = fakeSd();
        fakeSd().present = false;
        advance(CONFIG_SD_PROBE_MS); sdlogStep();
        CHECK(sdlogState() == SDLOG_STATE_NO_CARD && !fakeSd().mounted, "removed: found by the probe");
        advance(CONFIG_SD_BLINK_MS); sdlogTick();
        CHECK(sdlogProblem() && gAlarmBeeps > 0, "alarm on");
        feedN(8, KEITHLEY_UNIT_KOHM);
        // a new, empty card
        fakeSd().files.clear(); fakeSd().dirs.clear(); fakeSd().used = 0;
        fakeSd().present = true;
        advance(CONFIG_SD_RETRY_MS); sdlogStep();
        advance(CONFIG_SD_QUIET_MS + 10); sdlogStep();
        advance(CONFIG_SD_FLUSH_MS); sdlogStep();
        std::string f = onlyFileWith("_OHM");
        CHECK(!f.empty() && dataLines(fakeSd().files[f]) == 30, "new card: the 22 unsynced + 8 new records, nothing lost");
        SdlogStats st;
        sdlogGetStats(&st);
        CHECK(st.lostRecords == 0 && st.mounts == 2, "no records lost, 2 mounts");
        printf("8. card swap: %d records rescued into %s\n", dataLines(fakeSd().files[f]), f.c_str());
    }

    // 9. a cleared log continues in the same file; a write error -> ERROR, remount, new file
    {
        std::string f = onlyFileWith("_OHM");
        datalogClear();
        feedN(5, KEITHLEY_UNIT_KOHM);
        sdlogStep();
        advance(CONFIG_SD_QUIET_MS + 10); sdlogStep();
        advance(CONFIG_SD_FLUSH_MS); sdlogStep();
        CHECK(dataLines(fakeSd().files[f]) == 35 && filesWith("_OHM") == 1, "log cleared: same file continues");
        if (dataLines(fakeSd().files[f]) != 35) { printf("   %d lines, %d files\n", dataLines(fakeSd().files[f]), filesWith("_OHM")); for (auto &x : fakeSd().files) printf("   %s %d\n", x.first.c_str(), dataLines(x.second)); }
        feedN(5, KEITHLEY_UNIT_KOHM);
        fakeSd().failWrites = true;
        advance(CONFIG_SD_QUIET_MS + 10); sdlogStep();
        datalogMarkAtMs((uint32_t)millis(), NULL); // forces the write
        sdlogStep();
        CHECK(sdlogState() == SDLOG_STATE_ERROR && !fakeSd().mounted, "write error: ERROR, unmounted");
        fakeSd().failWrites = false;
        advance(CONFIG_SD_RETRY_MS); sdlogStep();
        advance(CONFIG_SD_QUIET_MS + 10); sdlogStep();
        CHECK(sdlogState() == SDLOG_STATE_LOGGING && filesWith("_OHM") == 2, "remounted, the 5 records in a new file");
        SdlogStats st;
        sdlogGetStats(&st);
        CHECK(st.writeErrors == 1, "write error counted");
        printf("9. log clear, write error + remount ok\n");
    }

    // 10. card full: FULL (a problem), nothing written, stays mounted; logging off: closed, unmounted
    {
        fakeSd().total = fakeSd().used + (uint64_t)CONFIG_SD_MIN_FREE_KB * 1024ULL + 1000;
        fakeSd().present = false;
        advance(CONFIG_SD_PROBE_MS); sdlogStep();
        fakeSd().present = true;
        advance(CONFIG_SD_RETRY_MS); sdlogStep();
        feedN(100, KEITHLEY_UNIT_KOHM);
        advance(CONFIG_SD_QUIET_MS + 10); sdlogStep();
        advance(CONFIG_SD_FLUSH_MS); sdlogStep();
        CHECK(sdlogState() == SDLOG_STATE_FULL && fakeSd().mounted, "card full: FULL, stays mounted");
        advance(CONFIG_SD_BLINK_MS); sdlogTick();
        CHECK(sdlogProblem(), "full card is a problem");
        gSdEnabled = false;
        sdlogStep();
        CHECK(sdlogState() == SDLOG_STATE_OFF && !fakeSd().mounted && sdlogReadyForSleep(), "logging off: unmounted");
        advance(CONFIG_SD_BLINK_MS); sdlogTick();
        CHECK(!sdlogProblem() && !gSdLit, "off: no alarm");
        printf("10. card full, logging off ok\n");
    }

    // 11. comma format; no RTC time -> NO-RTC names with a running number
    {
        fakeSd().total = 1024ULL * 1024ULL * 1024ULL;
        gCsv = SETTINGS_CSV_COMMA;
        gSdEnabled = true;
        sdlogStep();
        uint32_t keep = T0;
        (void)keep;
        // readings without RTC time
        for (int i = 0; i < 12; i++)
        {
            fakeMillisOffset() += 300;
            fakeRtcSeconds() = 0;
            char d[7] = "123456";
            datalogUpdateReading(d, 1, 0, KEITHLEY_UNIT_MA);
        }
        advance(CONFIG_SD_QUIET_MS + 10); sdlogStep();
        gSdEnabled = false; sdlogStep(); // closes
        std::string f = onlyFileWith("/K197/NO-RTC/K197_0001_DCI.csv");
        CHECK(!f.empty() && dataLines(fakeSd().files[f]) == 12, "NO-RTC file 0001");
        if (f.empty() || dataLines(fakeSd().files[f]) != 12) for (auto &x : fakeSd().files) printf("   %s %d\n", x.first.c_str(), dataLines(x.second));
        auto ls = lines(fakeSd().files[f]);
        CHECK(ls[4] == "record,timestamp,status,function,value,indicators" && ls[5].find("1,NO-RTC-TIME,N,DCI,+") == 0,
              "comma format, no time");
        gSdEnabled = true; sdlogStep();
        for (int i = 0; i < 3; i++)
        {
            fakeMillisOffset() += 300;
            char d[7] = "123456";
            datalogUpdateReading(d, 1, 0, KEITHLEY_UNIT_MA);
        }
        sdlogStep();
        advance(CONFIG_SD_QUIET_MS + 10); sdlogStep();
        CHECK(filesWith("/K197/NO-RTC/K197_0002_DCI.csv") == 1, "the next NO-RTC file gets 0002");
        printf("11. comma format, NO-RTC names ok\n");
    }

    printf(failures ? "FAILURES: %d\n" : "ALL OK\n", failures);
    return failures ? 1 : 0;
}
