// sdlog.ino
// v23: SD card logging - see sdlog.h.
//
// Threads: sdlogStep() runs only in the SD task (it alone touches the card,
// the file and the buffer). sdlogTick() runs in the esp_timer task (blink,
// alarm beep, silencing). The state and the statistics are read from the
// other tasks (UI, SCPI, power) - plain aligned 8/32-bit values, and a copy
// of the statistics under sdStatsMux.
//
// Debug output: state changes, mounts, files, removal and silencing go through
// debugLogf() - only with SYSTem:DEBUG ON (the debug port may be the SCPI
// UART). Only real failures (a file cannot be created, a write fails) use
// debugLogAlways(), like the other modules' error reports.

#include "sdlog.h"
#include "config.h"
#include "settings.h"
#include "datalog.h"
#include "reading_math.h"
#include "display.h"
#include "beeper.h"
#include "buttons.h"
#include "debuglog.h"
#include "rtc.h"
#include "build_info.h"
#include <FS.h>
#include <SD_MMC.h>
#include <esp_timer.h>
#ifdef ARDUINO
#include <sys/time.h>
#endif

// dmm_bridge.ino (no header of its own - Arduino generates the prototype)
bool dmmIsOff();

#define SDLOG_BATCH_RECORDS 32       // records read from the log under one mutex hold
#define SDLOG_STEP_MAX_RECORDS 4096  // records handled in one pass at most (a long backlog takes several)
#define SDLOG_LINE_MAX 128           // longest CSV line (record, timestamp, status, function, value, indicators)
#define SDLOG_SECONDS_1970_TO_2000 946684800UL

// ---- state of the SD task ----
static volatile uint8_t sdState = SDLOG_STATE_OFF;
static bool sdMounted = false;
static bool sdMountTried = false;          // at least one mount attempt since logging was switched on
static volatile bool sdBusy = false;       // a card operation is running (no light sleep now)
static File sdFile;
static bool sdFileOpen = false;
static char sdFileFunction[4] = "";
static uint32_t sdFileRecords = 0;
static uint32_t sdFileFirstSerial = 0;     // serial of the file's first record (late markers)
static bool sdFileSerialsValid = false;    // false after a clear of the log (serials started again)
static bool sdNewFileRequested = true;
static bool sdSerialValid = false;         // sdNextSerial is set up (false while logging is off)
static uint32_t sdNextSerial = 0;          // the next record to write
static uint32_t sdSyncedSerial = 0;        // every record before this one is synced to the card
static uint32_t sdBufEndSerial = 0;        // every record before this one has its line in the buffer / file
static uint32_t sdClearGen = 0;
static uint32_t sdMarkSeen = 0;            // markers handled so far (datalogMarkCount())
static uint32_t sdLastEnd = 0;
static uint32_t sdLastEndChangeMs = 0;
static uint32_t sdLastMountTryMs = 0;
static uint32_t sdLastProbeMs = 0;
static char sdBuf[CONFIG_SD_BUFFER_BYTES];
static size_t sdBufLen = 0;
static bool sdUnsynced = false;            // written to the file but not synced yet
static uint32_t sdUnsyncedSinceMs = 0;     // the oldest line not synced yet
static uint64_t sdFreeBytes = 0;

// ---- statistics (copied under the mux) ----
static portMUX_TYPE sdStatsMux = portMUX_INITIALIZER_UNLOCKED;
static SdlogStats sdStats;

// ---- alarm (esp_timer task) ----
static esp_timer_handle_t sdTickTimer = NULL;
static volatile bool sdAlarmActive = false;
static volatile bool sdSilenced = false;
static bool sdBlinkOn = false;
static bool sdSwallowArmed = false;
static uint32_t sdLastBeepMs = 0;
static uint32_t sdSwallowSeen = 0;

static volatile TaskHandle_t sdTaskHandle = NULL;

// =====================================================================
// Pure helpers
// =====================================================================

