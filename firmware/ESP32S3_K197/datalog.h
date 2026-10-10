// datalog.h
// A ring buffer of past readings, stored in the ESP32-S3's external
// PSRAM. Capacity is determined AT RUNTIME (datalogBegin(), called
// from setup()) from however much PSRAM is actually free at that
// point, not a hardcoded guess - see datalogBegin()'s own comment in
// datalog.ino for exactly how. This project's board has 8 MB of
// PSRAM and no other subsystem competing for it (no SD card, no
// WiFi/BT), so nearly all of it goes to this log, aiming for as many
// days of full-speed logging as the physical PSRAM allows.
//
// Recording happens automatically (datalogUpdateReading(), called
// from dmm_bridge.ino's dmmApplyFrame() the same way
// serialOutUpdateReading() is) at whatever cadence SYSTem:AUTO's
// *interval* is currently set to - but, unlike SYSTem:AUTO's own
// unsolicited sending, logging ignores SYSTem:AUTO's ON/OFF enable
// flag entirely and always records (interval 0 = every measurement,
// same as if SYSTem:AUTO were "0" - this applies even while
// SYSTem:AUTO itself is OFF; SYSTem:AUTO's own interval is also the
// practical way to trade logging resolution for a longer time span
// covered, on top of just maximizing PSRAM capacity here). Retrieval
// and statistics are exposed as SCPI LOG:* commands, registered and
// handled in serial_out.ino - this header is what those handlers call
// into.
//
// Each record stores a timestamp, the value (in base SI units, same
// as keithleyReadingToDouble() - see reading_math.h), the unit, the
// full indicator bits, and whether it was an overload ("OL") reading
// - unit+indicators together are what "including units, indicators,
// range, etc." (the author's own words) ends up meaning here: there's
// no separate "measurement range" field, because the only range
// information available from the K197's own display data IS the unit
// it chose to show (e.g. mV vs V range) - already captured by `unit`.
//
// The record is deliberately compact - see LogRecord below - since
// record size directly trades off against how many days of history
// fit in a fixed amount of PSRAM: `value` is a `float`, not a
// `double`. The K197's display only ever shows about 6 significant
// digits, well within a float's ~7 - so this loses no precision
// that was ever actually there, it just stops keeping 8 bytes of
// storage for a number that only ever needed 4.
//
// Timestamps are wall-clock time from the DS3231 RTC (rtc.h,
// centisecond resolution via the third-party DS3231Centisecond
// library), NOT milliseconds-since-boot the way earlier snapshots of
// this project recorded them - see LogRecord's own comment for the
// packed on-disk (well, on-PSRAM) representation. A record made
// before the RTC has synced this session (or with no RTC present at
// all) stores the reserved 0 sentinel instead - see
// rtcCompactTimestamp()/rtcFormatCompactTimestamp() (rtc.h) for
// exactly what that means and how it's displayed - rather than a
// plausible-looking but wrong wall-clock value.
//
// IMPORTANT: requires PSRAM enabled in the Arduino IDE
// (Tools > PSRAM > ..., board-dependent) - datalogBegin() prints a
// warning to Serial and the log silently stays empty if allocation
// fails entirely, rather than crashing.

#ifndef DATALOG_H
#define DATALOG_H

#include <Arduino.h>
#include "keithley_shapes.h"

