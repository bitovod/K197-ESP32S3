// graph.ino
// See graph.h (v14).

#include "graph.h"
#include "datalog.h"
#include "ui_format.h"
#include "rtc.h"
#include "settings.h"
#include "debuglog.h"
#include <algorithm> // v21: std::nth_element (ROBUST axis)
#include "help_text.h" // v15: HELP_LANG_* (the settings screen in the language of SYSTem:LANGuage)

// ---- constant tables ----

static const uint32_t graphIntervalSeconds[GRAPH_INTERVAL_COUNT] = {10, 30, 60, 300, 900, 3600, 21600, 86400, 0};
static const char *const graphIntervalLabels[GRAPH_INTERVAL_COUNT] = {"10 SEC", "30 SEC", "1 MIN", "5 MIN", "15 MIN", "1 HOUR",
                                                                     "6 HRS", "24 HRS", "FULL"};
// v21: +-1, +-2 and +-5 counts added in front (see GRAPH_SPANS_ADDED_V21 in settings.h)
static const uint16_t graphSpanCounts[GRAPH_SPAN_COUNT] = {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000};
static const char *const graphYModeNames[GRAPH_YMODE_COUNT] = {"AUTO", "ZERO", "SUPP", "ROBUST"};
// v21: order of the modes on the RCL click and on the settings screen (ROBUST is stored as 3, shown after AUTO)
static const uint8_t graphYModeOrder[GRAPH_YMODE_COUNT] = {GRAPH_YMODE_AUTO, GRAPH_YMODE_ROBUST, GRAPH_YMODE_ZERO, GRAPH_YMODE_SUPP};
// v21: the column extremes for the ROBUST axis (float: 2 x 1.5 kB, the precision of the log values)
static float graphRobustHigh[FB_GRAPH_COLUMNS];
static float graphRobustLow[FB_GRAPH_COLUMNS];

// FULL: the window is the smallest of these (seconds) that holds 1.1 x the
// time the log covers - the last one if even that is not enough.
static const uint32_t graphFullSpans[] = {60, 300, 900, 1800, 3600, 7200, 10800, 21600, 43200,
                                          86400, 172800, 259200, 345600, 432000, 518400, 604800};
#define GRAPH_FULL_SPAN_COUNT (sizeof(graphFullSpans) / sizeof(graphFullSpans[0]))

// Distance of the time grid lines (seconds): the smallest that gives at most
// GRAPH_MAX_TIME_DIVISIONS divisions over the window.
static const uint32_t graphTickSeconds[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200,
                                            10800, 21600, 43200, 86400, 172800};
#define GRAPH_TICK_CHOICES (sizeof(graphTickSeconds) / sizeof(graphTickSeconds[0]))
#define GRAPH_MAX_TIME_DIVISIONS 6

// Records fetched per mutex hold of the log (like the statistics job).
#define GRAPH_READ_CHUNK 64

// The Y axis: at most this many divisions (grid lines on 1-2-5 values), and
// never less than this many counts of the last displayed digit high (AUTO,
// ZERO) - a perfectly steady reading would otherwise have a zero range.
#define GRAPH_Y_DIVISIONS  4
#define GRAPH_MIN_COUNTS   4

// ---- settings ----

static uint8_t graphInterval = CONFIG_GRAPH_DEFAULT_INTERVAL_INDEX;
static uint8_t graphYMode = CONFIG_GRAPH_DEFAULT_YMODE;
static uint8_t graphTrace = CONFIG_GRAPH_DEFAULT_TRACE;
static uint8_t graphSpan = CONFIG_GRAPH_DEFAULT_SPAN_INDEX;

// ---- the columns and what they were built for ----

static GraphColumn graphCols[FB_GRAPH_COLUMNS];
static bool graphBuilt = false;
static char graphCtxFilter[8] = "";
static uint8_t graphCtxInterval = 0;
static uint32_t graphCtxClearGen = 0;
// The window start is kept as an integer in units of 1/FB_GRAPH_COLUMNS centisecond: with the column
// borders on multiples of the column length (span * 100 / FB_GRAPH_COLUMNS centiseconds) it is always a
// whole number there, and the column of a record is then one 64-bit integer division - the ESP32-S3
// has no double precision FPU, and this runs for every record (up to ~500k for FULL).
static int64_t graphStartU = 0;
static double graphStartCs = 0;  // the same in centiseconds since 2000 (for the drawing)
static double graphColumnCs = 1; // one column, centiseconds
static uint32_t graphSpanSeconds = 60;
static uint32_t graphNextSerial = 0; // next log record to look at
static uint32_t graphMarksSeen = 0;  // v18: datalogMarkCount() already applied to the columns
static bool graphCatchingUp = false; // older records still to be read
static bool graphNeedRebuild = false; // FULL: a record past the end of the window
static uint32_t graphRecordsAdded = 0;
static uint32_t graphRebuilds = 0;
// v16 (SYSTem:DIAGnostic?): how long reading the log for a (re)built graph took, until the newest record
static uint32_t graphBuildStartMs = 0;
static uint32_t graphBuildLastMs = 0;
static uint32_t graphBuildMaxMs = 0;
static uint32_t graphBuildLastRecords = 0;
static LogRecord graphReadBuf[GRAPH_READ_CHUNK]; // static: 832 bytes are too many for the render task's stack

// ---- SUPP centre (display units of the live reading) ----

static bool graphCenterValid = false;
static double graphCenter = 0;
static uint8_t graphCenterUnit = KEITHLEY_UNIT_NONE;
static uint8_t graphCenterDecimals = 0;

// ---- live reading, scene ----