// Civil date and time from seconds since 2000-01-01 (H. Hinnant's days -> civil).
static void sdlogCivil(uint32_t seconds2000, uint16_t *year, uint8_t *month, uint8_t *day, uint8_t *hour,
                       uint8_t *minute, uint8_t *second)
{
    uint32_t days = seconds2000 / 86400UL;
    uint32_t rem = seconds2000 % 86400UL;
    *hour = (uint8_t)(rem / 3600UL);
    *minute = (uint8_t)((rem % 3600UL) / 60UL);
    *second = (uint8_t)(rem % 60UL);
    long z = (long)days + 10957L + 719468L; // days since 0000-03-01
    long era = z / 146097L;
    long doe = z - era * 146097L;
    long yoe = (doe - doe / 1460L + doe / 36524L - doe / 146096L) / 365L;
    long y = yoe + era * 400L;
    long doy = doe - (365L * yoe + yoe / 4L - yoe / 100L);
    long mp = (5L * doy + 2L) / 153L;
    long d = doy - (153L * mp + 2L) / 5L + 1L;
    long m = (mp < 10L) ? mp + 3L : mp - 9L;
    *year = (uint16_t)(y + ((m <= 2L) ? 1L : 0L));
    *month = (uint8_t)m;
    *day = (uint8_t)d;
}

void sdlogFormatTimestamp(char *buf, size_t size, uint32_t seconds2000, uint8_t centisecond, bool decimalComma)
{
    if (seconds2000 == 0)
    {
        snprintf(buf, size, "NO-RTC-TIME");
        return;
    }
    uint16_t year;
    uint8_t month, day, hour, minute, second;
    sdlogCivil(seconds2000, &year, &month, &day, &hour, &minute, &second);
    snprintf(buf, size, "%04u-%02u-%02u %02u:%02u:%02u%c%02u", (unsigned)year, (unsigned)month, (unsigned)day,
             (unsigned)hour, (unsigned)minute, (unsigned)second, decimalComma ? ',' : '.', (unsigned)(centisecond % 100));
}

// The function of a record without the blank of " dB"; "UNK" for no unit.
static const char *sdlogFunctionOf(const LogRecord &rec)
{
    const char *prefix = keithleyFunctionPrefix(rec.unit, rec.indicatorBits);
    while (*prefix == ' ')
    {
        prefix++;
    }
    return (*prefix == '\0') ? "UNK" : prefix;
}

// Base unit of a function (the value column).
static const char *sdlogUnitOf(const char *function)
{
    if (strcmp(function, "DCV") == 0 || strcmp(function, "ACV") == 0)
    {
        return "V";
    }
    if (strcmp(function, "DCI") == 0 || strcmp(function, "ACI") == 0)
    {
        return "A";
    }
    if (strcmp(function, "OHM") == 0)
    {
        return "Ohm";
    }
    if (strcmp(function, "dB") == 0)
    {
        return "dB";
    }
    return "?";
}

size_t sdlogFormatLine(char *buf, size_t size, uint32_t recordNo, const LogRecord &rec, uint8_t csvFormat)
{
    bool semicolon = (csvFormat == SETTINGS_CSV_SEMICOLON);
    char sep = semicolon ? ';' : ',';
    char timestamp[32];
    sdlogFormatTimestamp(timestamp, sizeof(timestamp), rec.timestampSeconds2000, rec.timestampCentisecond, semicolon);
    char value[24];
    keithleyFormatScientific(value, sizeof(value), rec.value, false); // like LOG:DATA?: always a real sign
    if (semicolon)
    {
        for (char *p = value; *p != '\0'; p++)
        {
            if (*p == '.')
            {
                *p = ',';
            }
        }
    }
    char indicators[48];
    datalogFormatIndicators(indicators, sizeof(indicators), rec.indicatorBits);
    char status = rec.overload ? 'O' : ((rec.indicatorBits & KEITHLEY_BIT_REL) ? 'Z' : 'N');
    int n = snprintf(buf, size, "%lu%c%s%c%c%c%s%c%s%c%s\r\n", (unsigned long)recordNo, sep, timestamp, sep, status, sep,
                     sdlogFunctionOf(rec), sep, value, sep, indicators);
    if (n < 0)
    {
        buf[0] = '\0';
        return 0;
    }
    return ((size_t)n < size) ? (size_t)n : size - 1;
}