// One stored measurement - 13 bytes packed (4+1+4+2+1+1; NOT the 16
// bytes an unpacked layout would round up to, since the tail after
// `value` - indicatorBits/unit/overload/timestampCentisecond, 5 bytes
// - isn't itself a multiple of 4, unlike the previous millis()-only
// version of this struct, which was carefully ordered to avoid ever
// needing __attribute__((packed)) at all. Swapping in a centisecond-
// resolution RTC timestamp cost this record exactly ONE extra byte
// over that original 12 (a 4-byte seconds-since-2000 value plus a new
// 1-byte centisecond field, replacing the old plain 4-byte millis()
// value) - packed is what keeps it at that real cost instead of the
// compiler silently padding it out to 16. See rtcCompactTimestamp()'s
// own comment (rtc.h) for the exact meaning/range of
// timestampSeconds2000 and timestampCentisecond, including the
// reserved 0 sentinel for "no RTC timestamp available when this was
// recorded".
//
// Defined here (not privately in datalog.ino) specifically so it's
// visible from the very top of the merged sketch, the same fix
// already needed once before for SCPI_C/SCPI_P (see
// ESP32S3_K197.ino's Vrekrer_scpi_parser.h include comment):
// Arduino's auto-generated function-prototype pass inserts every
// prototype it finds near the top of the whole concatenated sketch,
// including for static/internal helpers like recordAtUserIndex()
// (datalog.ino) that return "const LogRecord&" - if LogRecord were
// only defined further down inside datalog.ino itself, that
// auto-generated prototype would come before the type exists
// anywhere in the merged file and the build would fail with
// "'LogRecord' does not name a type", regardless of the struct being
// defined correctly later in the same file.
struct __attribute__((packed)) LogRecord
{
    uint32_t timestampSeconds2000; // seconds since 2000-01-01 00:00:00 - see rtc.h; 0 = no RTC timestamp available
    uint8_t timestampCentisecond;  // 0-99; meaningless when timestampSeconds2000 == 0
    float value;
    uint16_t indicatorBits;
    uint8_t unit;
    bool overload;
};

// Floor for datalogBegin()'s allocation retry loop (see datalog.ino)
// - if even this many records' worth of PSRAM can't be allocated,
// logging is given up on entirely for this boot rather than retrying
// forever. 1,000 records is a trivially small ask (12 KB) next to
// what this is aiming for, so hitting this floor means something is
// genuinely wrong (PSRAM not enabled, or far smaller than expected),
// not just "the log is a bit smaller than hoped for".
#ifndef DATALOG_MIN_CAPACITY
#define DATALOG_MIN_CAPACITY 1000UL
#endif

// How much of the PSRAM that's free at datalogBegin() time (i.e.
// essentially all of it, called this early in setup()) is left OUT of
// the log buffer. v16: an absolute amount instead of the earlier 10 %
// (~780 kB, of which SYSTem:DIAGnostic? showed nothing ever used more
// than LOG:MEDian?'s scratch): exactly the largest scratch buffer
// LOG:MEDian? can ask for (DATALOG_MEDIAN_SAMPLE_CAP floats, 400 kB) plus
// DATALOG_PSRAM_SPARE_BYTES for anything else (large mallocs the core
// may place in PSRAM, a future SD card's buffers). Gains ~29k records,
// about 2.4 h of logging (measured: 609273 records, 51.8 h). Measured on
// the instrument: something else takes ~78 kB of PSRAM after
// datalogBegin(), so the largest free block (~385 kB) can be a little
// smaller than the full median buffer - datalogPrintMedian() then samples
// a little more coarsely instead of failing.
#ifndef DATALOG_PSRAM_SPARE_BYTES
#define DATALOG_PSRAM_SPARE_BYTES (64UL * 1024UL)
#endif
#define DATALOG_PSRAM_RESERVE_BYTES (DATALOG_MEDIAN_SAMPLE_CAP * sizeof(float) + DATALOG_PSRAM_SPARE_BYTES)

// LOG:MEDian?'s scratch buffer (a sortable copy of the matching
// values, see datalog.ino) holds at most this many floats, REGARDLESS
// of how large the log itself is or how large a range was requested -
// deliberately decoupled from the log's own (now dynamic, PSRAM-
// maximizing) capacity, so a huge log doesn't also require a huge
// second allocation just to ask for a median over most of it. A range
// bigger than this is not declined - it's sampled every Nth record
// instead (see datalogPrintMedian()'s own comment for the reasoning),
// trading exact precision for being able to answer at all over an
// arbitrarily large span; a range that already fits within this cap
// is still computed exactly, unaffected. 100,000 records (400 KB of
// scratch) keeps that worst-case allocation small and predictable no
// matter how large the log grows.
#ifndef DATALOG_MEDIAN_SAMPLE_CAP
#define DATALOG_MEDIAN_SAMPLE_CAP 100000UL
#endif