static GraphLive graphLive;
static FbGraphScene graphSceneData;
static bool graphDirty = true;       // columns or live value changed since the last scene
static bool graphSceneForce = true;  // settings changed: new scene at once
static uint32_t graphSceneMs = 0;
static char graphLastTime[FB_GRAPH_TEXT_LEN] = "";
static double graphAxisLow = 0;
static double graphAxisHigh = 0;

// ---------------------------------------------------------------
// Settings
// ---------------------------------------------------------------

static void graphPersist()
{
    settingsSetGraphDeferred(graphInterval, graphYMode, graphTrace, graphSpan);
}

void graphBegin()
{
    memset(&graphLive, 0, sizeof(graphLive));
    memset(&graphSceneData, 0, sizeof(graphSceneData));
    graphInterval = settingsGetGraphInterval();
    if (graphInterval >= GRAPH_INTERVAL_COUNT)
    {
        graphInterval = CONFIG_GRAPH_DEFAULT_INTERVAL_INDEX;
    }
    graphYMode = settingsGetGraphYMode();
    if (graphYMode >= GRAPH_YMODE_COUNT)
    {
        graphYMode = CONFIG_GRAPH_DEFAULT_YMODE;
    }
    graphTrace = settingsGetGraphTrace();
    if (graphTrace >= GRAPH_TRACE_COUNT)
    {
        graphTrace = CONFIG_GRAPH_DEFAULT_TRACE;
    }
    graphSpan = settingsGetGraphSpan();
    if (graphSpan >= GRAPH_SPAN_COUNT)
    {
        graphSpan = CONFIG_GRAPH_DEFAULT_SPAN_INDEX;
    }
}

uint8_t graphGetInterval()
{
    return graphInterval;
}

void graphSetInterval(uint8_t index)
{
    if (index >= GRAPH_INTERVAL_COUNT)
    {
        index = GRAPH_INTERVAL_COUNT - 1;
    }
    graphInterval = index; // graphPoll() sees the new context and rebuilds
    graphSceneForce = true;
    graphPersist();
}

uint8_t graphGetYMode()
{
    return graphYMode;
}

void graphSetYMode(uint8_t mode)
{
    if (mode >= GRAPH_YMODE_COUNT)
    {
        mode = GRAPH_YMODE_COUNT - 1;
    }
    if (mode == GRAPH_YMODE_SUPP && graphYMode != GRAPH_YMODE_SUPP)
    {
        graphCenterValid = false; // newly chosen: centred on the newest value
    }
    graphYMode = mode;
    graphSceneForce = true;
    graphPersist();
}

uint8_t graphGetTrace()
{
    return graphTrace;
}

void graphSetTrace(uint8_t trace)
{
    if (trace >= GRAPH_TRACE_COUNT)
    {
        trace = GRAPH_TRACE_COUNT - 1;
    }
    graphTrace = trace;
    graphSceneForce = true;
    graphPersist();
}

uint8_t graphGetSpan()
{
    return graphSpan;
}

void graphSetSpan(uint8_t index)
{
    if (index >= GRAPH_SPAN_COUNT)
    {
        index = GRAPH_SPAN_COUNT - 1;
    }
    graphSpan = index;
    graphSceneForce = true;
    graphPersist();
}

const char *graphIntervalLabel(uint8_t index)
{
    return graphIntervalLabels[(index < GRAPH_INTERVAL_COUNT) ? index : (GRAPH_INTERVAL_COUNT - 1)];
}

void graphRecenter()
{
    graphCenterValid = false;
    if (graphYMode != GRAPH_YMODE_SUPP)
    {
        graphYMode = GRAPH_YMODE_SUPP;
        graphPersist();
    }
    graphSceneForce = true;
}

void graphInvalidate()
{
    graphBuilt = false;
    graphSceneForce = true;
}

void graphSetLive(const GraphLive *live)
{
    if (memcmp(live, &graphLive, sizeof(GraphLive)) != 0)
    {
        graphLive = *live;
        graphDirty = true;
    }
}

bool graphBusy()
{
    return graphBuilt && graphCatchingUp;
}

const FbGraphScene &graphScene()
{
    return graphSceneData;
}

GraphDebug graphGetDebug()
{
    GraphDebug d;
    d.built = graphBuilt;
    d.busy = graphBusy();
    d.nextSerial = graphNextSerial;
    d.recordsAdded = graphRecordsAdded;
    d.startCs = graphStartCs;
    d.columnCs = graphColumnCs;
    d.spanSeconds = graphSpanSeconds;
    d.axisLow = graphAxisLow;
    d.axisHigh = graphAxisHigh;
    d.rebuilds = graphRebuilds;
    d.lastBuildMs = graphBuildLastMs;
    d.maxBuildMs = graphBuildMaxMs;
    d.lastBuildRecords = graphBuildLastRecords;
    return d;
}

// ---------------------------------------------------------------
// Columns
// ---------------------------------------------------------------

static bool graphNowCs(double *nowCs)
{
    uint32_t seconds = 0;
    uint8_t centis = 0;
    if (!rtcCompactTimestamp(&seconds, &centis) || seconds == 0)
    {
        return false;
    }
    *nowCs = (double)seconds * 100.0 + (double)centis;
    return true;
}

static void graphClearColumns(uint16_t from, uint16_t count)
{
    memset(&graphCols[from], 0, sizeof(GraphColumn) * count);
}