void sdlogFileName(char *buf, size_t size, uint32_t seconds2000, const char *function, uint16_t suffix)
{
    if (seconds2000 == 0)
    {
        snprintf(buf, size, "/K197/NO-RTC/K197_%04u_%s.csv", (unsigned)suffix, function);
        return;
    }
    uint16_t year;
    uint8_t month, day, hour, minute, second;
    sdlogCivil(seconds2000, &year, &month, &day, &hour, &minute, &second);
    char tail[8] = "";
    if (suffix >= 2)
    {
        snprintf(tail, sizeof(tail), "_%u", (unsigned)suffix);
    }
    snprintf(buf, size, "/K197/%04u-%02u/K197_%04u-%02u-%02u_%02u-%02u-%02u_%s%s.csv", (unsigned)year, (unsigned)month,
             (unsigned)year, (unsigned)month, (unsigned)day, (unsigned)hour, (unsigned)minute, (unsigned)second, function,
             tail);
}

const char *sdlogStateName(uint8_t state)
{
    static const char *const names[SDLOG_STATE_COUNT] = {"OFF", "NOCARD", "READY", "LOGGING", "FULL", "ERROR"};
    return (state < SDLOG_STATE_COUNT) ? names[state] : "?";
}

// =====================================================================
// SD task
// =====================================================================

static void sdlogSetState(uint8_t state)
{
    if (sdState != state)
    {
        debugLogf("sd: %s -> %s", sdlogStateName(sdState), sdlogStateName(state));
        sdState = state;
        displayWakeUi(); // the DMM settings screen shows the state
    }
}

// The system time from the RTC: the FAT file and directory dates come from it.
static void sdlogSetSystemTime()
{
#ifdef ARDUINO
    uint32_t seconds2000 = 0;
    uint8_t centis = 0;
    if (rtcCompactTimestamp(&seconds2000, &centis))
    {
        struct timeval tv;
        tv.tv_sec = (time_t)(seconds2000 + SDLOG_SECONDS_1970_TO_2000);
        tv.tv_usec = (long)centis * 10000L;
        settimeofday(&tv, NULL);
    }
#endif
}

static void sdlogStatsUpdate()
{
    portENTER_CRITICAL(&sdStatsMux);
    sdStats.state = sdState;
    sdStats.fileRecords = sdFileRecords;
    sdStats.freeBytes = sdMounted ? sdFreeBytes : 0;
    portEXIT_CRITICAL(&sdStatsMux);
}

static bool sdlogMount()
{
    sdBusy = true;
    SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN);
    bool ok = SD_MMC.begin("/sdcard", true, false, CONFIG_SD_FREQ_KHZ, 2);
    if (ok && SD_MMC.cardType() == CARD_NONE)
    {
        SD_MMC.end();
        ok = false;
    }
    if (ok)
    {
        uint64_t total = SD_MMC.totalBytes();
        uint64_t used = SD_MMC.usedBytes();
        sdFreeBytes = (total > used) ? total - used : 0;
        sdMounted = true;
        sdlogSetSystemTime();
        SD_MMC.mkdir("/K197");
        portENTER_CRITICAL(&sdStatsMux);
        sdStats.mounts++;
        portEXIT_CRITICAL(&sdStatsMux);
        debugLogf("sd: card mounted, %lu MB free", (unsigned long)(sdFreeBytes / (1024ULL * 1024ULL)));
    }
    sdBusy = false;
    return ok;
}

static void sdlogUnmount()
{
    if (!sdMounted)
    {
        return;
    }
    sdBusy = true;
    SD_MMC.end();
    sdBusy = false;
    sdMounted = false;
}

// A removed card is found by reading sector 0 (Arduino ESP32 core 3.x).
static bool sdlogProbe()
{
#if CONFIG_SD_PROBE_MS > 0 && defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    static uint8_t sector[512] __attribute__((aligned(4)));
    sdBusy = true;
    bool ok = SD_MMC.readRAW(sector, 0);
    sdBusy = false;
    return ok;
#else
    return true;
#endif
}

