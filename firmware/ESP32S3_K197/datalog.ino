// datalog.ino
// See datalog.h.

#include "datalog.h"
#include "rtc.h"
#include "reading_math.h"
#include "settings.h"
#include "display.h" // KEITHLEY_BIT_*
#include "debuglog.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// LogRecord itself is defined in datalog.h, not here - see that
// file's comment for why it has to be.

static LogRecord *logBuffer = NULL;
static uint32_t logCapacity = 0; // actual, runtime-determined capacity - see datalogBegin()
static uint32_t writeIndex = 0;
static uint32_t recordCount = 0;
static uint32_t lastRecordedMillis = 0;
// v14: running number of every record ever written since boot or the last
// clear ("serial"), and how often the log has been cleared - for readers that
// follow the log as it grows (the graph, graph.ino), whose 1-based record
// numbers shift by one with every record once the ring buffer is full. The
// record with serial s is the (s - (logWritten - recordCount) + 1)-th stored
// one while it is still stored. Both change only under datalogMutex.
static uint32_t logWritten = 0;
static uint32_t logClearGeneration = 0;

// v18 markers: when the newest records were appended (millis() and serial), so a
// marker can be put on the record that was SHOWN at a given moment (the first
// edge of a key press, an EXTERNAL TRIGGER edge) - and the serials of the last
// markers set, for the graph (which may have read that record already). All
// under datalogMutex.
#define DATALOG_APPEND_HISTORY 8
static uint32_t appendHistMs[DATALOG_APPEND_HISTORY];
static uint32_t appendHistSerial[DATALOG_APPEND_HISTORY];
static uint8_t appendHistNext = 0;
static uint8_t appendHistCount = 0;
static uint32_t markSerials[DATALOG_MARK_RING];
// v20: settling after a change of the function (capture task only)
static const char *settleLastPrefix = NULL;
static uint32_t settleReadings = 0;
// v22: the K197 was off (or the ESP32 just started): the next function's settling is the long one
static bool settlePowerOn = true;
static uint32_t settleTarget = CONFIG_DATALOG_SETTLE_READINGS;
static bool settleLast = false; // the newest reading was settling (alarm.ino)
static uint32_t markCount = 0;

// Guards every read AND write of writeIndex/recordCount/logBuffer[]
// contents - this ring buffer is the one piece of shared state
// genuinely written from core 0 (datalogUpdateReading(), called from
// dmm_bridge.ino's dmmApplyFrame()) and read from core 1
// (datalogPrintRange()/PrintMin()/PrintMax()/PrintAverage()/
// PrintMedian()/CountFiltered(), called from serial_out.ino's LOG:*
// handlers), so - unlike the small SCPI reading cache in
// serial_out.ino, deliberately left unprotected, see the comment
// there - it needs a real mutex.
//
// Design, per the dual-core refactor notes: the WRITER (core 0) takes
// this for only as long as it takes to fill in one record - a few
// struct-field assignments plus the index/count update, nothing that
// waits on anything. The READER (core 1) takes it once per record
// too (see recordAtUserIndex() below), even when a caller like
// datalogPrintRange() is walking a range of thousands of records for
// a single LOG:DATA? reply - NEVER once for the whole reply. That is
// the entire point: a multi-thousand-record dump on core 1 must never
// hold this lock long enough to make core 0 wait on it, since core 0
// blocking would risk missing/corrupting the next DMM frame. At
// ~285 ms between DMM updates and a per-record critical section on
// the order of microseconds, actual contention between the two cores
// is expected to be effectively zero in practice - this pays a small,
// constant locking overhead on every access in exchange for never
// having a large one.
static SemaphoreHandle_t datalogMutex = NULL;