// Starts the columns over for the current function and interval: the window
// ends now (FULL: starts at the oldest stored record), reading starts at the
// first record of the window.
static void graphRebuild(double nowCs)
{
    graphClearColumns(0, FB_GRAPH_COLUMNS);
    snprintf(graphCtxFilter, sizeof(graphCtxFilter), "%s", graphLive.filter);
    graphCtxInterval = graphInterval;
    datalogEndSerial(&graphCtxClearGen);

    if (graphInterval == GRAPH_INTERVAL_FULL)
    {
        uint32_t first = datalogFirstSerialAtOrAfter(1); // the oldest record with an RTC timestamp
        uint32_t serial = first;
        double oldestCs = nowCs;
        if (datalogReadSerial(&serial, graphReadBuf, 1, NULL) == 1)
        {
            oldestCs = (double)graphReadBuf[0].timestampSeconds2000 * 100.0;
            first = serial;
        }
        if (oldestCs > nowCs)
        {
            oldestCs = nowCs;
        }
        double wanted = (nowCs - oldestCs) / 100.0 * 1.1;
        graphSpanSeconds = graphFullSpans[GRAPH_FULL_SPAN_COUNT - 1];
        for (uint8_t i = 0; i < GRAPH_FULL_SPAN_COUNT; i++)
        {
            if ((double)graphFullSpans[i] >= wanted)
            {
                graphSpanSeconds = graphFullSpans[i];
                break;
            }
        }
        graphStartU = (int64_t)oldestCs * FB_GRAPH_COLUMNS; // whole seconds: an integer
        graphNextSerial = first;
    }
    else
    {
        // The column borders sit on whole multiples of the column length (counted from 2000), so a record
        // always lands in the same column, however often the window is rebuilt; "now" lies in the last column.
        graphSpanSeconds = graphIntervalSeconds[graphInterval];
        int64_t spanCs = (int64_t)graphSpanSeconds * 100;
        int64_t nowU = (int64_t)nowCs * FB_GRAPH_COLUMNS;
        int64_t columnsSince2000 = nowU / spanCs; // the column "now" falls into
        graphStartU = (columnsSince2000 + 1 - FB_GRAPH_COLUMNS) * spanCs;
        double startSeconds = floor((double)graphStartU / FB_GRAPH_COLUMNS / 100.0);
        graphNextSerial = datalogFirstSerialAtOrAfter((startSeconds > 0) ? (uint32_t)startSeconds : 0);
    }
    graphColumnCs = (double)graphSpanSeconds * 100.0 / (double)FB_GRAPH_COLUMNS;
    graphStartCs = (double)graphStartU / (double)FB_GRAPH_COLUMNS;
    graphMarksSeen = datalogMarkCount(); // v18: older markers are in the records themselves
    graphBuilt = true;
    graphCatchingUp = true;
    graphNeedRebuild = false;
    graphRecordsAdded = 0;
    graphRebuilds++;
    graphBuildStartMs = (uint32_t)millis();
    graphDirty = true;
    debugLogf("GRAPH: rebuild, %s, window %lu s, from record serial %lu", graphIntervalLabels[graphInterval],
              (unsigned long)graphSpanSeconds, (unsigned long)graphNextSerial);
}

// Moves the window `shift` columns to the right (the columns move left).
static void graphShift(uint32_t shift)
{
    if (shift >= FB_GRAPH_COLUMNS)
    {
        graphClearColumns(0, FB_GRAPH_COLUMNS);
    }
    else
    {
        memmove(&graphCols[0], &graphCols[shift], sizeof(GraphColumn) * (FB_GRAPH_COLUMNS - shift));
        graphClearColumns((uint16_t)(FB_GRAPH_COLUMNS - shift), (uint16_t)shift);
    }
    graphStartU += (int64_t)shift * (int64_t)graphSpanSeconds * 100;
    graphStartCs = (double)graphStartU / (double)FB_GRAPH_COLUMNS;
    graphDirty = true;
}

static double graphEndCs()
{
    return graphStartCs + (double)FB_GRAPH_COLUMNS * graphColumnCs;
}

// The fixed windows follow the clock: once now has passed the right edge,
// scroll by whole columns. A jump by a whole window or more (the screen was
// not shown for a while) starts over instead - quicker than reading
// everything in between. FULL rebuilds with a longer window.
static void graphFollowClock(double nowCs)
{
    double end = graphEndCs();
    if (nowCs < end)
    {
        return;
    }
    if (graphCtxInterval == GRAPH_INTERVAL_FULL)
    {
        graphRebuild(nowCs);
        return;
    }
    uint32_t shift = (uint32_t)floor((nowCs - end) / graphColumnCs) + 1;
    if (shift >= FB_GRAPH_COLUMNS)
    {
        graphRebuild(nowCs);
        return;
    }
    graphShift(shift);
}

// v18: a marker put on a record the graph has already read - marks its column
// (if the record is of this function and inside the window; never scrolls).
static void graphMarkColumnOf(const LogRecord *rec)
{
    if (rec->timestampSeconds2000 == 0 || !datalogRecordMatchesFunction(graphCtxFilter, rec))
    {
        return;
    }
    int64_t tU = ((int64_t)rec->timestampSeconds2000 * 100 + rec->timestampCentisecond) * FB_GRAPH_COLUMNS;
    if (tU < graphStartU)
    {
        return;
    }
    int64_t column = (tU - graphStartU) / ((int64_t)graphSpanSeconds * 100);
    if (column >= FB_GRAPH_COLUMNS)
    {
        return;
    }
    graphCols[(uint16_t)column].marked = 1;
    graphDirty = true;
}