// Call once from setup(), after settingsBegin() (each recorded
// reading reads the interval setting, though not at startup itself) -
// and as early as reasonably possible otherwise, since it sizes the
// log buffer from however much PSRAM is free AT THAT MOMENT (see
// datalog.ino).
void datalogBegin();

// Called once per decoded DMM frame (dmm_bridge.ino) - the log
// decides for itself whether this particular call is "due" to be
// recorded, based on SYSTem:AUTO's interval (ignoring its ON/OFF
// flag - see the file comment above). Records an "OL" (overload)
// reading too (using KEITHLEY_OVERLOAD_SENTINEL as its value, same as
// SYSTem:READing 1/2 would show live) - only content that isn't an
// ordinary measurement at all (dpPos < 0, e.g. self-test) is skipped.
void datalogUpdateReading(const char digits[KEITHLEY_DIGIT_COUNT], int8_t dpPos,
                           uint16_t indicatorBits, uint8_t unit);

// Number of records currently stored (<= datalogCapacity()).
uint32_t datalogCount();

// Total capacity of the ring buffer, in records - determined at
// datalogBegin() time from actually-free PSRAM (see above), so this
// is the only reliable way to know it; there's no compile-time
// constant for it anymore. 0 if PSRAM allocation failed entirely.
uint32_t datalogCapacity();

// Empties the log (does not free/reallocate the PSRAM buffer).
void datalogClear();

// Every LOG:* range command shares this numbering: records are
// numbered from 1 (oldest) to datalogCount() (newest) - there is no
// record #0. `first`/`last` of 0 (or anything <= 0, for `first`
// specifically only when it's not the tail mode below) is a sentinel
// meaning "no bound on this side" - 0 as `first` means "start from
// the oldest", 0 as `last` means "go to the newest". Any value beyond
// the currently stored count is clamped into range rather than
// treated as an error ("ohlidat preteceni" - watch out for overflow -
// per the author's own requirement); first > last after resolving
// both ends prints/computes nothing.
//
// A NEGATIVE `first` is a third mode, Unix `tail`-style: `-N` means
// "the N records immediately before `last`" - `last <= 0` there means
// "one past the newest", so `-50` alone (`last` omitted or 0) is
// plain "the last 50 records", while an explicit `last` means "N
// records before that index" instead, for paging backward through
// history - e.g. `LOG:DATA? -50 200` gives records 150..199. Both
// `LOG:DATA? -50` and `LOG:DATA? -50 0` mean exactly the same thing.
//
// `functionFilter`: an empty string ("") means every function, or one
// of "DCV"/"ACV"/"DCI"/"ACI"/"OHM"/"DB" (case-insensitive, matching
// keithleyFunctionPrefix()'s own possible outputs) to only consider
// records of that one function - added specifically so a log spanning
// more than one measurement function (the author switched DCV to OHM
// partway through, say) can still be queried/aggregated meaningfully:
// filtering at read time instead of clearing the log on a function
// change means nothing ever gets thrown away, but LOG:MIN?/MAX?/
// AVERage?/MEDian? still never mix incompatible units when asked not
// to. Record numbers in LOG:DATA?'s output are always the position in
// the WHOLE (unfiltered) log - filtering only skips non-matching rows,
// it never renumbers the ones that do match.
uint32_t datalogCountFiltered(const char *functionFilter, long first, long last);