// Sizes the log from however much PSRAM is actually free right now,
// rather than a hardcoded guess at total chip size - this project's
// board has 8 MB of PSRAM and nothing else competing for it (no SD
// card, no WiFi/BT), so as long as datalogBegin() runs early in
// setup() (before anything else could claim any PSRAM), "free right
// now" is effectively "everything the chip actually has, minus
// whatever the IDF/bootloader itself already reserved" - a more
// robust number to size against than a compile-time constant that
// might not match reality on a slightly different board/core version.
//
// ESP.getFreePsram() is the standard Arduino-ESP32-core call for
// this; if a particular core version doesn't have it,
// heap_caps_get_free_size(MALLOC_CAP_SPIRAM) (esp_heap_caps.h) is the
// equivalent lower-level ESP-IDF call.
//
// All of that free space minus DATALOG_PSRAM_RESERVE_BYTES is
// claimed (see datalog.h) - if the first, generous attempt fails
// (PSRAM more fragmented, or smaller, than the free-size query alone
// suggested), this backs off by half and retries down to
// DATALOG_MIN_CAPACITY rather than giving up on logging entirely over
// one failed allocation.
void datalogBegin()
{
    size_t freePsram = ESP.getFreePsram();
    size_t usable = (freePsram > DATALOG_PSRAM_RESERVE_BYTES) ? (freePsram - DATALOG_PSRAM_RESERVE_BYTES) : 0;
    uint32_t capacity = (uint32_t)(usable / sizeof(LogRecord));
#if CONFIG_DATALOG_TEST_CAPACITY > 0
    // v21 TEST: a small log, so that it wraps soon (config.h)
    if (capacity > (uint32_t)CONFIG_DATALOG_TEST_CAPACITY)
    {
        capacity = (uint32_t)CONFIG_DATALOG_TEST_CAPACITY;
    }
#endif

    while (capacity >= DATALOG_MIN_CAPACITY)
    {
        logBuffer = (LogRecord *)ps_malloc(sizeof(LogRecord) * (size_t)capacity);
        if (logBuffer)
            break;
        capacity /= 2;
    }

    if (!logBuffer)
    {
        // A real problem (logging won't work at all) - always worth
        // printing, not gated behind SYSTem:DEBUG the way the
        // routine capacity report below is.
        debugLogAlways("datalogBegin: PSRAM allocation failed - check Tools > PSRAM is enabled. The log will stay empty.");
        logCapacity = 0;
        return;
    }

    logCapacity = capacity;

    // Created only once the buffer itself exists - if allocation
    // failed above, this function already returned and every other
    // datalog function's own `if (!logBuffer) return;`/equivalent
    // guard means datalogMutex is never touched while still NULL.
    datalogMutex = xSemaphoreCreateMutex();

    // Routine startup info, not an error - only worth the clutter
    // when actually debugging (settingsBegin() has already loaded the
    // persisted SYSTem:DEBUG value by the time this runs, see
    // ESP32S3_K197.ino's setup() ordering).
    // v13: through the debug queue (SYSTem:DEBUG:PORT), always - part of the boot report.
    debugLogAlways("boot: log capacity %lu records (%lu KB PSRAM)%s", (unsigned long)logCapacity,
                   (unsigned long)((logCapacity * sizeof(LogRecord)) / 1024),
                   (CONFIG_DATALOG_TEST_CAPACITY > 0) ? " - TEST LIMIT (config.h CONFIG_DATALOG_TEST_CAPACITY)" : "");
}

void datalogNoteDmmOff()
{
    settlePowerOn = true;
    settleLastPrefix = NULL;
}

bool datalogLastReadingSettling()
{
    return settleLast;
}

void datalogUpdateReading(const char digits[KEITHLEY_DIGIT_COUNT], int8_t dpPos,
                           uint16_t indicatorBits, uint8_t unit)
{
    if (!logBuffer)
        return;

    bool overload = keithleyIsOverload(digits);
    double value;
    if (overload)
    {
        value = (indicatorBits & KEITHLEY_BIT_MINUS)
                    ? -KEITHLEY_OVERLOAD_SENTINEL
                    : KEITHLEY_OVERLOAD_SENTINEL;
    }
    else if (!keithleyReadingToDouble(digits, dpPos, indicatorBits, unit, &value))
    {
        settleLastPrefix = NULL; // v20: self-test / no DMM: the readings after it settle again
        settleLast = true;
        return; // not overload, not an ordinary number either (self-test, ...) - don't log it
    }

    // v20: count the DMM readings since the measurement function last changed (every reading, also those
    // SYSTem:AUTO does not log); the first CONFIG_DATALOG_SETTLE_READINGS of them are marked as settling.
    const char *prefix = keithleyFunctionPrefix(unit, indicatorBits);
    if (settleLastPrefix == NULL || strcmp(prefix, settleLastPrefix) != 0)
    {
        settleLastPrefix = prefix;
        settleReadings = 0;
        // v22: the first function after the K197 was switched on settles for much longer
        settleTarget = settlePowerOn ? (uint32_t)CONFIG_DATALOG_SETTLE_POWERON_READINGS : (uint32_t)CONFIG_DATALOG_SETTLE_READINGS;
        settlePowerOn = false;
    }
    bool settling = settleReadings < settleTarget;
    settleLast = settling;
    if (settleReadings < 0xFFFFFFFFu)
        settleReadings++;
    if (settling)
        indicatorBits = (uint16_t)(indicatorBits | DATALOG_BIT_SETTLING);

    uint16_t intervalSeconds = settingsGetAutoIntervalSeconds();
    uint32_t nowMs = millis();
    if (intervalSeconds > 0 && (nowMs - lastRecordedMillis) < (uint32_t)intervalSeconds * 1000UL)
        return; // not due yet - SYSTem:AUTO's interval, regardless of its ON/OFF flag

    // Fetched before the critical section below - it's not part of
    // the mutex-protected state (writeIndex/recordCount/logBuffer[]
    // contents), just a couple of cheap reads of the RTC library's
    // own cache (see rtcCompactTimestamp()'s comment, rtc.h) - no
    // reason to hold the lock any longer than the actual record write
    // needs. Leaves both outputs at the 0/0 "no timestamp" sentinel
    // if the RTC isn't synced (absent, or not yet caught its first
    // second edge this session) - the record still gets written
    // either way, just without a usable timestamp.
    uint32_t timestampSeconds2000;
    uint8_t timestampCentisecond;
    rtcCompactTimestamp(&timestampSeconds2000, &timestampCentisecond);

    // Critical section is just these six field assignments plus the
    // index/count update below - nothing here waits on anything, so
    // the lock is held for a few microseconds at most (see the
    // design comment by datalogMutex's declaration above).
    xSemaphoreTake(datalogMutex, portMAX_DELAY);
    LogRecord &rec = logBuffer[writeIndex];
    rec.timestampSeconds2000 = timestampSeconds2000;
    rec.timestampCentisecond = timestampCentisecond;
    rec.value = (float)value; // KEITHLEY_OVERLOAD_SENTINEL (~1e38) fits float's range fine
    rec.unit = unit;
    rec.indicatorBits = indicatorBits;
    rec.overload = overload;
    writeIndex = (writeIndex + 1) % logCapacity;
    if (recordCount < logCapacity)
        recordCount++;
    appendHistMs[appendHistNext] = nowMs; // v18: this record is what the display shows from now on
    appendHistSerial[appendHistNext] = logWritten;
    appendHistNext = (uint8_t)((appendHistNext + 1) % DATALOG_APPEND_HISTORY);
    if (appendHistCount < DATALOG_APPEND_HISTORY)
        appendHistCount++;
    logWritten++;
    xSemaphoreGive(datalogMutex);
    lastRecordedMillis = nowMs;
}