// v18: markers set since the last look whose record was already read.
static void graphApplyNewMarks()
{
    uint32_t count = datalogMarkCount();
    if (count == graphMarksSeen)
    {
        return;
    }
    uint32_t from = (count - graphMarksSeen > DATALOG_MARK_RING) ? count - DATALOG_MARK_RING : graphMarksSeen;
    for (uint32_t k = from; k < count; k++)
    {
        uint32_t serial;
        if (!datalogMarkSerialAt(k, &serial) || serial >= graphNextSerial)
        {
            continue; // not read yet: the record itself carries the marker when it is
        }
        LogRecord rec;
        uint32_t s = serial;
        if (datalogReadSerial(&s, &rec, 1, NULL) == 1 && s == serial)
        {
            graphMarkColumnOf(&rec);
        }
    }
    graphMarksSeen = count;
}

// Adds one record. Returns false if the record lies past the end of a FULL
// window - it is then read again after the rebuild.
static bool graphAddRecord(const LogRecord *rec)
{
    if (rec->timestampSeconds2000 == 0)
    {
        return true; // made before the RTC synced: no place on the time axis
    }
    if (!datalogRecordMatchesFunction(graphCtxFilter, rec))
    {
        return true;
    }
    int64_t tU = ((int64_t)rec->timestampSeconds2000 * 100 + rec->timestampCentisecond) * FB_GRAPH_COLUMNS;
    if (tU < graphStartU)
    {
        return true;
    }
    int64_t column = (tU - graphStartU) / ((int64_t)graphSpanSeconds * 100);
    if (column >= FB_GRAPH_COLUMNS)
    {
        if (graphCtxInterval == GRAPH_INTERVAL_FULL)
        {
            graphNeedRebuild = true;
            return false;
        }
        int64_t shift = column - FB_GRAPH_COLUMNS + 1;
        graphShift((shift > FB_GRAPH_COLUMNS) ? (uint32_t)FB_GRAPH_COLUMNS : (uint32_t)shift);
        column = (tU - graphStartU) / ((int64_t)graphSpanSeconds * 100); // FB_GRAPH_COLUMNS - 1, or less after a reset
        if (column < 0 || column >= FB_GRAPH_COLUMNS)
        {
            // more than a window ahead: the columns were cleared, start the window at this record
            int64_t spanCs = (int64_t)graphSpanSeconds * 100;
            graphStartU = (tU / spanCs + 1 - FB_GRAPH_COLUMNS) * spanCs;
            graphStartCs = (double)graphStartU / (double)FB_GRAPH_COLUMNS;
            column = FB_GRAPH_COLUMNS - 1;
        }
    }
    GraphColumn &c = graphCols[(uint16_t)column];
    if ((rec->indicatorBits & DATALOG_BIT_MARKER) != 0)
    {
        c.marked = 1; // v18
    }
    if ((rec->indicatorBits & DATALOG_BIT_SETTLING) != 0)
    {
        // v20: still settling after a change of the function - no value, no OL mark
        graphRecordsAdded++;
        return true;
    }
    if (rec->overload)
    {
        c.overload = 1;
    }
    else
    {
        float v = rec->value;
        if (c.count == 0 || v < c.minimum)
        {
            c.minimum = v;
        }
        if (c.count == 0 || v > c.maximum)
        {
            c.maximum = v;
        }
        c.sum += (double)v;
        c.count++;
    }
    graphRecordsAdded++;
    graphDirty = true;
    return true;
}

// Reads records for at most CONFIG_GRAPH_SLICE_US (at least one chunk).
static void graphReadSlice()
{
    uint32_t sliceStartUs = micros();
    for (;;)
    {
        uint32_t generation = 0;
        uint32_t serial = graphNextSerial;
        uint32_t n = datalogReadSerial(&serial, graphReadBuf, GRAPH_READ_CHUNK, &generation);
        if (generation != graphCtxClearGen)
        {
            graphBuilt = false; // the log was cleared meanwhile: start over next time
            return;
        }
        uint32_t used = 0;
        while (used < n && graphAddRecord(&graphReadBuf[used]))
        {
            used++;
        }
        graphNextSerial = serial + used;
        if (graphNeedRebuild)
        {
            return;
        }
        if (n < GRAPH_READ_CHUNK)
        {
            graphCatchingUp = false; // reached the newest record
            return;
        }
        if ((uint32_t)(micros() - sliceStartUs) >= (uint32_t)CONFIG_GRAPH_SLICE_US)
        {
            graphCatchingUp = true; // more than a slice behind (also after a burst of new records)
            return;
        }
    }
}

// ---------------------------------------------------------------
// Scene
// ---------------------------------------------------------------

// Smallest 1-2-5 value >= raw, but at least minStep.
static double graphNiceStep(double raw, double minStep)
{
    if (!(raw > 0))
    {
        return minStep;
    }
    double e = pow(10.0, floor(log10(raw)));
    double step = 10.0 * e;
    const double m[3] = {1.0, 2.0, 5.0};
    for (uint8_t i = 0; i < 3; i++)
    {
        // 0.1 % tolerance: the values are floats, a range of "exactly" 80 counts arrives as 80.0014 counts,
        // and without it the grid would jump to the next coarser step (here: one grid line instead of five)
        if (m[i] * e >= raw * (1.0 - 1e-3))
        {
            step = m[i] * e;
            break;
        }
    }
    return (step < minStep) ? minStep : step;
}

// Row of a value (display units) on the current axis, not clamped.
static double graphRowOf(double v)
{
    double span = graphAxisHigh - graphAxisLow;
    return (double)FB_GRAPH_BOTTOM - (v - graphAxisLow) / span * (double)(FB_GRAPH_BOTTOM - FB_GRAPH_TOP);
}

