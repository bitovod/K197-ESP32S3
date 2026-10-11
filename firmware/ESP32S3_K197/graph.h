// graph.h
// v14: the graph screen - the logged values of the current measurement
// function over a chosen time window, drawn as a MIN/MAX band with the
// average line in it, plus its settings screen. ui.ino owns the screens and
// the buttons and calls into this module; display.ino draws the scene this
// module prepares (fbComposeGraph() in framebuffer.h).
//
// Data model
// ----------
// The window (10 SEC ... 24 HRS, or FULL = the whole log) is divided into
// FB_GRAPH_COLUMNS (384) equal time slots, one per pixel column. Each slot
// keeps min, max, sum and count of the records that fell into it (24 bytes,
// about 9 kB in internal RAM in all) - so a column needs no further look at
// the log once a record has been added to it, and the median is
// deliberately not offered here (it cannot be accumulated like that).
//
// Records are read from the log by SERIAL number (datalog.h), in slices of
// at most CONFIG_GRAPH_SLICE_US per pass of the UI loop: right after the
// screen is opened or the window changed, from the first record of the
// window on (found by binary search over the RTC timestamps); after that
// only the new ones, as they arrive. Building the columns and following the
// live log are therefore the same code: a full log (~500k records) simply
// takes a few seconds of slices first, during which the partial graph is
// already shown and the header says "...". Leaving the graph screen keeps
// the columns; coming back only catches up with what was logged meanwhile.
//
// The fixed windows end at "now" (RTC) and scroll: once the time passes the
// right edge, the columns move left by whole slots. FULL starts at the
// oldest stored record and ends a "nice" span later (1.1 x the time the log
// covers, rounded up to 1/5/15/30 min, 1/2/3/6/12 h, 1..7 days); when the
// time reaches its end it is rebuilt with the next span. So the time of a
// column never changes while it is shown, only its place.
//
// Records without an RTC timestamp (made before the RTC synced) and OL
// records are not drawn as values; an OL record marks its column at the top
// edge. Records of other functions (the log may hold DCV and OHM ...) are
// skipped. Values are in base units (datalog.h), so a range change mV <-> V
// within one function is drawn correctly; the labels use the unit and the
// number of decimals of the live reading.
//
// Y axis modes
// ------------
//   AUTO  from the lowest to the highest value in the window (at least 4
//         counts of the last displayed digit high), grid on 1-2-5 values
//   ROBUST (v21) like AUTO, but the highest and the lowest
//         CONFIG_GRAPH_ROBUST_PERCENT % of the columns are left out of the
//         range (a single spike does not set the scale); they are clipped and
//         marked by a dot on the edge like in SUPP. With fewer than
//         CONFIG_GRAPH_ROBUST_MIN_COLUMNS columns it is plain AUTO.
//   ZERO  the axis always includes zero, both ends on grid values
//   SUPP  suppressed zero: a fixed centre +- a fixed span (counts of the
//         last displayed digit, 1-2-5 steps from +-1 to +-10000), plus a
//         reserve of CONFIG_GRAPH_SUPP_MARGIN_PERCENT on each side. The
//         centre is the newest value at the moment SUPP was chosen (RCL
//         click on the graph, or the settings screen); values outside are clipped and
//         marked by a dot on the edge. A change of the unit/decimals of the
//         live reading re-centres.
//
// Threading: everything runs in the display render/UI task (core 1), like
// ui.ino - including the scene, which display.ino reads right after
// uiPoll() in the same task (no copy, no lock).

#ifndef GRAPH_H
#define GRAPH_H

#include <Arduino.h>
#include "framebuffer.h" // FbGraphScene, FbSetupScene

#define GRAPH_INTERVAL_COUNT 9 // 10 SEC, 30 SEC (v15), 1 MIN, 5 MIN, 15 MIN, 1 HOUR, 6 HRS, 24 HRS, FULL
#define GRAPH_INTERVAL_FULL  (GRAPH_INTERVAL_COUNT - 1)

#define GRAPH_YMODE_AUTO  0
#define GRAPH_YMODE_ZERO  1
#define GRAPH_YMODE_SUPP  2
#define GRAPH_YMODE_ROBUST 3 // v21, appended (stored settings keep their meaning); shown after AUTO
#define GRAPH_YMODE_COUNT 4

#define GRAPH_TRACE_BOTH     0 // MIN/MAX band + average
#define GRAPH_TRACE_AVG      1
#define GRAPH_TRACE_ENVELOPE 2
#define GRAPH_TRACE_COUNT    3

#define GRAPH_SPAN_COUNT 13 // +-1 ... +-10000 counts
// (v21: +-1, +-2, +-5 added in front - GRAPH_SPANS_ADDED_V21 in settings.h)