// A plain, unlocked read of a single uint32_t - atomic on this
// platform (32-bit aligned access), so never torn; at worst
// momentarily stale by one in-flight record if core 0 is mid-write on
// the other core at the exact same instant, same reasoning as the
// SCPI reading cache in serial_out.ino. Not worth locking for that.
uint32_t datalogCount()
{
    return recordCount;
}

uint32_t datalogCapacity()
{
    return logCapacity;
}

void datalogClear()
{
    if (!logBuffer)
        return; // allocation failed at boot - datalogMutex doesn't exist either

    // Same shared state as datalogUpdateReading()'s write path
    // (writeIndex/recordCount), so guarded the same way - LOG:CLEar
    // (serial_out.ino, core 1) must not race a concurrent write from
    // core 0.
    xSemaphoreTake(datalogMutex, portMAX_DELAY);
    recordCount = 0;
    writeIndex = 0;
    logWritten = 0;
    logClearGeneration++;
    appendHistCount = 0; // v18: those serials are gone
    xSemaphoreGive(datalogMutex);
}

// Logical index i (0 = oldest of the currently-stored records) ->
// actual position in logBuffer[].
static uint32_t recordPos(uint32_t i)
{
    uint32_t oldestPos = (writeIndex + logCapacity - recordCount) % logCapacity;
    return (oldestPos + i) % logCapacity;
}

// Resolves the public 1-based, 0-means-"open end" numbering (see
// datalog.h) into a concrete, clamped [first, last] pair, both
// 1-based and within [1, recordCount]. Returns false (nothing to do)
// if the log is empty or the resolved range is empty.
// Resolves the public 1-based, 0-means-"open end" numbering (see
// datalog.h) into a concrete, clamped [first, last] pair, both
// 1-based and within [1, recordCount]. Returns false (nothing to do)
// if the log is empty or the resolved range is empty.
//
// firstUser < 0 is a third mode on top of the usual "explicit index"/
// "0 = open end": Unix `tail`-style - "the |firstUser| records
// immediately before lastUser". lastUser <= 0 there means "one past
// the newest", i.e. plain "last N records" ("LOG:DATA? -50" alone);
// an explicit lastUser instead means "N records before that index"
// ("LOG:DATA? -50 200" -> records 150..199) - the author's own
// request, for paging backward through history the way `tail -50`
// or `tail -n 50 file | head` would. "LOG:DATA? -50" and
// "LOG:DATA? -50 0" are exactly equivalent (both resolve to the same
// "anchor = one past the newest" case) - either syntax works.
static bool resolveRange(long firstUser, long lastUser, long *outFirst, long *outLast)
{
    long total = (long)recordCount;
    if (total == 0)
        return false;

    long first, last;
    if (firstUser < 0)
    {
        long count = -firstUser;
        long anchor = (lastUser <= 0) ? (total + 1) : lastUser;
        last = anchor - 1;
        first = last - count + 1;
    }
    else
    {
        first = (firstUser <= 0) ? 1 : firstUser;
        last = (lastUser <= 0) ? total : lastUser;
    }

    if (first < 1)
        first = 1;
    if (first > total)
        first = total;
    if (last < 1)
        last = 1;
    if (last > total)
        last = total;
    if (first > last)
        return false;

    *outFirst = first;
    *outLast = last;
    return true;
}