static int16_t graphClampRow(double row)
{
    double r = floor(row + 0.5);
    if (r < FB_GRAPH_TOP)
    {
        return FB_GRAPH_TOP;
    }
    if (r > FB_GRAPH_BOTTOM)
    {
        return FB_GRAPH_BOTTOM;
    }
    return (int16_t)r;
}

static void graphFormatSeconds(char *out, size_t size, uint32_t seconds, uint32_t tick)
{
    if (seconds == 0)
    {
        snprintf(out, size, "0");
    }
    else if (tick >= 86400 && seconds % 86400 == 0)
    {
        snprintf(out, size, "-%lud", (unsigned long)(seconds / 86400));
    }
    else if (tick >= 3600 && seconds % 3600 == 0)
    {
        snprintf(out, size, "-%luh", (unsigned long)(seconds / 3600));
    }
    else if (tick >= 60 && seconds % 60 == 0)
    {
        snprintf(out, size, "-%lum", (unsigned long)(seconds / 60));
    }
    else
    {
        snprintf(out, size, "-%lus", (unsigned long)(seconds % 100000UL)); // (never more than 6 digits here)
    }
}

static void graphBuildTimeTicks(FbGraphScene &sc, double nowCs)
{
    uint32_t tick = graphTickSeconds[GRAPH_TICK_CHOICES - 1];
    for (uint8_t i = 0; i < GRAPH_TICK_CHOICES; i++)
    {
        if (graphSpanSeconds <= graphTickSeconds[i] * GRAPH_MAX_TIME_DIVISIONS)
        {
            tick = graphTickSeconds[i];
            break;
        }
    }
    // Collected from "now" backwards, stored left to right.
    FbGraphTick ticks[FB_GRAPH_MAX_XTICKS];
    uint8_t count = 0;
    double end = graphEndCs();
    for (uint32_t k = 0; count < FB_GRAPH_MAX_XTICKS; k++)
    {
        double t = nowCs - (double)k * (double)tick * 100.0;
        if (t < graphStartCs - graphColumnCs)
        {
            break; // (one column of tolerance: the window starts up to a column after now - span)
        }
        if (t > end)
        {
            continue;
        }
        double column = floor((t - graphStartCs) / graphColumnCs);
        if (column < 0)
        {
            column = 0;
        }
        if (column > FB_GRAPH_COLUMNS - 1)
        {
            column = FB_GRAPH_COLUMNS - 1;
        }
        ticks[count].pos = (int16_t)(FB_GRAPH_X0 + 1 + (int16_t)column);
        graphFormatSeconds(ticks[count].label, sizeof(ticks[count].label), k * tick, tick);
        count++;
    }
    sc.xTickCount = count;
    for (uint8_t i = 0; i < count; i++)
    {
        sc.xTicks[i] = ticks[count - 1 - i];
    }
}

// Gap (seconds) up to which two columns with values are joined.
static double graphGapSeconds()
{
    double gap = (double)CONFIG_GRAPH_GAP_SECONDS;
    double autoGap = 2.5 * (double)settingsGetAutoIntervalSeconds();
    return (autoGap > gap) ? autoGap : gap;
}