static bool sdlogFreeEnough(size_t moreBytes)
{
    return sdFreeBytes >= (uint64_t)moreBytes + (uint64_t)CONFIG_SD_MIN_FREE_KB * 1024ULL;
}

// The card failed (newState NO_CARD / ERROR: unmounted; FULL: stays mounted). The file is dropped without
// writing, the buffer emptied, and the log is read again from the first record not synced yet - into a new file
// on the next card.
static void sdlogFail(uint8_t newState)
{
    if (sdFileOpen)
    {
        sdBusy = true;
        sdFile.close();
        sdBusy = false;
        sdFileOpen = false;
    }
    sdBufLen = 0;
    sdUnsynced = false;
    sdNextSerial = sdSyncedSerial;
    sdBufEndSerial = sdSyncedSerial;
    sdNewFileRequested = true;
    if (newState != SDLOG_STATE_FULL)
    {
        sdlogUnmount();
        sdLastMountTryMs = (uint32_t)millis(); // the next attempt after CONFIG_SD_RETRY_MS
    }
    if (newState == SDLOG_STATE_ERROR)
    {
        portENTER_CRITICAL(&sdStatsMux);
        sdStats.writeErrors++;
        portEXIT_CRITICAL(&sdStatsMux);
    }
    sdlogSetState(newState);
}

// Writes the buffer to the file; with sync also flushes the file to the card. False on a failure (already
// handled by sdlogFail()).
static bool sdlogWriteOut(bool sync)
{
    if (!sdFileOpen)
    {
        sdBufLen = 0;
        return true;
    }
    uint32_t t0 = (uint32_t)millis();
    bool wrote = false;
    if (sdBufLen > 0)
    {
        if (!sdlogFreeEnough(sdBufLen))
        {
            debugLogf("sd: card full");
            sdlogFail(SDLOG_STATE_FULL);
            return false;
        }
        sdBusy = true;
        size_t written = sdFile.write((const uint8_t *)sdBuf, sdBufLen);
        sdBusy = false;
        if (written != sdBufLen)
        {
            debugLogAlways("sd: write failed (%u of %u B)", (unsigned)written, (unsigned)sdBufLen);
            sdlogFail(SDLOG_STATE_ERROR);
            return false;
        }
        sdFreeBytes -= sdBufLen;
        sdBufLen = 0;
        sdUnsynced = true;
        wrote = true;
    }
    if (sync && sdUnsynced)
    {
        sdlogSetSystemTime(); // the file's modification time
        sdBusy = true;
        sdFile.flush();
        sdBusy = false;
        sdUnsynced = false;
        sdSyncedSerial = sdBufEndSerial;
        wrote = true;
    }
    if (wrote)
    {
        uint32_t ms = (uint32_t)millis() - t0;
        portENTER_CRITICAL(&sdStatsMux);
        sdStats.lastWriteMs = ms;
        if (ms > sdStats.maxWriteMs)
        {
            sdStats.maxWriteMs = ms;
        }
        portEXIT_CRITICAL(&sdStatsMux);
    }
    return true;
}

// Appends text to the buffer (written out first if it would not fit).
static bool sdlogAppend(const char *text, size_t len)
{
    if (sdBufLen + len > sizeof(sdBuf))
    {
        if (!sdlogWriteOut(false))
        {
            return false;
        }
    }
    if (sdBufLen == 0 && !sdUnsynced)
    {
        sdUnsyncedSinceMs = (uint32_t)millis();
    }
    memcpy(sdBuf + sdBufLen, text, len);
    sdBufLen += len;
    return true;
}

static bool sdlogCloseFile()
{
    if (!sdFileOpen)
    {
        return true;
    }
    if (!sdlogWriteOut(true))
    {
        return false;
    }
    sdBusy = true;
    sdFile.close();
    sdBusy = false;
    sdFileOpen = false;
    debugLogf("sd: file closed, %lu records", (unsigned long)sdFileRecords);
    return true;
}