// Returns a COPY of the record, not a reference into logBuffer[] -
// deliberately, so the lock only needs to be held for the copy
// itself (a few dozen bytes), not for however long the caller then
// spends formatting/comparing/accumulating it. Every one of this
// function's five callers below already loops one record at a time
// (datalogPrintRange() etc.), so this gives the exact "lock per
// record, never for the whole range" behavior the dual-core split
// needs - see datalogMutex's own comment above for why that matters.
// Callers binding the result to `const LogRecord &rec = ...` (as they
// all do) still work unchanged: binding a const reference to a
// temporary extends the temporary's lifetime to the reference's own
// scope, so nothing about the five call sites needed to change.
static LogRecord recordAtUserIndex(long userIndex)
{
    xSemaphoreTake(datalogMutex, portMAX_DELAY);
    LogRecord rec = logBuffer[recordPos((uint32_t)(userIndex - 1))];
    xSemaphoreGive(datalogMutex);
    return rec;
}

// True if functionFilter is empty (no filter requested) or matches
// (case-insensitive, trimmed) the function keithleyFunctionPrefix()
// derives for this record's unit + AC indicator - e.g. "DCV", "OHM".
// Callers (serial_out.ino's parseLogArgs()) already uppercase the
// filter before passing it in, so this only needs to normalize the
// record's own prefix the same way before comparing - handles
// keithleyFunctionPrefix()'s " dB" (leading space, for column
// alignment elsewhere) matching a plain "DB" filter correctly.
static bool functionMatches(const char *functionFilter, uint8_t unit, uint16_t indicatorBits)
{
    if (functionFilter[0] == '\0')
        return true;

    // Compares the function's prefix ("DCV", " dB", ...) with the filter
    // ignoring blanks and case, character by character. This runs once per
    // record, so it must not allocate - a String per record (what the
    // earlier version did) costs more than everything else a statistics
    // pass does for that record.
    const char *prefix = keithleyFunctionPrefix(unit, indicatorBits);
    const char *filter = functionFilter;
    for (;;)
    {
        while (*prefix == ' ')
            prefix++;
        while (*filter == ' ')
            filter++;
        if (*prefix == '\0' || *filter == '\0')
            return *prefix == '\0' && *filter == '\0';
        char a = *prefix;
        char b = *filter;
        if (a >= 'a' && a <= 'z')
            a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z')
            b = (char)(b - 'a' + 'A');
        if (a != b)
            return false;
        prefix++;
        filter++;
    }
}

// AUTO/STO/RCL/REM/BAT - whichever of these are set, space-separated.
// AC/REL/MINUS are deliberately not repeated here; they're already
// reflected in the status character and the value's own sign.
// Space-separated WITHIN this one CSV field (not further split into
// separate columns) - no comma appears in any indicator name, so it
// can't be confused with the field separator around it.
static void printExtraIndicatorsCsv(Stream &interface, uint16_t indicatorBits)
{
    static const struct
    {
        const char *name;
        uint16_t bit;
    } kExtra[] = {
        {"AUTO", KEITHLEY_BIT_AUT},
        {"STO",  KEITHLEY_BIT_STO},
        {"RCL",  KEITHLEY_BIT_RCL},
        {"REM",  KEITHLEY_BIT_REM},
        {"BAT",  KEITHLEY_BIT_BAT},
        {"TAG",  DATALOG_BIT_MARKER}, // v18: a marker
        {"SETTLE", DATALOG_BIT_SETTLING}, // v20: first readings after a change of the function
    };
    bool first = true;
    for (uint8_t i = 0; i < sizeof(kExtra) / sizeof(kExtra[0]); i++)
    {
        if (indicatorBits & kExtra[i].bit)
        {
            if (!first)
                interface.print(' ');
            interface.print(kExtra[i].name);
            first = false;
        }
    }
}

uint32_t datalogCountFiltered(const char *functionFilter, long first, long last)
{
    if (!resolveRange(first, last, &first, &last))
        return 0;

    uint32_t matched = 0;
    for (long userIndex = first; userIndex <= last; userIndex++)
    {
        const LogRecord &rec = recordAtUserIndex(userIndex);
        if (functionMatches(functionFilter, rec.unit, rec.indicatorBits))
            matched++;
    }
    return matched;
}