static void graphBuildScene(bool haveTime, double nowCs)
{
    FbGraphScene &sc = graphSceneData;
    sc.message[0] = '\0';
    sc.yTickCount = 0;
    sc.xTickCount = 0;
    sc.drawEnvelope = (graphTrace != GRAPH_TRACE_AVG);
    sc.drawAverage = (graphTrace != GRAPH_TRACE_ENVELOPE);
    for (uint16_t i = 0; i < FB_GRAPH_COLUMNS; i++)
    {
        sc.cols[i].flags = 0;
    }

    // ---- header ----
    char unitText[8];
    uiUnitText(graphLive.unit, unitText, sizeof(unitText));
    if (!graphLive.valid)
    {
        sc.headLeft[0] = '\0';
    }
    else if (strcmp(graphLive.filter, "DB") == 0)
    {
        snprintf(sc.headLeft, sizeof(sc.headLeft), "%s", unitText);
    }
    else
    {
        snprintf(sc.headLeft, sizeof(sc.headLeft), "%s %s", graphLive.filter, unitText);
    }
    snprintf(sc.headInterval, sizeof(sc.headInterval), "%s", graphIntervalLabels[graphInterval]);
    snprintf(sc.headMode, sizeof(sc.headMode), "%s%s", graphYModeNames[graphYMode], graphBusy() ? " ..." : "");
    snprintf(sc.headTime, sizeof(sc.headTime), "%s", graphLastTime);
    if (graphLive.valid)
    {
        snprintf(sc.headLive, sizeof(sc.headLive), "%s %s", graphLive.text, unitText);
    }
    else
    {
        snprintf(sc.headLive, sizeof(sc.headLive), "%s", UI_PLACEHOLDER);
    }

    if (!haveTime)
    {
        snprintf(sc.message, sizeof(sc.message), "NO RTC TIME");
        return;
    }
    if (!graphLive.valid)
    {
        snprintf(sc.message, sizeof(sc.message), "NO READING");
        return;
    }
    if (!graphBuilt)
    {
        return;
    }
    graphBuildTimeTicks(sc, nowCs);

    // ---- value range of the columns (display units) ----
    double scale = uiUnitMultiplier(graphLive.unit);
    double resolution = pow(10.0, -(double)graphLive.decimals);
    bool any = false;
    double low = 0;
    double high = 0;
    double newest = 0;
    uint16_t robustCount = 0;
    for (uint16_t i = 0; i < FB_GRAPH_COLUMNS; i++)
    {
        const GraphColumn &c = graphCols[i];
        if (c.count == 0)
        {
            continue;
        }
        double avg = c.sum / (double)c.count / scale;
        double lo = sc.drawEnvelope ? (double)c.minimum / scale : avg;
        double hi = sc.drawEnvelope ? (double)c.maximum / scale : avg;
        if (!any || lo < low)
        {
            low = lo;
        }
        if (!any || hi > high)
        {
            high = hi;
        }
        newest = avg;
        any = true;
        graphRobustHigh[robustCount] = (float)hi;
        graphRobustLow[robustCount] = (float)lo;
        robustCount++;
    }
    if (graphYMode == GRAPH_YMODE_ROBUST && robustCount >= CONFIG_GRAPH_ROBUST_MIN_COLUMNS)
    {
        // v21: leave out the CONFIG_GRAPH_ROBUST_PERCENT % highest maxima and lowest minima
        uint16_t skip = (uint16_t)((uint32_t)robustCount * CONFIG_GRAPH_ROBUST_PERCENT / 100);
        std::nth_element(graphRobustHigh, graphRobustHigh + (robustCount - 1 - skip), graphRobustHigh + robustCount);
        std::nth_element(graphRobustLow, graphRobustLow + skip, graphRobustLow + robustCount);
        high = graphRobustHigh[robustCount - 1 - skip];
        low = graphRobustLow[skip];
    }
    for (uint16_t i = 0; i < FB_GRAPH_COLUMNS; i++)
    {
        if (graphCols[i].overload)
        {
            sc.cols[i].flags = (uint8_t)(sc.cols[i].flags | FB_GRAPH_COL_OL);
        }
        if (graphCols[i].marked)
        {
            sc.cols[i].flags = (uint8_t)(sc.cols[i].flags | FB_GRAPH_COL_MARK); // v18
        }
    }
    if (!any)
    {
        if (!graphCatchingUp)
        {
            snprintf(sc.message, sizeof(sc.message), "NO DATA");
        }
        return;
    }

    // ---- axis ----
    double minSpan = GRAPH_MIN_COUNTS * resolution;
    if (graphYMode == GRAPH_YMODE_SUPP)
    {
        if (!graphCenterValid || graphCenterUnit != graphLive.unit || graphCenterDecimals != graphLive.decimals)
        {
            graphCenter = floor(newest / resolution + 0.5) * resolution;
            graphCenterUnit = graphLive.unit;
            graphCenterDecimals = graphLive.decimals;
            graphCenterValid = true;
        }
        // the chosen span plus a reserve on each side (CONFIG_GRAPH_SUPP_MARGIN_PERCENT of the span), so a value
        // just outside +- span is still drawn instead of being clipped at once
        double half = (double)graphSpanCounts[graphSpan] * resolution * (1.0 + (double)CONFIG_GRAPH_SUPP_MARGIN_PERCENT / 100.0);
        low = graphCenter - half;
        high = graphCenter + half;
    }
    else if (graphYMode == GRAPH_YMODE_ZERO)
    {
        if (low > 0)
        {
            low = 0;
        }
        if (high < 0)
        {
            high = 0;
        }
        if (high - low < minSpan)
        {
            if (high <= 0 && low < 0)
            {
                low = high - minSpan;
            }
            else
            {
                high = low + minSpan;
            }
        }
        double step = graphNiceStep((high - low) / GRAPH_Y_DIVISIONS, resolution);
        low = floor(low / step + 1e-9) * step;
        high = ceil(high / step - 1e-9) * step;
    }
    else if (graphYMode == GRAPH_YMODE_AUTO || graphYMode == GRAPH_YMODE_ROBUST)
    {
        // (v21: not in SUPP - its span is chosen by the user, down to +-1 count)
        if (high - low < minSpan)
        {
            double mid = (low + high) / 2.0;
            low = mid - minSpan / 2.0;
            high = mid + minSpan / 2.0;
        }
    }
    graphAxisLow = low;
    graphAxisHigh = high;

    // ---- Y grid ----
    double step = graphNiceStep((high - low) / GRAPH_Y_DIVISIONS, resolution);
    int labelDecimals = (int)ceil(-log10(step) - 1e-9);
    if (labelDecimals < 0)
    {
        labelDecimals = 0;
    }
    if (labelDecimals > (int)graphLive.decimals)
    {
        labelDecimals = (int)graphLive.decimals;
    }
    for (double t = ceil(low / step - 1e-6) * step; t <= high + step * 1e-6 && sc.yTickCount < FB_GRAPH_MAX_YTICKS; t += step)
    {
        FbGraphTick &tk = sc.yTicks[sc.yTickCount++];
        tk.pos = graphClampRow(graphRowOf(t));
        double shown = (fabs(t) < step * 1e-6) ? 0.0 : t;
        int length = snprintf(tk.label, sizeof(tk.label), "%.*f", labelDecimals, shown);
        if (length > FB_GRAPH_YLABEL_CHARS)
        {
            tk.label[0] = '\0'; // does not fit left of the axis: grid line only
        }
    }

    // ---- columns ----
    double gapColumns = graphGapSeconds() * 100.0 / graphColumnCs;
    int16_t previous = -1;
    for (uint16_t i = 0; i < FB_GRAPH_COLUMNS; i++)
    {
        const GraphColumn &c = graphCols[i];
        if (c.count == 0)
        {
            continue;
        }
        FbGraphColumn &out = sc.cols[i];
        double avg = c.sum / (double)c.count / scale;
        double rowMax = graphRowOf((double)c.maximum / scale);
        double rowMin = graphRowOf((double)c.minimum / scale);
        double rowAvg = graphRowOf(avg);
        out.flags = (uint8_t)(out.flags | FB_GRAPH_COL_DATA);
        out.yTop = graphClampRow(rowMax);
        out.yBottom = graphClampRow(rowMin);
        out.yAvg = graphClampRow(rowAvg);
        double top = sc.drawEnvelope ? rowMax : rowAvg;
        double bottom = sc.drawEnvelope ? rowMin : rowAvg;
        if (top < (double)FB_GRAPH_TOP - 0.5)
        {
            out.flags = (uint8_t)(out.flags | FB_GRAPH_COL_CLIP_HI);
        }
        if (bottom > (double)FB_GRAPH_BOTTOM + 0.5)
        {
            out.flags = (uint8_t)(out.flags | FB_GRAPH_COL_CLIP_LO);
        }
        if (previous >= 0 && ((int16_t)i - previous == 1 || (double)((int16_t)i - previous - 1) <= gapColumns))
        {
            out.flags = (uint8_t)(out.flags | FB_GRAPH_COL_CONNECT);
        }
        previous = (int16_t)i;
    }
}