// Creates the directories of a path ("/K197/2026-10/...": /K197 and /K197/2026-10).
static void sdlogMakeDirs(const char *path)
{
    char dir[64];
    for (const char *p = path + 1; *p != '\0'; p++)
    {
        if (*p == '/')
        {
            size_t n = (size_t)(p - path);
            if (n >= sizeof(dir))
            {
                return;
            }
            memcpy(dir, path, n);
            dir[n] = '\0';
            if (!SD_MMC.exists(dir))
            {
                SD_MMC.mkdir(dir);
            }
        }
    }
}

static bool sdlogOpenFile(const LogRecord &first, const char *function, uint32_t serial)
{
    char path[64];
    uint16_t suffix = (first.timestampSeconds2000 == 0) ? 1 : 0;
    sdBusy = true;
    sdlogSetSystemTime();
    sdlogFileName(path, sizeof(path), first.timestampSeconds2000, function, suffix);
    sdlogMakeDirs(path);
    while (SD_MMC.exists(path) && suffix < 9999)
    {
        suffix = (uint16_t)((suffix == 0) ? 2 : suffix + 1);
        sdlogFileName(path, sizeof(path), first.timestampSeconds2000, function, suffix);
    }
    sdFile = SD_MMC.open(path, FILE_WRITE);
    sdBusy = false;
    if (!sdFile)
    {
        debugLogAlways("sd: cannot create %s", path);
        sdlogFail(SDLOG_STATE_ERROR);
        return false;
    }
    sdFileOpen = true;
    sdUnsynced = false;
    strncpy(sdFileFunction, function, sizeof(sdFileFunction) - 1);
    sdFileFunction[sizeof(sdFileFunction) - 1] = '\0';
    sdFileRecords = 0;
    sdFileFirstSerial = serial;
    sdFileSerialsValid = true;
    portENTER_CRITICAL(&sdStatsMux);
    strncpy(sdStats.file, path, sizeof(sdStats.file) - 1);
    sdStats.file[sizeof(sdStats.file) - 1] = '\0';
    sdStats.files++;
    portEXIT_CRITICAL(&sdStatsMux);
    debugLogf("sd: new file %s", path);

    // the comment lines and the column header
    bool semicolon = (settingsGetSdCsvFormat() == SETTINGS_CSV_SEMICOLON);
    char sep = semicolon ? ';' : ',';
    char stamp[24];
    buildInfoFormatStamp(stamp, sizeof(stamp));
    char start[32];
    sdlogFormatTimestamp(start, sizeof(start), first.timestampSeconds2000, first.timestampCentisecond, semicolon);
    char header[320];
    int n = snprintf(header, sizeof(header),
                     "# KEITHLEY 197R firmware %s build %s\r\n"
                     "# function %s unit %s\r\n"
                     "# start %s\r\n"
                     "# CSV separator '%c' decimal '%c'\r\n"
                     "record%ctimestamp%cstatus%cfunction%cvalue%cindicators\r\n",
                     BUILD_FIRMWARE, stamp, function, sdlogUnitOf(function), start, sep, semicolon ? ',' : '.', sep, sep,
                     sep, sep, sep);
    if (n > 0)
    {
        if (!sdlogAppend(header, ((size_t)n < sizeof(header)) ? (size_t)n : sizeof(header) - 1))
        {
            return false;
        }
    }
    sdlogSetState(SDLOG_STATE_LOGGING);
    return true;
}

// One record into the file (a new file first if needed). False on a card failure.
static bool sdlogHandleRecord(const LogRecord &rec, uint32_t serial)
{
    const char *function = sdlogFunctionOf(rec);
    if (!sdFileOpen || sdNewFileRequested || strcmp(function, sdFileFunction) != 0)
    {
        if (!sdlogCloseFile())
        {
            return false;
        }
        if (!sdlogOpenFile(rec, function, serial))
        {
            return false;
        }
        sdNewFileRequested = false;
    }
    char line[SDLOG_LINE_MAX];
    size_t len = sdlogFormatLine(line, sizeof(line), sdFileRecords + 1, rec, settingsGetSdCsvFormat());
    if (!sdlogAppend(line, len))
    {
        return false;
    }
    sdFileRecords++;
    sdBufEndSerial = serial + 1;
    portENTER_CRITICAL(&sdStatsMux);
    sdStats.records++;
    portEXIT_CRITICAL(&sdStatsMux);
    return true;
}