// CSV, one header row then one row per matching record: record
// number, timestamp, status, function, value, indicators. Record
// numbers are always the position in the WHOLE log - functionFilter
// only skips non-matching rows, it never renumbers the ones that do
// match, so gaps in the sequence (e.g. "3", "7", "12", ...) are
// expected and correct once a filter is in use. The value column
// always gets a real '+'/'-' sign (acMagnitude=false, unlike the
// AC-gets-a-blank-space-instead-of-'+' convention
// keithleyPrintScientific() otherwise uses for a live single-string
// reading) - column alignment doesn't matter once it's comma-
// separated, and a plain numeric string is easier for a CSV/
// spreadsheet consumer to parse than one with a leading space
// standing in for a sign. Status/function are split into their own
// columns rather than glued together like a live reading's
// "<status><prefix>" - more useful once this is meant to become a
// CSV a person can filter/sort by, e.g., function.
// v18: markedOnly = only the records with a marker (LOG:MARKers?).
static void datalogPrintRows(Stream &interface, const char *functionFilter, long first, long last, bool markedOnly)
{
    if (!resolveRange(first, last, &first, &last))
        return;

    interface.println(F("record,timestamp,status,function,value,indicators"));

    for (long userIndex = first; userIndex <= last; userIndex++)
    {
        const LogRecord &rec = recordAtUserIndex(userIndex);
        if (markedOnly && (rec.indicatorBits & DATALOG_BIT_MARKER) == 0)
            continue;
        if (!functionMatches(functionFilter, rec.unit, rec.indicatorBits))
            continue;

        char status = rec.overload ? 'O' : (rec.indicatorBits & KEITHLEY_BIT_REL) ? 'Z' : 'N';

        char timestampBuf[23]; // "YYYY-MM-DD HH:MM:SS.cc" + terminator - see rtcFormatCompactTimestamp() (rtc.h)
        rtcFormatCompactTimestamp(rec.timestampSeconds2000, rec.timestampCentisecond,
                                   timestampBuf, sizeof(timestampBuf));

        interface.print(userIndex);
        interface.print(',');
        interface.print(timestampBuf);
        interface.print(',');
        interface.print(status);
        interface.print(',');
        interface.print(keithleyFunctionPrefix(rec.unit, rec.indicatorBits));
        interface.print(',');
        keithleyPrintScientific(interface, rec.value, false);
        interface.print(',');
        printExtraIndicatorsCsv(interface, rec.indicatorBits);
        interface.println();
    }
}

void datalogPrintRange(Stream &interface, const char *functionFilter, long first, long last)
{
    datalogPrintRows(interface, functionFilter, first, last, false);
}

void datalogPrintMarked(Stream &interface, const char *functionFilter, long first, long last)
{
    datalogPrintRows(interface, functionFilter, first, last, true);
}

void datalogPrintMin(Stream &interface, const char *functionFilter, long first, long last)
{
    if (!resolveRange(first, last, &first, &last))
        return;

    bool found = false;
    double best = 0;
    for (long userIndex = first; userIndex <= last; userIndex++)
    {
        const LogRecord &rec = recordAtUserIndex(userIndex);
        if (datalogRecordSkipped(rec)) // v20: OL or settling
            continue;
        if (!functionMatches(functionFilter, rec.unit, rec.indicatorBits))
            continue;
        if (!found || rec.value < best)
        {
            best = rec.value;
            found = true;
        }
    }
    if (!found)
        return; // nothing in range matched (or every match was an overload)

    keithleyPrintScientific(interface, best, false);
    interface.println();
}

void datalogPrintMax(Stream &interface, const char *functionFilter, long first, long last)
{
    if (!resolveRange(first, last, &first, &last))
        return;

    bool found = false;
    double best = 0;
    for (long userIndex = first; userIndex <= last; userIndex++)
    {
        const LogRecord &rec = recordAtUserIndex(userIndex);
        if (datalogRecordSkipped(rec)) // v20: OL or settling
            continue;
        if (!functionMatches(functionFilter, rec.unit, rec.indicatorBits))
            continue;
        if (!found || rec.value > best)
        {
            best = rec.value;
            found = true;
        }
    }
    if (!found)
        return;

    keithleyPrintScientific(interface, best, false);
    interface.println();
}

void datalogPrintAverage(Stream &interface, const char *functionFilter, long first, long last)
{
    if (!resolveRange(first, last, &first, &last))
        return;

    double sum = 0;
    long n = 0;
    for (long userIndex = first; userIndex <= last; userIndex++)
    {
        const LogRecord &rec = recordAtUserIndex(userIndex);
        if (datalogRecordSkipped(rec)) // v20: OL or settling
            continue;
        if (!functionMatches(functionFilter, rec.unit, rec.indicatorBits))
            continue;
        sum += rec.value;
        n++;
    }
    if (n == 0)
        return;

    keithleyPrintScientific(interface, sum / (double)n, false);
    interface.println();
}

static int compareFloat(const void *a, const void *b)
{
    float fa = *(const float *)a;
    float fb = *(const float *)b;
    if (fa < fb)
        return -1;
    if (fa > fb)
        return 1;
    return 0;
}