// ---------------------------------------------------------------
// Poll
// ---------------------------------------------------------------

// v15: the SCPI GRAPh:* commands write the settings directly (settingsSetGraph(), saved at once, from the
// comm task); the graph adopts them here, in its own task - so only this task ever changes the graph's own
// state. Front-panel changes come the other way (graphSet*() -> settingsSetGraphDeferred()), so both always
// agree afterwards. Out-of-range stored values are ignored.
static void graphSyncFromSettings()
{
    uint8_t v = settingsGetGraphInterval();
    if (v < GRAPH_INTERVAL_COUNT && v != graphInterval)
    {
        graphInterval = v; // graphPoll() sees the new context and rebuilds
        graphSceneForce = true;
    }
    v = settingsGetGraphYMode();
    if (v < GRAPH_YMODE_COUNT && v != graphYMode)
    {
        if (v == GRAPH_YMODE_SUPP)
        {
            graphCenterValid = false; // newly chosen: centred on the newest value
        }
        graphYMode = v;
        graphSceneForce = true;
    }
    v = settingsGetGraphTrace();
    if (v < GRAPH_TRACE_COUNT && v != graphTrace)
    {
        graphTrace = v;
        graphSceneForce = true;
    }
    v = settingsGetGraphSpan();
    if (v < GRAPH_SPAN_COUNT && v != graphSpan)
    {
        graphSpan = v;
        graphSceneForce = true;
    }
}

uint32_t graphIntervalSecondsAt(uint8_t index)
{
    return (index < GRAPH_INTERVAL_COUNT) ? graphIntervalSeconds[index] : 0;
}

uint16_t graphSpanCountsAt(uint8_t index)
{
    return (index < GRAPH_SPAN_COUNT) ? graphSpanCounts[index] : 0;
}