// Lines of the settings screen that can be changed (the cursor rows).
#define GRAPH_SETUP_ROW_INTERVAL 0
#define GRAPH_SETUP_ROW_YMODE    1
#define GRAPH_SETUP_ROW_TRACE    2
#define GRAPH_SETUP_ROW_SPAN     3
#define GRAPH_SETUP_ROWS         4
static_assert(GRAPH_SETUP_ROWS == FB_SETUP_ROWS, "the settings screen has a line per setting");

// What the graph needs to know about the live reading (prepared by ui.ino
// from the newest DMM frame). Plain struct in a header (Arduino prototypes).
struct GraphLive
{
    bool valid;         // a reading with a measurement function is known
    char filter[8];     // function filter, "DCV", "OHM", ... (ui_format.h uiFunctionFilter())
    uint8_t unit;       // KEITHLEY_UNIT_* of the live reading (labels, SUPP span)
    uint8_t decimals;   // decimals of the live reading (uiDecimalsFor())
    char text[16];      // the live value as shown on the statistics screen ("-1.23456", "OL", "--.----")
};

// One time slot (pixel column) of the graph. In the header for the Arduino
// prototype reason, although only graph.ino uses it.
struct GraphColumn
{
    float minimum;
    float maximum;
    double sum;      // for the average (double: a slot of FULL can hold thousands of records)
    uint32_t count;  // records with a value
    uint8_t overload; // 1 = an OL record fell into this slot
    uint8_t marked;   // v18: 1 = a record with a marker (TAG) fell into this slot
};

// Numbers for the host tests and SYSTem:DEBUG.
struct GraphDebug
{
    bool built;            // columns exist for the current context
    bool busy;             // still reading older records
    uint32_t nextSerial;   // next log record to read
    uint32_t recordsAdded; // records that went into the columns since the last rebuild
    double startCs;        // window start, centiseconds since 2000
    double columnCs;       // length of one column, centiseconds
    uint32_t spanSeconds;  // length of the window
    double axisLow;        // Y axis bottom / top in display units (last scene)
    double axisHigh;
    uint32_t rebuilds;
    uint32_t lastBuildMs;      // v16: last (re)build, from the rebuild until the newest record was read
    uint32_t maxBuildMs;       // ... the longest since boot / SYSTem:DIAGnostic:CLEar
    uint32_t lastBuildRecords; // records that went into the columns during the last build
};

// Call once from uiBegin() (after settingsBegin()): loads the settings.
void graphBegin();

// The live reading for the next graphPoll() (cheap, same task).
void graphSetLive(const GraphLive *live);

// Called by ui.ino every pass of the UI loop while the graph screen is
// shown: scrolls, reads new records (one slice), rebuilds when the window or
// the function changed, refreshes the scene. Returns true when the picture
// needs to be redrawn.
bool graphPoll();

// True while older records are still being read (the UI loop then comes
// back after one tick instead of CONFIG_UI_POLL_MS).
bool graphBusy();

// The scene for fbComposeGraph() - valid until the next graphPoll().
const FbGraphScene &graphScene();

// Forgets all columns (e.g. after the log was cleared) - rebuilt on the next poll.
void graphInvalidate();

// Settings (each change persists deferred, settings.h). Out-of-range values are clamped.
uint8_t graphGetInterval();
void graphSetInterval(uint8_t index);
uint8_t graphGetYMode();
void graphSetYMode(uint8_t mode);
uint8_t graphGetTrace();
void graphSetTrace(uint8_t trace);
uint8_t graphGetSpan();
void graphSetSpan(uint8_t index);
const char *graphIntervalLabel(uint8_t index);
// v15 (SCPI GRAPh:*): window length of interval `index` in seconds (0 = FULL), SUPP span `index` in counts.
uint32_t graphIntervalSecondsAt(uint8_t index);
uint16_t graphSpanCountsAt(uint8_t index);

// SUPP mode, centred on the newest value (not bound to a key since the RCL
// click cycles the Y modes - kept for a future SCPI command).
void graphRecenter();

// The settings screen: one step of the value in line `row` (GRAPH_SETUP_ROW_*),
// delta -1 (REL) or +1 (dB); the interval and the span stop at their ends,
// the Y mode and the trace wrap around.
void graphSetupStep(uint8_t row, int8_t delta);

// Fills the settings screen (same 6-line layout as the help screen):
// a title, the four settings with the cursor on `cursorRow`, a key hint.
void graphFillSetupScene(FbSetupScene *scene, uint8_t cursorRow);

GraphDebug graphGetDebug();

// v16: SYSTem:DIAGnostic:CLEar - forgets the longest build time.
void graphDiagClear();

#endif // GRAPH_H