void datalogPrintMedian(Stream &interface, const char *functionFilter, long first, long last)
{
    if (!resolveRange(first, last, &first, &last))
        return;

    long rangeLen = last - first + 1;

    // A range bigger than DATALOG_MEDIAN_SAMPLE_CAP is sampled every
    // `stride`-th record instead of computed exactly - the author's
    // own idea, on the reasoning that a median is inherently about
    // the "typical" value rather than one exact reading: a short-
    // lived spike wouldn't have swung an EXACT median much either
    // (it's the middle of a sorted list, not sensitive to a single
    // outlier the way an average is), and a longer-lived one still
    // shows up across several sampled points regardless of stride.
    // stride stays 1 (no sampling at all, exactly the old exact
    // behavior) whenever the range already fits the cap - ordinary,
    // smaller queries are completely unaffected.
    long stride = 1;
    if (rangeLen > (long)DATALOG_MEDIAN_SAMPLE_CAP)
        stride = (rangeLen + (long)DATALOG_MEDIAN_SAMPLE_CAP - 1) / (long)DATALOG_MEDIAN_SAMPLE_CAP;
    long maxSamples = (rangeLen + stride - 1) / stride; // <= DATALOG_MEDIAN_SAMPLE_CAP by construction

    // Allocated from PSRAM, not the regular heap - even capped this
    // way, it could be a few hundred KB, more than the ESP32's much
    // smaller internal SRAM heap could reliably spare alongside
    // everything else already using it. Sized to maxSamples even
    // though a function filter may mean fewer than that many actually
    // get used (n below tracks how many were actually written in) -
    // simpler than a two-pass count-then-allocate, and the extra
    // unused tail of the buffer costs nothing but a slightly larger
    // allocation.
    float *values = (float *)ps_malloc(sizeof(float) * (size_t)maxSamples);
    // v16: if the PSRAM left beside the log has no block that large (the
    // reserve is now exact, and other code may hold some of it), sample
    // more coarsely instead of giving up: twice the stride, half the
    // buffer, down to a floor of 1000 samples.
    while (!values && maxSamples > 1000)
    {
        stride *= 2;
        maxSamples = (rangeLen + stride - 1) / stride;
        values = (float *)ps_malloc(sizeof(float) * (size_t)maxSamples);
    }
    if (!values)
        return; // not even the floor could be allocated - shouldn't happen

    long n = 0;
    for (long userIndex = first; userIndex <= last; userIndex += stride)
    {
        const LogRecord &rec = recordAtUserIndex(userIndex);
        if (datalogRecordSkipped(rec)) // v20: OL or settling
            continue;
        if (!functionMatches(functionFilter, rec.unit, rec.indicatorBits))
            continue;
        values[n++] = rec.value;
    }
    if (n == 0)
    {
        free(values);
        return;
    }

    qsort(values, n, sizeof(float), compareFloat);
    float median = (n % 2 == 1) ? values[n / 2]
                                 : (values[n / 2 - 1] + values[n / 2]) / 2.0f;
    free(values);

    keithleyPrintScientific(interface, median, false);
    interface.println();
}

// The values the median is taken from (see DATALOG_STATS_MEDIAN_SAMPLES in
// datalog.h): a static buffer in internal RAM, 16 kB, shared by whichever
// statistics job is running (one at a time). Sorting in internal RAM is
// several times faster than in PSRAM, and the buffer never has to be
// allocated or freed.
static float datalogStatsScratch[DATALOG_STATS_MEDIAN_SAMPLES];

// Copies `count` consecutive records, the first being the 1-based user
// index `first`, into dst under ONE hold of the log mutex (the writer on
// core 0 waits for at most this copy, a few microseconds for the 64
// records the statistics job asks for). Taking the mutex once per record
// instead, as recordAtUserIndex() does, costs about as much as reading
// the record.
static void copyRecords(long first, uint32_t count, LogRecord *dst)
{
    xSemaphoreTake(datalogMutex, portMAX_DELAY);
    for (uint32_t i = 0; i < count; i++)
        dst[i] = logBuffer[recordPos((uint32_t)(first - 1) + i)];
    xSemaphoreGive(datalogMutex);
}

void datalogStatsStart(DatalogStatsJob *job, const char *functionFilter, long first, long last)
{
    memset(job, 0, sizeof(*job));
    snprintf(job->filter, sizeof(job->filter), "%s", functionFilter);

    if (!logBuffer)
        return;
    // A first record beyond the end is an EMPTY range here (what datalogFirstRecordSince() returns when no
    // record is within the window) - unlike the SCPI numbering, where resolveRange() clamps it to the last record.
    if (first > 0 && first > (long)recordCount)
        return;
    if (!resolveRange(first, last, &first, &last))
        return;

    long rangeLen = last - first + 1;
    long stride = 1;
    if (rangeLen > (long)DATALOG_STATS_MEDIAN_SAMPLES)
        stride = (rangeLen + (long)DATALOG_STATS_MEDIAN_SAMPLES - 1) / (long)DATALOG_STATS_MEDIAN_SAMPLES;

    job->samples = datalogStatsScratch;
    job->active = true;
    job->next = first;
    job->strideOrigin = first;
    job->last = last;
    job->stride = stride;
}