// Writes the records sdNextSerial .. limit - 1. False on a card failure.
static bool sdlogConsume(uint32_t limit)
{
    uint32_t handled = 0;
    while (sdNextSerial < limit && handled < SDLOG_STEP_MAX_RECORDS)
    {
        LogRecord recs[SDLOG_BATCH_RECORDS];
        uint32_t serial = sdNextSerial;
        uint32_t want = limit - sdNextSerial;
        if (want > SDLOG_BATCH_RECORDS)
        {
            want = SDLOG_BATCH_RECORDS;
        }
        uint32_t n = datalogReadSerial(&serial, recs, want, NULL);
        if (serial != sdNextSerial)
        {
            // overwritten in PSRAM before they reached the card (or the log was cleared meanwhile)
            if (serial > sdNextSerial)
            {
                portENTER_CRITICAL(&sdStatsMux);
                sdStats.lostRecords += serial - sdNextSerial;
                portEXIT_CRITICAL(&sdStatsMux);
            }
            sdNextSerial = serial;
        }
        if (n == 0)
        {
            break;
        }
        for (uint32_t i = 0; i < n; i++)
        {
            if (!sdlogHandleRecord(recs[i], serial + i))
            {
                return false;
            }
            sdNextSerial = serial + i + 1;
        }
        handled += n;
    }
    return true;
}

// Markers set since the last pass: the limit is moved to include their records (written + synced in this pass);
// a marker on a record already written gets a comment line. Returns true if there was a new marker.
static bool sdlogTakeMarkers(uint32_t end, uint32_t *limit)
{
    uint32_t count = datalogMarkCount();
    bool any = false;
    while (sdMarkSeen != count)
    {
        uint32_t serial = 0;
        if (datalogMarkSerialAt(sdMarkSeen, &serial))
        {
            any = true;
            if (serial >= sdNextSerial)
            {
                if (serial < end && serial + 1 > *limit)
                {
                    *limit = serial + 1;
                }
            }
            else if (sdFileOpen && sdFileSerialsValid && serial >= sdFileFirstSerial)
            {
                uint32_t recordNo = sdFileRecords - (sdBufEndSerial - 1 - serial);
                char line[48];
                int n = snprintf(line, sizeof(line), "# TAG: record %lu\r\n", (unsigned long)recordNo);
                if (n > 0)
                {
                    sdlogAppend(line, (size_t)n);
                }
            }
        }
        sdMarkSeen++;
    }
    return any;
}