// Prints records [first, last] (see numbering above), optionally
// restricted to `functionFilter` (see above), as CSV: a header row
// ("record,timestamp,status,function,value,indicators"), then one
// row per matching record, oldest to newest, e.g.
// "12,2026-09-24 14:03:07.42,N,DCV,1.23456E+1,AUTO" - or
// "12,NO-RTC-TIME,N,DCV,1.23456E+1,AUTO" for a record made before the
// RTC synced this session (see rtc.h). The "indicators" column is
// whichever of AUTO/STO/RCL/REM/BAT are set (space-separated within
// that one field) - AC/REL/MINUS are not repeated there, since
// they're already reflected in the status column and the value
// column's sign. Unlike a live SYSTem:READing=1 reply, the value
// column always gets a real '+'/'-' (never a blank space for an AC
// reading) - a plain signed number is easier for a CSV/spreadsheet
// consumer to parse, and column alignment doesn't matter once
// everything is comma-separated anyway.
//
// If SYSTem:AUTO is actively sending, an unsolicited reading line
// could otherwise land in the middle of this output - serial_out.ino
// suppresses that for the duration of any LOG:* reply (see
// suppressUnsolicitedOutput there), so what this prints is a single
// clean, uninterrupted block regardless of whether auto-send happens
// to also be running. datalogPrintRange() itself needs no extra
// scratch memory no matter how large [first,last] is - unlike
// datalogPrintMedian() below, it streams straight from the ring
// buffer - so it has no equivalent of DATALOG_MEDIAN_SAMPLE_CAP, and
// never samples/approximates anything.
void datalogPrintRange(Stream &interface, const char *functionFilter, long first, long last);
// v18 LOG:MARKers?: the same CSV, only the records that carry a marker.
void datalogPrintMarked(Stream &interface, const char *functionFilter, long first, long last);

// Statistics over the same [first, last] numbering and optional
// `functionFilter` - each prints a single scientific-notation number
// (no status/prefix, since even a filtered range could still be
// spanning more than one unit if two functions happen to share a
// prefix - they don't currently, but this doesn't assume otherwise)
// and nothing (not even a blank line) if nothing matches. Overload
// ("OL") records are excluded from all four regardless of the filter -
// their KEITHLEY_OVERLOAD_SENTINEL value would otherwise completely
// dominate MIN/MAX/AVERage and badly skew MEDian; LOG:DATA? above
// still shows them (with status 'O'), just not folded into these.
//
// MIN/MAX/AVERage need no extra memory (a running comparison/sum as
// they scan the range) and are always exact, however large the range.
// MEDian needs a temporary sortable copy of the matching values,
// allocated from PSRAM (a large range wouldn't fit the ESP32's much
// smaller internal heap) - kept to at most DATALOG_MEDIAN_SAMPLE_CAP
// floats regardless of the log's own size (see that macro's comment)
// by sampling every Nth record instead of every one once the
// requested range would otherwise need a bigger scratch buffer than
// that - see datalogPrintMedian()'s own comment in datalog.ino for
// the reasoning. A range that already fits the cap is still computed
// exactly, same as before this sampling was added.
void datalogPrintMin(Stream &interface, const char *functionFilter, long first, long last);
void datalogPrintMax(Stream &interface, const char *functionFilter, long first, long last);
void datalogPrintAverage(Stream &interface, const char *functionFilter, long first, long last);
void datalogPrintMedian(Stream &interface, const char *functionFilter, long first, long last);

// ---------------------------------------------------------------
// Statistics as numbers (used by the user interface, ui.ino)
// ---------------------------------------------------------------
// The LOG:MIN?/MAX?/AVERage?/MEDian? printers above do their
// computation straight into a Stream. The user interface needs the same
// four numbers (same range numbering - "-N, 0" = the last N records -,
// same function filter, overload records excluded), but a range can be
// the whole log, which is hundreds of thousands of records: scanning it
// in one go would keep the calling task busy for a second or more (the
// user interface only asks for up to an hour, but the job engine does not
// know that). So the computation is a JOB that is advanced in slices:
//
//     DatalogStatsJob job;
//     datalogStatsStart(&job, "DCV", first, last);
//     while (!datalogStatsStep(&job, 2000)) { ...do something else... }
//     DatalogStats result;
//     bool found = datalogStatsFinish(&job, &result);
//
// datalogStatsAbort() abandons a running job (it must be called, or the
// job finished, before another one is started - a static scratch buffer
// for the median is shared). One job at a time, from one task (the
// display render/UI task).
//
// The median is taken from at most DATALOG_STATS_MEDIAN_SAMPLES values:
// a range with more records contributes every Nth one. That keeps the
// sort - the one step that cannot be sliced - to a few milliseconds even
// for an hour of readings (about 12600 records, sampled by 4), and the
// result is the median of an evenly spaced sample, which differs from the
// exact one by a tiny fraction of the spread. Ranges up to that many
// records give the exact median. The minimum, maximum and average always
// cover every record.
//
// The log may keep growing while a job runs: it keeps working on the
// range resolved at the start, so once the log is full and the oldest
// records are being overwritten the result can be off by the few records
// written meanwhile - never out of bounds.
#define DATALOG_STATS_MEDIAN_SAMPLES 4096