bool datalogStatsStep(DatalogStatsJob *job, uint32_t maxRecords)
{
    if (!job->active)
        return true;

    // The log may have been cleared meanwhile: never read past what is stored.
    if (job->last > (long)recordCount)
        job->last = (long)recordCount;

    long stop = job->next + (long)maxRecords - 1;
    if (maxRecords == 0xFFFFFFFFUL || stop > job->last || stop < job->next)
        stop = job->last;

    // Records are fetched in chunks of up to 64 (one mutex hold each).
    const uint32_t chunk = 64;
    LogRecord buffer[chunk];
    while (job->next <= stop)
    {
        uint32_t n = (uint32_t)(stop - job->next + 1);
        if (n > chunk)
            n = chunk;
        copyRecords(job->next, n, buffer);

        for (uint32_t i = 0; i < n; i++)
        {
            const LogRecord &rec = buffer[i];
            if (datalogRecordSkipped(rec)) // v20: OL or settling
                continue;
            if (!functionMatches(job->filter, rec.unit, rec.indicatorBits))
                continue;

            double v = rec.value;
            if (job->count == 0 || v < job->minimum)
                job->minimum = v;
            if (job->count == 0 || v > job->maximum)
                job->maximum = v;
            job->sum += v;
            job->count++;

            if (((job->next + (long)i - job->strideOrigin) % job->stride) == 0 && job->sampleCount < (long)DATALOG_STATS_MEDIAN_SAMPLES)
                job->samples[job->sampleCount++] = rec.value;
        }
        job->next += (long)n;
    }
    return job->next > job->last;
}

bool datalogStatsFinish(DatalogStatsJob *job, DatalogStats *out)
{
    out->count = 0;
    out->minimum = 0;
    out->maximum = 0;
    out->average = 0;
    out->median = 0;
    out->medianValid = false;

    bool found = (job->count > 0);
    if (found)
    {
        out->count = job->count;
        out->minimum = job->minimum;
        out->maximum = job->maximum;
        out->average = job->sum / (double)job->count;

        if (job->samples != NULL && job->sampleCount > 0)
        {
            qsort(job->samples, job->sampleCount, sizeof(float), compareFloat);
            long n = job->sampleCount;
            out->median = (n % 2 == 1) ? job->samples[n / 2]
                                        : ((double)job->samples[n / 2 - 1] + (double)job->samples[n / 2]) / 2.0;
            out->medianValid = true;
        }
    }

    datalogStatsAbort(job);
    return found;
}

void datalogStatsAbort(DatalogStatsJob *job)
{
    job->samples = NULL;
    job->active = false;
}

bool datalogComputeStats(const char *functionFilter, long first, long last, DatalogStats *out)
{
    DatalogStatsJob job;
    datalogStatsStart(&job, functionFilter, first, last);
    datalogStatsStep(&job, 0xFFFFFFFFUL);
    return datalogStatsFinish(&job, out);
}

long datalogFirstRecordSince(uint32_t windowSeconds, bool *usedEstimate)
{
    if (usedEstimate != NULL)
        *usedEstimate = false;
    if (!logBuffer || recordCount == 0)
        return 0;
    if (windowSeconds == 0)
        return 1;

    long total = (long)recordCount;

    uint32_t nowSeconds = 0;
    uint8_t nowCentis = 0;
    bool haveTime = rtcCompactTimestamp(&nowSeconds, &nowCentis) && nowSeconds > windowSeconds;
    if (haveTime && recordAtUserIndex(total).timestampSeconds2000 != 0)
    {
        // Timestamps never go backwards except for the leading run of
        // zeros (records made before the RTC synced), which count as
        // "older than any window" - so "timestamp >= cutoff" is false
        // for the first part of the log and true for the rest, which a
        // binary search finds in about log2(count) reads.
        uint32_t cutoff = nowSeconds - windowSeconds;
        long lo = 1;
        long hi = total + 1;
        while (lo < hi)
        {
            long mid = lo + (hi - lo) / 2;
            if (recordAtUserIndex(mid).timestampSeconds2000 >= cutoff)
                hi = mid;
            else
                lo = mid + 1;
        }
        return lo; // total + 1 when not even the newest record is that recent: an empty range (the UI then shows dashes)
    }

    // No usable time: estimate from the recording rate.
    if (usedEstimate != NULL)
        *usedEstimate = true;
    uint16_t intervalSeconds = settingsGetAutoIntervalSeconds();
    double perSecond = (intervalSeconds > 0) ? (1.0 / (double)intervalSeconds) : (double)CONFIG_UI_FALLBACK_RECORDS_PER_SECOND;
    double wanted = (double)windowSeconds * perSecond + 0.5;
    if (wanted >= (double)total)
        return 1;
    long n = (long)wanted;
    if (n < 1)
        n = 1;
    return total - n + 1;
}