void sdlogStep()
{
    uint32_t nowMs = (uint32_t)millis();
    if (!settingsGetSdEnabled())
    {
        if (sdState != SDLOG_STATE_OFF)
        {
            if (sdMounted && sdSerialValid && sdState != SDLOG_STATE_FULL)
            {
                // every record up to now still goes to the card (no lag: no marker can come for them any more)
                uint32_t end = datalogEndSerial(NULL);
                sdlogTakeMarkers(end, &end);
                sdlogConsume(end);
            }
            sdlogCloseFile(); // everything to the card (a failure has already unmounted it)
            sdlogUnmount();   // the card may be taken out now
            sdMountTried = false;
            sdSerialValid = false;
            sdlogSetState(SDLOG_STATE_OFF);
        }
        sdlogStatsUpdate();
        return;
    }

    uint32_t clearGen = 0;
    uint32_t end = datalogEndSerial(&clearGen);
    if (!sdSerialValid)
    {
        // switched on (or at boot): from the records that come now - the log so far is not copied
        sdSerialValid = true;
        sdNextSerial = end;
        sdSyncedSerial = end;
        sdBufEndSerial = end;
        sdClearGen = clearGen;
        sdMarkSeen = datalogMarkCount();
        sdNewFileRequested = true;
        sdLastEnd = end;
        sdLastEndChangeMs = nowMs;
        if (sdState == SDLOG_STATE_OFF)
        {
            sdlogSetState(SDLOG_STATE_NO_CARD); // not shown as a problem before the first mount attempt
        }
    }
    if (clearGen != sdClearGen)
    {
        // the log was cleared: its serials start again at 0 - continue with the new records (same file)
        sdClearGen = clearGen;
        sdNextSerial = 0;
        sdSyncedSerial = 0;
        sdBufEndSerial = 0;
        sdFileSerialsValid = false;
    }
    if (end != sdLastEnd)
    {
        sdLastEnd = end;
        sdLastEndChangeMs = nowMs;
    }

    if (dmmIsOff())
    {
        // the K197 is switched off: everything to the card, the file closed - the next reading opens a new file.
        // Nothing else while it is off (no mount attempts, no probing): the ESP32 may go to light sleep.
        if (sdMounted && sdState != SDLOG_STATE_FULL)
        {
            sdlogTakeMarkers(end, &end);
            if (sdlogConsume(end) && sdlogCloseFile() && sdState == SDLOG_STATE_LOGGING)
            {
                sdlogSetState(SDLOG_STATE_READY);
            }
        }
        sdNewFileRequested = true;
        sdlogStatsUpdate();
        return;
    }

    if (!sdMounted)
    {
        if (!sdMountTried || (uint32_t)(nowMs - sdLastMountTryMs) >= (uint32_t)CONFIG_SD_RETRY_MS)
        {
            sdLastMountTryMs = nowMs;
            sdMountTried = true;
            if (sdlogMount())
            {
                sdNewFileRequested = true;
                sdLastProbeMs = nowMs;
                sdlogSetState(sdlogFreeEnough(0) ? SDLOG_STATE_READY : SDLOG_STATE_FULL);
            }
            else
            {
                sdlogSetState(SDLOG_STATE_NO_CARD);
            }
        }
        if (!sdMounted)
        {
            sdlogStatsUpdate();
            return;
        }
    }

    if (CONFIG_SD_PROBE_MS > 0 && (uint32_t)(nowMs - sdLastProbeMs) >= (uint32_t)CONFIG_SD_PROBE_MS)
    {
        sdLastProbeMs = nowMs;
        if (!sdlogProbe())
        {
            debugLogf("sd: card removed");
            sdlogFail(SDLOG_STATE_NO_CARD);
            sdlogStatsUpdate();
            return;
        }
    }

    if (sdState == SDLOG_STATE_FULL)
    {
        sdlogStatsUpdate();
        return; // nothing more to this card; a removed card is found by the probe
    }

    // up to which record: CONFIG_SD_LAG_RECORDS behind the newest (a marker may still come for those), all of them
    // once the log has been quiet for CONFIG_SD_QUIET_MS, and always up to a marked record
    uint32_t limit = end;
    if ((uint32_t)(nowMs - sdLastEndChangeMs) < (uint32_t)CONFIG_SD_QUIET_MS)
    {
        limit = (end > (uint32_t)CONFIG_SD_LAG_RECORDS) ? end - (uint32_t)CONFIG_SD_LAG_RECORDS : 0;
    }
    bool marker = sdlogTakeMarkers(end, &limit);
    if (!sdlogConsume(limit))
    {
        sdlogStatsUpdate();
        return;
    }
    bool due = (sdBufLen > 0 || sdUnsynced) && (uint32_t)(nowMs - sdUnsyncedSinceMs) >= (uint32_t)CONFIG_SD_FLUSH_MS;
    if (sdFileOpen && (marker || due))
    {
        sdlogWriteOut(true);
    }
    sdlogStatsUpdate();
}

static void sdlogTaskEntry(void *arg)
{
    (void)arg;
    for (;;)
    {
        sdlogStep();
        vTaskDelay(pdMS_TO_TICKS(CONFIG_SD_POLL_MS));
    }
}