// Result of a statistics job. Plain struct in this header (not in
// datalog.ino) for the same Arduino auto-prototype reason as LogRecord
// above.
struct DatalogStats
{
    uint32_t count;    // records that went into minimum/maximum/average (overload excluded)
    double minimum;
    double maximum;
    double average;
    double median;     // only meaningful when medianValid
    bool medianValid;  // false if the scratch memory for the median could not be allocated
};

// State of a running job - treat as opaque.
struct DatalogStatsJob
{
    bool active;
    long next;            // next record (1-based) to look at
    long last;            // last record of the range
    long stride;          // the median looks at every stride-th record of the range
    long strideOrigin;    // first record of the range
    char filter[8];       // function filter, "" = every function
    uint32_t count;
    double sum;
    double minimum;
    double maximum;
    float *samples;       // values for the median (the static buffer in datalog.ino)
    long sampleCount;
};

// Resolves the range and prepares the job. If the log is empty or the
// range is empty the job is simply not active (datalogStatsStep() then
// returns true at once and datalogStatsFinish() returns false).
void datalogStatsStart(DatalogStatsJob *job, const char *functionFilter, long first, long last);

// Looks at up to maxRecords more records. Returns true when the whole
// range has been processed.
bool datalogStatsStep(DatalogStatsJob *job, uint32_t maxRecords);

// Call once datalogStatsStep() has returned true. Fills `out`, releases
// the job's memory and returns true if any record matched.
bool datalogStatsFinish(DatalogStatsJob *job, DatalogStats *out);

// Abandons a running job and releases its memory. Harmless on a job
// that is not active.
void datalogStatsAbort(DatalogStatsJob *job);

// The same in one call (start + all steps + finish) - for callers that
// know the range is small. Blocks for as long as the scan takes.
bool datalogComputeStats(const char *functionFilter, long first, long last, DatalogStats *out);

// The first record (1-based, in the numbering above) that belongs to
// the last `windowSeconds` seconds, to be used as `first` of a range
// that runs to the newest record. 0 seconds means the whole log
// (returns 1). Uses the records' RTC timestamps; records made before
// the RTC was synced (timestamp 0) count as older than any window. If
// there is no usable time (no RTC now, or the newest record has no
// timestamp) the answer is an ESTIMATE from the recording rate - one
// record per measurement, CONFIG_UI_FALLBACK_RECORDS_PER_SECOND of
// them per second, or one per SYSTem:AUTO interval when that is set -
// and *usedEstimate is set to true (when not NULL). Returns 0 if the
// log is empty, and recordCount + 1 - an empty range - if records have
// timestamps but not even the newest is within the window (the DMM has
// not delivered anything for that long): there is nothing to compute
// statistics from, and the user interface shows dashes instead of the
// numbers of some old record.
long datalogFirstRecordSince(uint32_t windowSeconds, bool *usedEstimate);