// ---------------------------------------------------------------
// v14: reading the log by serial number (graph.ino)
// ---------------------------------------------------------------

uint32_t datalogReadSerial(uint32_t *serial, LogRecord *dst, uint32_t maxCount, uint32_t *clearGeneration)
{
    if (!logBuffer)
    {
        if (clearGeneration != NULL)
            *clearGeneration = 0;
        return 0;
    }
    xSemaphoreTake(datalogMutex, portMAX_DELAY);
    if (clearGeneration != NULL)
        *clearGeneration = logClearGeneration;
    uint32_t oldestSerial = logWritten - recordCount;
    if (*serial < oldestSerial || *serial > logWritten)
        *serial = oldestSerial; // overwritten meanwhile (or from before a clear): continue with the oldest stored one
    uint32_t available = logWritten - *serial;
    uint32_t n = (available < maxCount) ? available : maxCount;
    uint32_t first = *serial - oldestSerial; // logical index, 0 = oldest
    for (uint32_t i = 0; i < n; i++)
        dst[i] = logBuffer[recordPos(first + i)];
    xSemaphoreGive(datalogMutex);
    return n;
}

uint32_t datalogEndSerial(uint32_t *clearGeneration)
{
    if (!logBuffer)
    {
        if (clearGeneration != NULL)
            *clearGeneration = 0;
        return 0;
    }
    xSemaphoreTake(datalogMutex, portMAX_DELAY);
    uint32_t end = logWritten;
    if (clearGeneration != NULL)
        *clearGeneration = logClearGeneration;
    xSemaphoreGive(datalogMutex);
    return end;
}

uint32_t datalogFirstSerialAtOrAfter(uint32_t seconds2000)
{
    if (!logBuffer)
        return 0;
    // The serial of the oldest stored record and the count, taken together.
    xSemaphoreTake(datalogMutex, portMAX_DELAY);
    uint32_t oldestSerial = logWritten - recordCount;
    long total = (long)recordCount;
    xSemaphoreGive(datalogMutex);
    if (total == 0)
        return oldestSerial;

    // Same binary search as datalogFirstRecordSince(): timestamps never go
    // backwards except for the leading run of zeros (records made before the
    // RTC synced), which count as older than anything. If the writer drops the
    // oldest record meanwhile, the answer is off by that one record at most.
    long lo = 1;
    long hi = total + 1;
    while (lo < hi)
    {
        long mid = lo + (hi - lo) / 2;
        if (recordAtUserIndex(mid).timestampSeconds2000 >= seconds2000)
            hi = mid;
        else
            lo = mid + 1;
    }
    return oldestSerial + (uint32_t)(lo - 1);
}

bool datalogRecordMatchesFunction(const char *functionFilter, const LogRecord *record)
{
    return functionMatches(functionFilter, record->unit, record->indicatorBits);
}

// ---------------------------------------------------------------
// v18: markers
// ---------------------------------------------------------------

bool datalogMarkAtMs(uint32_t atMs, uint32_t *markedSerial)
{
    if (!logBuffer)
        return false;
    bool ok = false;
    xSemaphoreTake(datalogMutex, portMAX_DELAY);
    if (appendHistCount > 0 && recordCount > 0)
    {
        // newest first: the first record appended at or before atMs; older than the whole history -> the oldest one
        uint32_t serial = 0;
        bool found = false;
        for (uint8_t k = 0; k < appendHistCount; k++)
        {
            uint8_t i = (uint8_t)((appendHistNext + DATALOG_APPEND_HISTORY - 1 - k) % DATALOG_APPEND_HISTORY);
            serial = appendHistSerial[i];
            if ((int32_t)(atMs - appendHistMs[i]) >= 0)
            {
                found = true;
                break;
            }
        }
        (void)found; // not found: `serial` is the oldest entry of the history
        uint32_t oldestSerial = logWritten - recordCount;
        if (serial >= oldestSerial && serial < logWritten)
        {
            logBuffer[recordPos(serial - oldestSerial)].indicatorBits |= DATALOG_BIT_MARKER;
            markSerials[markCount % DATALOG_MARK_RING] = serial;
            markCount++;
            if (markedSerial != NULL)
                *markedSerial = serial;
            ok = true;
        }
    }
    xSemaphoreGive(datalogMutex);
    return ok;
}

uint32_t datalogMarkCount()
{
    return markCount; // a plain aligned 32-bit read
}

bool datalogMarkSerialAt(uint32_t index, uint32_t *serial)
{
    if (!logBuffer)
        return false;
    bool ok = false;
    xSemaphoreTake(datalogMutex, portMAX_DELAY);
    if (index < markCount && markCount - index <= DATALOG_MARK_RING)
    {
        *serial = markSerials[index % DATALOG_MARK_RING];
        ok = true;
    }
    xSemaphoreGive(datalogMutex);
    return ok;
}