bool graphPoll()
{
    graphSyncFromSettings();
    double nowCs = 0;
    bool haveTime = graphNowCs(&nowCs);

    // header clock: a new scene every second
    bool clockChanged = false;
    {
        char text[FB_GRAPH_TEXT_LEN];
        uint16_t year;
        uint8_t month, day, hour, minute, second;
        if (rtcGetDateTime(&year, &month, &day, &hour, &minute, &second))
        {
            snprintf(text, sizeof(text), "%02u:%02u:%02u", (unsigned)hour, (unsigned)minute, (unsigned)second);
        }
        else
        {
            text[0] = '\0';
        }
        if (strcmp(text, graphLastTime) != 0)
        {
            snprintf(graphLastTime, sizeof(graphLastTime), "%s", text);
            clockChanged = true;
        }
    }

    bool wasCatchingUp = graphBusy();
    if (haveTime && graphLive.valid)
    {
        uint32_t generation = 0;
        datalogEndSerial(&generation);
        bool contextChanged = !graphBuilt || strcmp(graphLive.filter, graphCtxFilter) != 0 ||
                              graphInterval != graphCtxInterval || generation != graphCtxClearGen;
        if (contextChanged)
        {
            graphRebuild(nowCs);
            graphSceneForce = true;
        }
        else
        {
            graphFollowClock(nowCs);
        }
        graphReadSlice();
        if (graphNeedRebuild)
        {
            graphRebuild(nowCs);
            graphReadSlice();
        }
        graphApplyNewMarks(); // v18
    }

    uint32_t nowMs = (uint32_t)millis();
    bool finished = wasCatchingUp && !graphBusy();
    if (finished)
    {
        graphBuildLastMs = (uint32_t)millis() - graphBuildStartMs;
        graphBuildLastRecords = graphRecordsAdded;
        if (graphBuildLastMs > graphBuildMaxMs)
        {
            graphBuildMaxMs = graphBuildLastMs;
        }
    }
    bool due = graphDirty && (uint32_t)(nowMs - graphSceneMs) >= (uint32_t)CONFIG_GRAPH_REFRESH_MS;
    if (graphSceneForce || clockChanged || finished || due)
    {
        graphBuildScene(haveTime, nowCs);
        graphSceneForce = false;
        graphDirty = false;
        graphSceneMs = nowMs;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------
// Settings screen
// ---------------------------------------------------------------

void graphSetupStep(uint8_t row, int8_t delta)
{
    switch (row)
    {
        case GRAPH_SETUP_ROW_INTERVAL:
        {
            int next = (int)graphInterval + delta;
            if (next >= 0 && next < GRAPH_INTERVAL_COUNT)
            {
                graphSetInterval((uint8_t)next);
            }
            break;
        }
        case GRAPH_SETUP_ROW_YMODE:
        {
            int pos = 0;
            for (int k = 0; k < GRAPH_YMODE_COUNT; k++)
            {
                if (graphYModeOrder[k] == graphYMode)
                {
                    pos = k;
                }
            }
            graphSetYMode(graphYModeOrder[(pos + delta + GRAPH_YMODE_COUNT) % GRAPH_YMODE_COUNT]);
        }
            break;
        case GRAPH_SETUP_ROW_TRACE:
            graphSetTrace((uint8_t)(((int)graphTrace + delta + GRAPH_TRACE_COUNT) % GRAPH_TRACE_COUNT));
            break;
        case GRAPH_SETUP_ROW_SPAN:
        {
            int next = (int)graphSpan + delta;
            if (next >= 0 && next < GRAPH_SPAN_COUNT)
            {
                graphSetSpan((uint8_t)next);
            }
            break;
        }
        default:
            break;
    }
}

// v15: the few words of the settings screen in every language of
// help_text.h (HELP_LANG_*, SYSTem:LANGuage). Interval names, the Y mode
// names (AUTO/ZERO/SUPP) and the trace names stay the same in every language.
#define GRAPH_WORD_TITLE     0
#define GRAPH_WORD_ROW0      1 // GRAPH_WORD_ROW0 + GRAPH_SETUP_ROW_*: the line labels
#define GRAPH_WORD_KEYS      5
#define GRAPH_WORD_FULL      6
#define GRAPH_WORD_YMODE0    7 // GRAPH_WORD_YMODE0 + GRAPH_YMODE_*
#define GRAPH_WORD_DIGITS    11 // (v21: after the 4th Y mode)
#define GRAPH_WORD_COUNT     12
static const char *const graphSetupWords[HELP_LANG_COUNT][GRAPH_WORD_COUNT] = {
    {"GRAF - NASTAVENI", "INTERVAL", "OSA Y", "STOPA", "ROZPETI Y", "REL/dB: zmena  RCL: dalsi  STO: zpet",
     "FULL (CELY LOG)", "AUTO (MIN..MAX)", "ZERO (S NULOU)", "SUPP (POTL. NULA)", "ROBUST", "DIG."},
    {"GRAPH SETTINGS", "INTERVAL", "Y AXIS", "TRACE", "Y SPAN", "REL/dB: change  RCL: next  STO: back",
     "FULL (WHOLE LOG)", "AUTO (MIN..MAX)", "ZERO (WITH 0)", "SUPP (SUPPR. ZERO)", "ROBUST", "DIG."},
};

static const char *graphWord(uint8_t word)
{
    uint8_t language = settingsGetLanguage();
    if (language >= HELP_LANG_COUNT)
    {
        language = 0;
    }
    return graphSetupWords[language][word];
}

// Text of one setting's value for the settings screen.
static void graphSetupValue(uint8_t row, char *out, size_t size)
{
    switch (row)
    {
        case GRAPH_SETUP_ROW_INTERVAL:
            if (graphInterval == GRAPH_INTERVAL_FULL)
            {
                snprintf(out, size, "%s", graphWord(GRAPH_WORD_FULL));
            }
            else
            {
                snprintf(out, size, "%s", graphIntervalLabels[graphInterval]);
            }
            break;
        case GRAPH_SETUP_ROW_YMODE:
            snprintf(out, size, "%s", graphWord((uint8_t)(GRAPH_WORD_YMODE0 + graphYMode)));
            break;
        case GRAPH_SETUP_ROW_TRACE:
        {
            static const char *const texts[GRAPH_TRACE_COUNT] = {"MIN/MAX + AVG", "AVG", "MIN/MAX"};
            snprintf(out, size, "%s", texts[graphTrace]);
            break;
        }
        case GRAPH_SETUP_ROW_SPAN:
        {
            uint16_t counts = graphSpanCounts[graphSpan];
            if (graphLive.valid && graphLive.unit != KEITHLEY_UNIT_NONE)
            {
                char unitText[8];
                uiUnitText(graphLive.unit, unitText, sizeof(unitText));
                double value = (double)counts * pow(10.0, -(double)graphLive.decimals);
                snprintf(out, size, "+-%.*f %s", (int)graphLive.decimals, value, unitText);
            }
            else
            {
                snprintf(out, size, "+-%u %s", (unsigned)counts, graphWord(GRAPH_WORD_DIGITS));
            }
            break;
        }
        default:
            out[0] = '\0';
            break;
    }
}

void graphFillSetupScene(FbSetupScene *scene, uint8_t cursorRow)
{
    graphSyncFromSettings();
    memset(scene, 0, sizeof(*scene));
    snprintf(scene->title, sizeof(scene->title), "%s", graphWord(GRAPH_WORD_TITLE));
    for (uint8_t row = 0; row < GRAPH_SETUP_ROWS; row++)
    {
        snprintf(scene->labels[row], sizeof(scene->labels[row]), "%s", graphWord((uint8_t)(GRAPH_WORD_ROW0 + row)));
        graphSetupValue(row, scene->values[row], sizeof(scene->values[row]));
    }
    snprintf(scene->keys, sizeof(scene->keys), "%s", graphWord(GRAPH_WORD_KEYS));
    scene->cursorRow = (cursorRow < GRAPH_SETUP_ROWS) ? cursorRow : 0;
    scene->rowCount = GRAPH_SETUP_ROWS;
}

void graphDiagClear()
{
    graphBuildMaxMs = 0;
}