// =====================================================================
// Alarm: blink, beep, silencing (esp_timer task)
// =====================================================================

static bool sdlogProblemNow()
{
    uint8_t state = sdState;
    bool bad = (state == SDLOG_STATE_NO_CARD || state == SDLOG_STATE_FULL || state == SDLOG_STATE_ERROR);
    return bad && sdMountTried && settingsGetSdEnabled() && !dmmIsOff();
}

void sdlogTick()
{
    uint32_t nowMs = (uint32_t)millis();
    if (!sdlogProblemNow())
    {
        if (sdAlarmActive)
        {
            sdAlarmActive = false;
            sdSilenced = false;
        }
        if (sdSwallowArmed)
        {
            buttonsArmSwallow(false);
            sdSwallowArmed = false;
        }
        sdBlinkOn = false;
        setSdIndicator(false);
        return;
    }
    if (!sdAlarmActive)
    {
        // a new problem: blinking from now, the first beep at once
        sdAlarmActive = true;
        sdSilenced = false;
        sdBlinkOn = false;
        sdLastBeepMs = nowMs - (uint32_t)CONFIG_SD_ALARM_PERIOD_MS;
        sdSwallowSeen = buttonsSwallowedCount();
    }
    sdBlinkOn = !sdBlinkOn;
    setSdIndicator(sdBlinkOn);
    if (!sdSilenced && buttonsSwallowedCount() != sdSwallowSeen)
    {
        sdSilenced = true; // a key was pressed: quiet until the next problem
        debugLogf("sd: alarm silenced by a key");
    }
    bool sounding = !sdSilenced && settingsGetBeepMode() != SETTINGS_BEEP_OFF;
    if (sounding != sdSwallowArmed)
    {
        buttonsArmSwallow(sounding);
        sdSwallowArmed = sounding;
    }
    if (sounding && (uint32_t)(nowMs - sdLastBeepMs) >= (uint32_t)CONFIG_SD_ALARM_PERIOD_MS)
    {
        sdLastBeepMs = nowMs;
        beeperAlarmAsync(CONFIG_SD_ALARM_BEEP_MS);
    }
}

static void sdlogTickCallback(void *arg)
{
    (void)arg;
    sdlogTick();
}

// =====================================================================
// Public
// =====================================================================

void sdlogBegin()
{
    memset(&sdStats, 0, sizeof(sdStats));
    esp_timer_create_args_t timerArgs = {};
    timerArgs.callback = &sdlogTickCallback;
    timerArgs.arg = NULL;
    timerArgs.dispatch_method = ESP_TIMER_TASK;
    timerArgs.name = "sd_tick";
    if (esp_timer_create(&timerArgs, &sdTickTimer) == 0)
    {
        esp_timer_start_periodic(sdTickTimer, (uint64_t)CONFIG_SD_BLINK_MS * 1000ULL);
    }
    TaskHandle_t handle = NULL;
    xTaskCreatePinnedToCore(sdlogTaskEntry, "sdlog", CONFIG_SD_TASK_STACK_BYTES, NULL, CONFIG_SD_TASK_PRIORITY, &handle, 1);
    sdTaskHandle = handle;
}

uint8_t sdlogState()
{
    return sdState;
}

bool sdlogProblem()
{
    return sdAlarmActive;
}

bool sdlogReadyForSleep()
{
    return !sdFileOpen && sdBufLen == 0 && !sdBusy;
}

void sdlogGetStats(SdlogStats *out)
{
    portENTER_CRITICAL(&sdStatsMux);
    *out = sdStats;
    portEXIT_CRITICAL(&sdStatsMux);
    out->state = sdState;
    out->problem = sdAlarmActive;
    out->silenced = sdSilenced;
    uint32_t end = datalogEndSerial(NULL);
    out->backlog = (sdSerialValid && end > sdNextSerial) ? end - sdNextSerial : 0;
}

TaskHandle_t sdlogGetTask()
{
    return sdTaskHandle;
}