// ---------------------------------------------------------------
// v14: following the log as it grows (the graph screen, graph.ino)
// ---------------------------------------------------------------
// The 1-based record numbers above shift by one with every new record once
// the ring buffer is full, so a reader that keeps its place between calls
// (the graph appends new records as they come) uses SERIAL numbers instead:
// the n-th record written since boot or the last clear has serial n - 1. A
// serial stays valid for as long as that record is stored. Every clear
// (datalogClear(), LOG:CLEar, RCL held on the statistics screen) bumps a
// "clear generation", so a reader can tell that what it had collected is
// gone.

// Copies up to maxCount records starting at serial *serial into dst, under
// ONE hold of the log mutex (keep maxCount small - 64, like the statistics
// job - so the writer on core 0 never waits long). If *serial is no longer
// stored (overwritten, or from before a clear) it is first moved to the
// oldest stored record. Returns the number of records copied; dst[0] is the
// record with the (possibly moved) *serial - the caller adds the returned
// count to *serial to continue. *clearGeneration (when not NULL) receives the
// current clear generation.
uint32_t datalogReadSerial(uint32_t *serial, LogRecord *dst, uint32_t maxCount, uint32_t *clearGeneration);

// Serial the next record will get (= number of records written since boot or
// the last clear), and the clear generation (when not NULL).
uint32_t datalogEndSerial(uint32_t *clearGeneration);

// Serial of the first stored record whose RTC timestamp is at least
// seconds2000 (seconds since 2000, see LogRecord); datalogEndSerial() if
// there is none. Records without a timestamp (0) count as older than any
// time. Binary search, about 20 short mutex holds for a full log.
uint32_t datalogFirstSerialAtOrAfter(uint32_t seconds2000);

// True if the record belongs to the function `functionFilter` ("DCV", "OHM",
// ...; "" = any) - the same test the LOG:* commands and the statistics use.
bool datalogRecordMatchesFunction(const char *functionFilter, const LogRecord *record);

// ---------------------------------------------------------------
// v18: markers (marker.h)
// ---------------------------------------------------------------
// A marker is one bit in the record's indicatorBits (the K197 uses the low 8:
// 7 indicators + minus), so a marked record costs nothing extra. LOG:DATA? /
// LOG:MARKers? list it as "TAG" in the indicators column.
#define DATALOG_BIT_MARKER 0x8000u
static_assert(DATALOG_BIT_MARKER > (1u << (KEITHLEY_INDICATOR_COUNT + 1)), "marker bit above the K197 indicator bits");

// v20: the first CONFIG_DATALOG_SETTLE_READINGS readings after a change of the
// measurement function (and after power-up) are still settling - logged and
// listed by LOG:DATA? ("SETTLE" among the indicators), but left out of the
// statistics (screen and LOG:MIN?/MAX?/AVERage?/MEDian?), the graph and the
// alarm's default limits.
#define DATALOG_BIT_SETTLING 0x4000u
static_assert(DATALOG_BIT_SETTLING > (1u << (KEITHLEY_INDICATOR_COUNT + 1)), "settling bit above the K197 indicator bits");

// True for a record the statistics and the graph skip: OL, or still settling.
static inline bool datalogRecordSkipped(const LogRecord &rec)
{
    return rec.overload || (rec.indicatorBits & DATALOG_BIT_SETTLING) != 0;
}

// How many of the newest markers datalogMarkSerialAt() still knows.
#define DATALOG_MARK_RING 16

// Puts a marker on the record that was the newest one (the one the display
// showed) at millis() `atMs` - normally a moment a few ms to a few hundred ms
// ago; older than the last DATALOG_APPEND_HISTORY records: the oldest of them.
// The serial of the marked record goes to *markedSerial (when not NULL).
// False if there is no record. Any task except the capture task.
bool datalogMarkAtMs(uint32_t atMs, uint32_t *markedSerial);

// Markers set since boot (never reset; a clear of the log keeps counting), and
// the serial of marker number `index` (0-based) while it is among the newest
// DATALOG_MARK_RING. For the graph, which must mark a column it has already
// filled.
uint32_t datalogMarkCount();
bool datalogMarkSerialAt(uint32_t index, uint32_t *serial);

#endif // DATALOG_H
