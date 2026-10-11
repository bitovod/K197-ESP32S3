// Host test for the v14 graph screen: the real graph.ino on top of the real datalog.ino (serial
// numbers, binary search, ring buffer wrap), the axis modes, gaps, OL marks, scrolling with the clock,
// FULL, the settings screen text and the pixel drawing of fbComposeGraph(). With an output directory
// as the first argument it also writes the composed screens as PPM pictures (the preview images).
#include <Arduino.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <vector>
#include <string>
#include <math.h>
#include "config.h"
// v22: the long power-on settling is tested in test_settle.cpp; here it is the ordinary one
#undef CONFIG_DATALOG_SETTLE_POWERON_READINGS
#define CONFIG_DATALOG_SETTLE_POWERON_READINGS CONFIG_DATALOG_SETTLE_READINGS
#include "display.h"
#include "datalog.h"
#include "reading_math.h"
#include "ui_format.h"
#include "graph.h"
#include "framebuffer.h"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

// ---- stand-ins ----
static uint16_t gAutoInterval = 0;
uint16_t settingsGetAutoIntervalSeconds() { return gAutoInterval; }
bool settingsGetDebugEnabled() { return false; }
static uint8_t gGraphStored[4] = {CONFIG_GRAPH_DEFAULT_INTERVAL_INDEX, CONFIG_GRAPH_DEFAULT_YMODE, CONFIG_GRAPH_DEFAULT_TRACE, CONFIG_GRAPH_DEFAULT_SPAN_INDEX};
static int gGraphSaves = 0;
uint8_t settingsGetGraphInterval() { return gGraphStored[0]; }
uint8_t settingsGetGraphYMode() { return gGraphStored[1]; }
uint8_t settingsGetGraphTrace() { return gGraphStored[2]; }
uint8_t settingsGetGraphSpan() { return gGraphStored[3]; }
void settingsSetGraphDeferred(uint8_t a, uint8_t b, uint8_t c, uint8_t d) { gGraphStored[0] = a; gGraphStored[1] = b; gGraphStored[2] = c; gGraphStored[3] = d; gGraphSaves++; }
static uint8_t gLanguage = 0;
uint8_t settingsGetLanguage() { return gLanguage; }
void debugLogf(const char *, ...) {}
void debugLogAlways(const char *, ...) {}

#include "reading_math.ino"
// the tests size the log themselves (ESP.freePsram): no test cap of config.h
#undef CONFIG_DATALOG_TEST_CAPACITY
#define CONFIG_DATALOG_TEST_CAPACITY 0
#include "datalog.ino"
// no time budget: exactly one chunk of records per poll, so the slicing is exercised on a fast PC too
#undef CONFIG_GRAPH_SLICE_US
#define CONFIG_GRAPH_SLICE_US 0
#include "graph.ino"

// ---- test clock ----
static uint32_t gBase = 0;      // seconds since 2000 at gClockMs = 0
static uint64_t gClockMs = 0;
static void setClock()
{
    fakeRtcSeconds() = gBase + (uint32_t)(gClockMs / 1000);
    fakeRtcCentis() = (uint8_t)((gClockMs % 1000) / 10);
}
static void advance(uint64_t ms)
{
    gClockMs += ms;
    fakeMillisOffset() += (uint32_t)ms;
    setClock();
}

struct Ref { double t; double value; bool ol; std::string function; bool settle; };
// v20: the settling rule of datalog.ino (first CONFIG_DATALOG_SETTLE_READINGS readings after a function change)
static std::string gSettlePrefix;
static bool gSettleKnown = false;
static int gSettleCount = 0;
static std::vector<Ref> gRef; // every logged record, oldest first

// One reading: `counts` of the last digit with `dp` (digit index of the decimal point) in `unit`.
static void feed(long counts, int dp, uint8_t unit, uint64_t stepMs = 300, bool overload = false)
{
    advance(stepMs);
    char d[6];
    long m = labs(counts);
    char text[16];
    m %= 200000;
    if (m < 100000) snprintf(text, sizeof(text), " %05d", (int)m);
    else snprintf(text, sizeof(text), "%06d", (int)m);
    memcpy(d, text, 6);
    if (overload)
    {
        memcpy(d, "   L  ", 6);
    }
    uint16_t ind = (counts < 0) ? KEITHLEY_BIT_MINUS : 0;
    datalogUpdateReading(d, (int8_t)dp, ind, unit);
    Ref r;
    r.t = (double)fakeRtcSeconds() * 100.0 + fakeRtcCentis();
    r.ol = overload;
    double v = 0;
    if (!overload) keithleyReadingToDouble(d, (int8_t)dp, ind, unit, &v);
    r.value = (double)(float)v;
    char f[8];
    uiFunctionFilter(f, sizeof(f), keithleyFunctionPrefix(unit, ind));
    r.function = f;
    std::string prefix = keithleyFunctionPrefix(unit, ind);
    if (!gSettleKnown || prefix != gSettlePrefix) { gSettlePrefix = prefix; gSettleKnown = true; gSettleCount = 0; }
    r.settle = gSettleCount < CONFIG_DATALOG_SETTLE_READINGS;
    gSettleCount++;
    gRef.push_back(r);
}

static GraphLive liveFor(const char *filter, uint8_t unit, uint8_t decimals, const char *text)
{
    GraphLive l;
    memset(&l, 0, sizeof(l));
    l.valid = true;
    snprintf(l.filter, sizeof(l.filter), "%s", filter);
    l.unit = unit;
    l.decimals = decimals;
    snprintf(l.text, sizeof(l.text), "%s", text);
    return l;
}

static int pollUntilIdle(int maxPolls = 100000)
{
    int n = 0;
    graphPoll();
    while (graphBusy() && n < maxPolls)
    {
        graphPoll();
        n++;
    }
    return n;
}

// Columns as an independent model: bucket the reference records of `function` by time.
struct RefCol { double mn, mx, sum; uint32_t n; bool ol; };
static std::vector<RefCol> referenceColumns(const char *function)
{
    GraphDebug d = graphGetDebug();
    std::vector<RefCol> cols(FB_GRAPH_COLUMNS, RefCol{0, 0, 0, 0, false});
    for (const Ref &r : gRef)
    {
        if (r.function != function || r.settle) continue; // v20: settling readings are not drawn
        // the column borders are exact multiples of span / 384 - counted in integers like the firmware, since a
        // column of 10 SEC (2.604 cs) cannot be represented exactly as a double
        long long startU = llround(d.startCs * FB_GRAPH_COLUMNS);
        long long tU = llround(r.t) * FB_GRAPH_COLUMNS;
        if (tU < startU) continue;
        long long c = (tU - startU) / ((long long)d.spanSeconds * 100);
        if (c >= FB_GRAPH_COLUMNS) continue;
        RefCol &rc = cols[(size_t)c];
        if (r.ol) { rc.ol = true; continue; }
        if (rc.n == 0 || r.value < rc.mn) rc.mn = r.value;
        if (rc.n == 0 || r.value > rc.mx) rc.mx = r.value;
        rc.sum += r.value;
        rc.n++;
    }
    return cols;
}

static int compareColumns(const char *function)
{
    std::vector<RefCol> ref = referenceColumns(function);
    int bad = 0;
    for (int i = 0; i < FB_GRAPH_COLUMNS; i++)
    {
        const GraphColumn &c = graphCols[i];
        const RefCol &r = ref[i];
        bool ok = (c.count == r.n) && ((c.overload != 0) == r.ol);
        if (ok && r.n > 0)
        {
            ok = fabs(c.minimum - r.mn) < 1e-9 && fabs(c.maximum - r.mx) < 1e-9 && fabs(c.sum - r.sum) < 1e-9 * (1 + fabs(r.sum));
        }
        if (!ok)
        {
            if (bad < 3) printf("   column %d: count %u vs %u, ol %d vs %d\n", i, c.count, r.n, c.overload, r.ol);
            bad++;
        }
    }
    return bad;
}

// ---- pictures ----
static const char *gOutDir = NULL;
static void writePicture(const uint8_t *fb, const KeithleyTheme &theme, const char *name)
{
    if (gOutDir == NULL) return;
    uint16_t pal[FB_PALETTE_SIZE];
    fbBuildPalette(fbStatsTheme(theme), pal);
    std::string path = std::string(gOutDir) + "/" + name + ".ppm";
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", FB_WIDTH, FB_HEIGHT);
    for (int y = 0; y < FB_HEIGHT; y++)
        for (int x = 0; x < FB_WIDTH; x++)
        {
            uint8_t r, g, b;
            fbUnpack565(pal[fbGet(fb, x, y)], r, g, b);
            fputc(r, f); fputc(g, f); fputc(b, f);
        }
    fclose(f);
}
static uint8_t gFb[FB_BYTES];
static void composeGraph(const char *name, int theme = 1)
{
    fbComposeGraph(gFb, graphScene());
    writePicture(gFb, themes[theme], name);
}
static void composeSetup(const char *name, uint8_t row, int theme = 1)
{
    FbSetupScene hs;
    graphFillSetupScene(&hs, row);
    fbComposeSetup(gFb, hs);
    writePicture(gFb, themes[theme], name);
}

// A slowly drifting DCV reading in the 2 V range around -1.23400 V with noise and a step,
// counts of the last digit (dp = 0 -> 1.23456 V, 5 decimals).
static long gNoise = 12345;
static long noisy(long center, long amplitude)
{
    gNoise = (gNoise * 1103515245L + 12345L) & 0x7FFFFFFF;
    return center + (long)(gNoise % (2 * amplitude + 1)) - amplitude;
}

int main(int argc, char **argv)
{
    if (argc > 1) gOutDir = argv[1];
    ESP.freePsram = (size_t)(200000.0 * sizeof(LogRecord)) + DATALOG_PSRAM_RESERVE_BYTES + 4;
    datalogBegin();
    gBase = 845208000; // 2026-10-13 10:00:00 (seconds since 2000)
    setClock();
    graphBegin();

    // ---- 1. pixel font: 97 glyphs, plain zero, digits within 7 rows ----
    {
        const uint8_t *zero = font_pixel_5x7 + fbGlyphIndex('0') * FONT_PIXEL_GLYPH_BYTES;
        CHECK(zero[1] == 0x41 && zero[2] == 0x41 && zero[3] == 0x41, "pixel font: zero without the slash");
        int tall = 0;
        for (char c = '0'; c <= '9'; c++)
            for (int k = 0; k < 5; k++)
                if (font_pixel_5x7[fbGlyphIndex(c) * 5 + k] & 0x80) tall++;
        CHECK(tall == 0, "pixel font: digits use 7 rows");
        CHECK(fbPixelTextWidth("-1.234") == 35, "pixel text width: 6 per glyph minus the last gap");
        printf("1. pixel font ok\n");
    }

    // ---- 2. no RTC time / no reading ----
    {
        fakeRtcSeconds() = 0;
        GraphLive l = liveFor("DCV", KEITHLEY_UNIT_V, 5, "-1.23400");
        graphSetLive(&l);
        graphPoll();
        CHECK(strcmp(graphScene().message, "NO RTC TIME") == 0, "no RTC: message");
        setClock();
        GraphLive none;
        memset(&none, 0, sizeof(none));
        graphSetLive(&none);
        graphPoll();
        CHECK(strcmp(graphScene().message, "NO READING") == 0, "no live reading: message");
        printf("2. messages ok\n");
    }

    // ---- 3. two hours of DCV (with some OHM in between), 1 HOUR window: build, compare with the model ----
    for (int i = 0; i < 24000; i++)
    {
        long c = noisy(-123400 + (i > 15000 ? 300 : 0) + (long)(200 * sin(i / 900.0)), 40);
        if (i % 500 == 7) feed(170123, 2, KEITHLEY_UNIT_KOHM); // another function in between
        else if (i % 4000 == 100) feed(0, 0, KEITHLEY_UNIT_V, 300, true); // an OL reading
        else feed(c, 0, KEITHLEY_UNIT_V);
    }
    printf("3. log: %lu records over %.0f s\n", (unsigned long)datalogCount(), (double)gClockMs / 1000.0);
    GraphLive dcv = liveFor("DCV", KEITHLEY_UNIT_V, 5, "-1.23100");
    graphSetLive(&dcv);
    graphSetInterval(5); // 1 HOUR
    {
        int polls = pollUntilIdle();
        GraphDebug d = graphGetDebug();
        CHECK(d.built && !d.busy, "1 HOUR built");
        CHECK(d.spanSeconds == 3600, "1 HOUR = 3600 s");
        CHECK(d.lastBuildRecords == d.recordsAdded && d.maxBuildMs >= d.lastBuildMs, "v16: build timing recorded at the end of the build");
        graphDiagClear();
        CHECK(graphGetDebug().maxBuildMs == 0, "v16: SYSTem:DIAGnostic:CLEar zeroes the longest build");
        double now = (double)fakeRtcSeconds() * 100 + fakeRtcCentis();
        CHECK(now >= d.startCs + (FB_GRAPH_COLUMNS - 1) * d.columnCs && now < d.startCs + FB_GRAPH_COLUMNS * d.columnCs, "now lies in the last column");
        CHECK(fabs(fmod(d.startCs, d.columnCs)) < 1e-6 || fabs(fmod(d.startCs, d.columnCs) - d.columnCs) < 1e-6, "column borders on multiples of the column length");
        int bad = compareColumns("DCV");
        CHECK(bad == 0, "1 HOUR columns equal the independent model");
        int ol = 0, data = 0;
        for (int i = 0; i < FB_GRAPH_COLUMNS; i++) { if (graphScene().cols[i].flags & FB_GRAPH_COL_OL) ol++; if (graphScene().cols[i].flags & FB_GRAPH_COL_DATA) data++; }
        CHECK(ol >= 2, "OL readings marked");
        CHECK(data >= 380, "almost every column has values");
        printf("3. 1 HOUR: %d polls, %lu records added, %d columns with data, %d OL marks, axis %.5f .. %.5f V\n", polls,
               (unsigned long)d.recordsAdded, data, ol, d.axisLow, d.axisHigh);
        // AUTO: axis = min..max of the band, in V
        double mn = 1e9, mx = -1e9;
        for (int i = 0; i < FB_GRAPH_COLUMNS; i++) if (graphCols[i].count) { mn = std::min(mn, (double)graphCols[i].minimum); mx = std::max(mx, (double)graphCols[i].maximum); }
        CHECK(fabs(d.axisLow - mn) < 1e-9 && fabs(d.axisHigh - mx) < 1e-9, "AUTO: axis from the lowest to the highest value");
        CHECK(graphScene().yTickCount >= 2 && graphScene().yTickCount <= FB_GRAPH_MAX_YTICKS, "AUTO: grid lines");
        CHECK(graphScene().xTickCount == 7 && strcmp(graphScene().xTicks[6].label, "0") == 0 && strcmp(graphScene().xTicks[0].label, "-60m") == 0,
              "1 HOUR: time grid every 10 minutes, -60m .. 0");
        CHECK(strcmp(graphScene().headLeft, "DCV V") == 0 && strcmp(graphScene().headMode, "AUTO") == 0, "header");
        for (int i = 0; i < graphScene().yTickCount; i++) CHECK((int)strlen(graphScene().yTicks[i].label) <= FB_GRAPH_YLABEL_CHARS, "Y labels fit");
        composeGraph("graph_auto_1hour");
    }

    // ---- 4. live: new readings go into the right edge, the window scrolls with the clock ----
    {
        GraphDebug before = graphGetDebug();
        for (int i = 0; i < 200; i++) feed(noisy(-123100, 40), 0, KEITHLEY_UNIT_V);
        pollUntilIdle();
        GraphDebug after = graphGetDebug();
        CHECK(after.rebuilds == before.rebuilds, "no rebuild while following");
        CHECK(after.startCs > before.startCs + 5000, "window scrolled with the clock");
        CHECK(compareColumns("DCV") == 0, "columns after scrolling equal the model");
        printf("4. live: start moved by %.1f s, %lu records added in all\n", (after.startCs - before.startCs) / 100.0, (unsigned long)after.recordsAdded);
    }

    // ---- 5. gap: the DMM is silent for 2 minutes -> no line across, joined again after it ----
    {
        advance(120000);
        for (int i = 0; i < 100; i++) feed(noisy(-123100, 40), 0, KEITHLEY_UNIT_V);
        pollUntilIdle();
        const FbGraphScene &sc = graphScene();
        int gapStart = -1, gapLen = 0, run = 0;
        for (int i = 0; i < FB_GRAPH_COLUMNS; i++)
        {
            if (sc.cols[i].flags & FB_GRAPH_COL_DATA) { if (run > gapLen) { gapLen = run; gapStart = i - run; } run = 0; }
            else run++;
        }
        bool connectedAcross = gapStart >= 0 && (sc.cols[gapStart + gapLen].flags & FB_GRAPH_COL_CONNECT);
        CHECK(gapLen >= 12 && !connectedAcross, "a 2 minute gap stays open (columns of 9.4 s)");
        int joined = 0, dataCols = 0;
        for (int i = 1; i < FB_GRAPH_COLUMNS; i++) if ((sc.cols[i].flags & FB_GRAPH_COL_DATA) && (sc.cols[i - 1].flags & FB_GRAPH_COL_DATA)) { dataCols++; if (sc.cols[i].flags & FB_GRAPH_COL_CONNECT) joined++; }
        CHECK(joined == dataCols, "neighbouring columns are joined");
        printf("5. gap of %d columns at column %d, not joined\n", gapLen, gapStart);
        composeGraph("graph_gap");
    }

    // ---- 6. ZERO and SUPP ----
    {
        graphSetYMode(GRAPH_YMODE_ZERO);
        graphPoll();
        GraphDebug d = graphGetDebug();
        CHECK(d.axisHigh == 0.0 && d.axisLow < -1.2, "ZERO: negative values -> axis ..0");
        composeGraph("graph_zero");
        graphSetYMode(GRAPH_YMODE_SUPP);
        graphSetSpan(3 + GRAPH_SPANS_ADDED_V21); // +-100 counts = +-0.00100 V
        graphPoll();
        d = graphGetDebug();
        double mid = (d.axisLow + d.axisHigh) / 2;
        CHECK(fabs((d.axisHigh - d.axisLow) - 0.002 * (1.0 + CONFIG_GRAPH_SUPP_MARGIN_PERCENT / 100.0)) < 1e-9, "SUPP: +-100 counts of 10 uV + the reserve on each side");
        CHECK(fabs(mid - (-1.23100)) < 0.0003, "SUPP: centred on the newest value");
        int clipped = 0;
        for (int i = 0; i < FB_GRAPH_COLUMNS; i++) if (graphScene().cols[i].flags & (FB_GRAPH_COL_CLIP_HI | FB_GRAPH_COL_CLIP_LO)) clipped++;
        CHECK(clipped > 0, "SUPP: values outside are marked");
        // v21: +-1 count is drawn as +-1 count (not widened to the AUTO minimum of 4 counts)
        graphSetSpan(0);
        graphPoll();
        d = graphGetDebug();
        CHECK(fabs((d.axisHigh - d.axisLow) - 0.00002 * (1.0 + CONFIG_GRAPH_SUPP_MARGIN_PERCENT / 100.0)) < 1e-12, "SUPP: +-1 count of 10 uV");
        graphSetSpan(3 + GRAPH_SPANS_ADDED_V21);
        graphPoll();
        printf("6. ZERO axis %.4f..%.4f V; SUPP axis %.5f..%.5f V, %d columns clipped\n", -1.3, 0.0, d.axisLow, d.axisHigh, clipped);
        composeGraph("graph_supp");
        graphSetSpan(6 + GRAPH_SPANS_ADDED_V21); // +-1000 counts
        graphPoll();
        composeGraph("graph_supp_wide");
        graphSetTrace(GRAPH_TRACE_AVG);
        graphPoll();
        composeGraph("graph_supp_wide_avg");
        graphSetTrace(GRAPH_TRACE_BOTH);
        graphSetYMode(GRAPH_YMODE_AUTO);
        graphPoll();
    }

    // ---- 7. other intervals: 1 MIN (columns of 0.16 s, most of them empty but joined), 24 HRS, FULL ----
    {
        for (int i = 0; i < 300; i++) feed(noisy(-123100, 40), 0, KEITHLEY_UNIT_V);
        graphSetInterval(2); // 1 MIN
        pollUntilIdle();
        CHECK(compareColumns("DCV") == 0, "1 MIN columns equal the model");
        const FbGraphScene &sc = graphScene();
        int data = 0, unjoined = 0, prev = -1;
        for (int i = 0; i < FB_GRAPH_COLUMNS; i++) if (sc.cols[i].flags & FB_GRAPH_COL_DATA) { data++; if (prev >= 0 && !(sc.cols[i].flags & FB_GRAPH_COL_CONNECT)) unjoined++; prev = i; }
        CHECK(data > 150 && unjoined == 0, "1 MIN: sparse columns, all joined");
        CHECK(strcmp(sc.xTicks[0].label, "-60s") == 0, "1 MIN: grid every 10 s");
        composeGraph("graph_1min");

        // v15: 10 SEC and 30 SEC - a handful of readings, every one its own point, all joined; grid every 2 / 5 s
        graphSetInterval(0);
        pollUntilIdle();
        CHECK(compareColumns("DCV") == 0, "10 SEC columns equal the model");
        {
            const FbGraphScene &s10 = graphScene();
            int pts = 0, unjoined10 = 0, prev10 = -1;
            for (int i = 0; i < FB_GRAPH_COLUMNS; i++) if (s10.cols[i].flags & FB_GRAPH_COL_DATA) { pts++; if (prev10 >= 0 && !(s10.cols[i].flags & FB_GRAPH_COL_CONNECT)) unjoined10++; prev10 = i; }
            CHECK(pts >= 30 && pts <= 35 && unjoined10 == 0, "10 SEC: ~33 readings, joined");
            CHECK(s10.yTickCount >= 4, "10 SEC: a full Y grid (float range just over a 1-2-5 step)");
            CHECK(strcmp(s10.headInterval, "10 SEC") == 0 && s10.xTickCount == 6 && strcmp(s10.xTicks[0].label, "-10s") == 0, "10 SEC: grid every 2 s");
            printf("7. 10 SEC: %d readings drawn, axis %.6f..%.6f, Y grid:", pts, graphGetDebug().axisLow, graphGetDebug().axisHigh);
            for (int i = 0; i < s10.yTickCount; i++) printf(" %d:'%s'", s10.yTicks[i].pos, s10.yTicks[i].label);
            printf("\n");
        }
        composeGraph("graph_10sec");
        graphSetInterval(1);
        pollUntilIdle();
        CHECK(compareColumns("DCV") == 0 && strcmp(graphScene().xTicks[0].label, "-30s") == 0 && graphScene().xTickCount == 7, "30 SEC: grid every 5 s");
        composeGraph("graph_30sec");

        graphSetInterval(7); // 24 HRS: the log covers ~2 h, the left part is empty
        pollUntilIdle();
        CHECK(compareColumns("DCV") == 0, "24 HRS columns equal the model");
        CHECK(strcmp(graphScene().xTicks[0].label, "-24h") == 0, "24 HRS: grid every 6 h");

        graphSetInterval(GRAPH_INTERVAL_FULL);
        int polls = pollUntilIdle();
        GraphDebug d = graphGetDebug();
        double covered = ((double)fakeRtcSeconds() * 100 - gRef[0].t) / 100.0;
        CHECK(d.spanSeconds >= covered * 1.1 && d.spanSeconds == 10800, "FULL: 1.1 x the covered time, rounded up (3 h)");
        CHECK(compareColumns("DCV") == 0, "FULL columns equal the model");
        printf("7. FULL: log covers %.0f s, window %lu s, %d polls\n", covered, (unsigned long)d.spanSeconds, polls);
        composeGraph("graph_full");
        // time runs past the end of the FULL window -> rebuilt with the next span
        uint32_t rebuilds = d.rebuilds;
        advance((uint64_t)(d.spanSeconds * 1000.0 - covered * 1000.0) + 5000);
        feed(-123000, 0, KEITHLEY_UNIT_V);
        pollUntilIdle();
        d = graphGetDebug();
        CHECK(d.rebuilds == rebuilds + 1 && d.spanSeconds == 21600, "FULL: rebuilt with 6 h when the time reached the end");
        CHECK(compareColumns("DCV") == 0, "FULL columns after the rebuild equal the model");
    }

    // ---- 8. ring buffer wrap: serial numbers keep their place while the oldest records are overwritten ----
    {
        graphSetInterval(3); // 5 MIN
        pollUntilIdle();
        uint32_t cap = datalogCapacity();
        uint32_t extra = cap - datalogCount() + 5000; // fill up and overwrite 5000
        for (uint32_t i = 0; i < extra; i++) feed(noisy(-123000, 40), 0, KEITHLEY_UNIT_V, 100);
        // gRef no longer mirrors the log at its start, but the model only looks at the window
        pollUntilIdle();
        CHECK(datalogCount() == cap, "log full");
        CHECK(compareColumns("DCV") == 0, "5 MIN columns after the wrap equal the model");
        printf("8. after wrap: capacity %lu, written %lu\n", (unsigned long)cap, (unsigned long)datalogEndSerial(NULL));
        // v16: LOG:MEDian? over the whole (full) log when PSRAM has no block for the full scratch buffer
        Stream sink;
        size_t limit = 60000 * sizeof(float); // less than DATALOG_MEDIAN_SAMPLE_CAP floats
        fakePsMallocLimit() = limit;
        fakePsMallocLastOk() = 0;
        datalogPrintMedian(sink, "", 0, 0);
        fakePsMallocLimit() = (size_t)-1;
        CHECK(fakePsMallocLastOk() > 0 && fakePsMallocLastOk() <= limit, "median: coarser sampling instead of giving up");
        printf("8. median with a %lu B PSRAM block: scratch %lu B\n", (unsigned long)limit, (unsigned long)fakePsMallocLastOk());
    }

    // ---- 9. log cleared -> rebuilt, NO DATA ----
    {
        datalogClear();
        graphPoll();
        CHECK(strcmp(graphScene().message, "NO DATA") == 0, "cleared log: NO DATA");
        feed(-123000, 0, KEITHLEY_UNIT_V);
        pollUntilIdle();
        GraphDebug d = graphGetDebug();
        CHECK(d.recordsAdded == 1, "after the clear the graph starts with the new record");
        gRef.clear();
    }

    // ---- 10. function change: OHM live -> only OHM records ----
    {
        for (int i = 0; i < 300; i++) feed(i % 2 ? 170123 + (long)(80 * sin(i / 40.0)) : noisy(-123000, 40), i % 2 ? 2 : 0, i % 2 ? KEITHLEY_UNIT_KOHM : KEITHLEY_UNIT_V);
        GraphLive ohm = liveFor("OHM", KEITHLEY_UNIT_KOHM, 3, "170.423");
        graphSetLive(&ohm);
        pollUntilIdle();
        CHECK(compareColumns("OHM") == 0, "OHM columns only hold OHM records");
        CHECK(strcmp(graphScene().headLeft, "OHM k\002") == 0, "OHM header");
        composeGraph("graph_ohm_5min");
        graphSetLive(&dcv);
        pollUntilIdle();
    }

    // ---- 11. settings screen ----
    {
        graphSetInterval(5);
        graphSetYMode(GRAPH_YMODE_AUTO);
        int saves = gGraphSaves;
        graphSetupStep(GRAPH_SETUP_ROW_YMODE, -1);
        CHECK(graphGetYMode() == GRAPH_YMODE_SUPP, "Y mode wraps around");
        graphSetupStep(GRAPH_SETUP_ROW_YMODE, +1);
        CHECK(graphGetYMode() == GRAPH_YMODE_AUTO, "Y mode back");
        graphSetupStep(GRAPH_SETUP_ROW_INTERVAL, +1);
        graphSetupStep(GRAPH_SETUP_ROW_INTERVAL, +1);
        graphSetupStep(GRAPH_SETUP_ROW_INTERVAL, +1);
        graphSetupStep(GRAPH_SETUP_ROW_INTERVAL, +1);
        CHECK(graphGetInterval() == GRAPH_INTERVAL_FULL, "interval stops at FULL");
        graphSetupStep(GRAPH_SETUP_ROW_INTERVAL, -1);
        graphSetupStep(GRAPH_SETUP_ROW_INTERVAL, -1);
        graphSetupStep(GRAPH_SETUP_ROW_INTERVAL, -1);
        graphSetupStep(GRAPH_SETUP_ROW_SPAN, -1);
        CHECK(graphGetInterval() == 5 && graphGetSpan() == 5 + GRAPH_SPANS_ADDED_V21, "steps");
        CHECK(gGraphSaves > saves && gGraphStored[0] == 5 && gGraphStored[3] == 5 + GRAPH_SPANS_ADDED_V21, "settings persisted");
        FbSetupScene hs;
        graphFillSetupScene(&hs, GRAPH_SETUP_ROW_YMODE);
        CHECK(hs.cursorRow == GRAPH_SETUP_ROW_YMODE && strcmp(hs.labels[1], "OSA Y") == 0 && strcmp(hs.values[1], "AUTO (MIN..MAX)") == 0, "cursor line");
        CHECK(strcmp(hs.labels[0], "INTERVAL") == 0 && strcmp(hs.values[0], "1 HOUR") == 0, "other line");
        graphFillSetupScene(&hs, GRAPH_SETUP_ROW_SPAN);
        CHECK(strcmp(hs.values[3], "+-0.00500 V") == 0, "span in the unit of the live reading");
        // every value of every setting in every language fits the field between the triangles
        int tooLong = 0;
        for (int lang = 0; lang < HELP_LANG_COUNT; lang++)
        {
            gLanguage = (uint8_t)lang;
            uint8_t keep[4] = {graphGetInterval(), graphGetYMode(), graphGetTrace(), graphGetSpan()};
            for (int iv = 0; iv < GRAPH_INTERVAL_COUNT; iv++)
                for (int ym = 0; ym < GRAPH_YMODE_COUNT; ym++)
                    for (int tr = 0; tr < GRAPH_TRACE_COUNT; tr++)
                    {
                        graphSetInterval((uint8_t)iv); graphSetYMode((uint8_t)ym); graphSetTrace((uint8_t)tr);
                        for (int sp = 0; sp < GRAPH_SPAN_COUNT; sp += 3)
                        {
                            graphSetSpan((uint8_t)sp);
                            graphFillSetupScene(&hs, 0);
                            for (int r = 0; r < FB_SETUP_ROWS; r++)
                            {
                                if (strlen(hs.values[r]) > FB_SETUP_FIELD_CHARS) { if (tooLong < 3) printf("   too long: '%s'\n", hs.values[r]); tooLong++; }
                                if ((int)strlen(hs.labels[r]) > FB_SETUP_VALUE_COL - FB_SETUP_LABEL_COL - 1) tooLong++;
                            }
                            if (strlen(hs.keys) > 47 || strlen(hs.title) > 47) tooLong++;
                        }
                    }
            graphSetInterval(keep[0]); graphSetYMode(keep[1]); graphSetTrace(keep[2]); graphSetSpan(keep[3]);
        }
        gLanguage = HELP_LANG_CZ;
        CHECK(tooLong == 0, "settings values fit the field, labels the label column");
        composeSetup("graph_setup", GRAPH_SETUP_ROW_YMODE);
        // the triangles: cursor at the line start, value framed at fixed places
        {
            int16_t row = (int16_t)(fbHelpBaseline(1 + GRAPH_SETUP_ROW_YMODE) - FONT_TINY.digitHeight / 2);
            int16_t valueX = FB_SETUP_VALUE_COL * FONT_TINY.advance;
            uint8_t ink = fbRampIndex(FB_INK_INDICATOR, 255);
            CHECK(fbGet(gFb, 2, row) == ink, "cursor triangle at the line start");
            CHECK(fbGet(gFb, valueX - FB_SETUP_ARROW_GAP - 1, row) == ink, "left triangle before the value");
            CHECK(fbGet(gFb, valueX + (int)strlen("AUTO (MIN..MAX)") * FONT_TINY.advance + FB_SETUP_ARROW_GAP + 1, row) == ink, "right triangle right after the value");
            int16_t other = (int16_t)(fbHelpBaseline(1 + GRAPH_SETUP_ROW_INTERVAL) - FONT_TINY.digitHeight / 2);
            CHECK(fbGet(gFb, 2, other) == 0 && fbGet(gFb, valueX - FB_SETUP_ARROW_GAP - 1, other) == 0, "no triangles on the other lines");
        }
        // v15: the key hint is flush right
        {
            int16_t base = fbHelpBaseline(FB_HELP_LINES - 1);
            int leftInk = 0, rightInk = 0;
            for (int16_t y = (int16_t)(base - FONT_TINY.digitHeight); y <= base; y++)
            {
                for (int16_t x = 0; x < 60; x++) if (fbGet(gFb, x, y) != 0) leftInk++;
                for (int16_t x = FB_WIDTH - 30; x < FB_WIDTH; x++) if (fbGet(gFb, x, y) != 0) rightInk++;
            }
            CHECK(leftInk == 0 && rightInk > 0, "key hint flush right");
            // v15: no separator line above the key hint any more (dropped after trying it on the panel)
            int16_t lineY = fbHelpUnderlineY(FB_HELP_LINES - 2);
            int lineInk = 0;
            for (int x = 0; x < 60; x++) if (fbGet(gFb, x, lineY) != 0) lineInk++;
            CHECK(lineInk == 0, "no line above the key hint");
            composeSetup("graph_setup", GRAPH_SETUP_ROW_YMODE);
        }
        // v15: SCPI GRAPh:* writes the settings directly; the graph adopts them on its next pass
        {
            uint8_t keepIv = graphGetInterval(), keepMode = graphGetYMode(), keepTrace = graphGetTrace(), keepSpan = graphGetSpan();
            gGraphStored[0] = 1; gGraphStored[1] = GRAPH_YMODE_ZERO; gGraphStored[2] = GRAPH_TRACE_AVG; gGraphStored[3] = 7;
            int saves = gGraphSaves;
            graphPoll();
            CHECK(graphGetInterval() == 1 && graphGetYMode() == GRAPH_YMODE_ZERO && graphGetTrace() == GRAPH_TRACE_AVG && graphGetSpan() == 7,
                  "SCPI settings adopted by the graph");
            CHECK(gGraphSaves == saves, "adopting does not save again");
            gGraphStored[0] = 200; // out of range: ignored
            graphPoll();
            CHECK(graphGetInterval() == 1, "invalid stored value ignored");
            graphSetInterval(keepIv); graphSetYMode(keepMode); graphSetTrace(keepTrace); graphSetSpan(keepSpan);
            pollUntilIdle();
        }
        gLanguage = HELP_LANG_EN; // v15
        graphFillSetupScene(&hs, GRAPH_SETUP_ROW_YMODE);
        CHECK(strcmp(hs.title, "GRAPH SETTINGS") == 0 && strcmp(hs.labels[1], "Y AXIS") == 0, "EN settings screen");
        composeSetup("graph_setup_en", GRAPH_SETUP_ROW_YMODE);
        gLanguage = HELP_LANG_EN + 7; // out of range -> the first language
        graphFillSetupScene(&hs, GRAPH_SETUP_ROW_YMODE);
        CHECK(strcmp(hs.title, "GRAF - NASTAVENI") == 0, "unknown language falls back to CZ");
        gLanguage = HELP_LANG_CZ;
    }

    // ---- 12. drawing: band, line, grid and labels land where the scene says ----
    {
        graphSetSpan(3);
        for (int i = 0; i < 3000; i++) feed(noisy(-123400 + (long)(150 * sin(i / 400.0)), 30), 0, KEITHLEY_UNIT_V);
        pollUntilIdle();
        graphPoll();
        const FbGraphScene &sc = graphScene();
        fbComposeGraph(gFb, sc);
        int wrong = 0;
        uint8_t line = fbRampIndex(FB_INK_BRIGHT, 255);
        for (int i = 0; i < FB_GRAPH_COLUMNS; i++)
        {
            if (!(sc.cols[i].flags & FB_GRAPH_COL_DATA)) continue;
            if (fbGet(gFb, FB_GRAPH_X0 + 1 + i, sc.cols[i].yAvg) != line) wrong++;
        }
        CHECK(wrong == 0, "the average point of every column is drawn");
        CHECK(fbGet(gFb, FB_GRAPH_X0, 40) == fbGraphAxisIndex() && fbGet(gFb, 200, FB_GRAPH_AXIS_Y) == fbGraphAxisIndex(), "axes");
        bool labelInk = false;
        for (int y = 0; y < 7; y++) for (int x = 0; x < 30; x++) if (fbGet(gFb, x, y) != 0) labelInk = true;
        CHECK(labelInk, "header drawn");
        composeGraph("graph_auto_1hour_b");
        for (int t = 0; t < 8; t++)
        {
            char name[32];
            snprintf(name, sizeof(name), "graph_theme_%s", themes[t].name);
            composeGraph(name, t);
        }
    }

    // ---- 13. speed: reading a full log (as on the instrument: ~520k records) - here 200k ----
    {
        while (datalogCount() < datalogCapacity()) feed(noisy(-123000, 40), 0, KEITHLEY_UNIT_V, 20);
        graphSetInterval(GRAPH_INTERVAL_FULL);
        unsigned long t0 = micros();
        int polls = pollUntilIdle();
        unsigned long t1 = micros();
        printf("13. FULL over %lu records: %d slices, %.1f ms on this PC\n", (unsigned long)datalogCount(), polls, (t1 - t0) / 1000.0);
        CHECK(!graphBusy(), "FULL finished");
    }

    graphSetYMode(GRAPH_YMODE_AUTO);
    graphSetInterval(5); // 1 HOUR
    pollUntilIdle();
    // ---- 14. v18 markers: a marked record marks its column - set before the graph read it (the bit is in the
    //      record), after it (datalogMarkSerialAt() path), and both survive a rebuild ----
    {
        auto markedColumns = []() { int n = 0; for (int i = 0; i < FB_GRAPH_COLUMNS; i++) if (graphCols[i].marked) n++; return n; };
        int marks0 = markedColumns();
        for (int i = 0; i < 60; i++) feed(noisy(-123100, 40), 0, KEITHLEY_UNIT_V);
        CHECK(datalogMarkAtMs((uint32_t)millis(), NULL), "marker on a record the graph has not read yet");
        for (int i = 0; i < 300; i++) feed(noisy(-123100, 40), 0, KEITHLEY_UNIT_V);
        pollUntilIdle();
        CHECK(markedColumns() == marks0 + 1, "its column is marked");
        uint32_t serial = 0;
        CHECK(datalogMarkAtMs((uint32_t)millis(), &serial), "marker on the newest record, already read by the graph");
        CHECK(serial < graphGetDebug().nextSerial, "(the graph had read it)");
        graphPoll();
        pollUntilIdle();
        CHECK(markedColumns() == marks0 + 2, "late marker: its column is marked too");
        int col = -1;
        for (int i = FB_GRAPH_COLUMNS - 1; i >= 0; i--) if (graphCols[i].marked) { col = i; break; }
        CHECK(col >= FB_GRAPH_COLUMNS - 2, "the newest marker sits at the right edge");
        graphInvalidate();
        pollUntilIdle();
        CHECK(markedColumns() == marks0 + 2, "markers survive a rebuild (they are in the records)");
        int sceneMarks = 0;
        for (int i = 0; i < FB_GRAPH_COLUMNS; i++) if (graphScene().cols[i].flags & FB_GRAPH_COL_MARK) sceneMarks++;
        CHECK(sceneMarks == marks0 + 2, "the scene shows both");
        composeGraph("graph_markers");
        composeGraph("graph_markers_LCD1", 5);
        // a picture for the eye: 5 MIN of a slow sine with three markers
        graphSetInterval(3);
        for (int i = 0; i < 1000; i++)
        {
            feed(noisy(-123100 + (long)(60 * sin(i / 70.0)), 6), 0, KEITHLEY_UNIT_V);
            if (i == 300 || i == 620 || i == 905) datalogMarkAtMs((uint32_t)millis(), NULL);
        }
        pollUntilIdle();
        graphInvalidate();
        pollUntilIdle();
        composeGraph("graph_markers_5min");
        composeGraph("graph_markers_5min_LCD1", 5);
        printf("14. markers: in the record, late, after a rebuild - %d columns marked\n", markedColumns());
    }

    // ---- 15. v21: ROBUST - one short spike does not set the scale ----
    {
        GraphLive l = liveFor("DCI", KEITHLEY_UNIT_UA, 3, "0.293");
        graphSetLive(&l);
        graphSetInterval(5); // 1 HOUR
        graphSetTrace(GRAPH_TRACE_BOTH);
        // 10 readings: fewer columns than CONFIG_GRAPH_ROBUST_MIN_COLUMNS -> ROBUST = AUTO
        for (int i = 0; i < 10; i++) feed(i == 9 ? 363 : noisy(293, 1), 2, KEITHLEY_UNIT_UA, 9000);
        graphSetYMode(GRAPH_YMODE_AUTO);
        pollUntilIdle();
        GraphDebug a = graphGetDebug();
        graphSetYMode(GRAPH_YMODE_ROBUST);
        pollUntilIdle();
        GraphDebug r = graphGetDebug();
        CHECK(a.axisLow == r.axisLow && a.axisHigh == r.axisHigh && r.axisHigh > 0.36, "ROBUST: few columns = AUTO");
        // one hour, a spike of 6 readings in the middle
        for (int i = 0; i < 11000; i++)
        {
            long c = noisy(293, 1);
            if (i >= 6000 && i < 6006) c = 280 + (i - 6000) * 17;
            feed(c, 2, KEITHLEY_UNIT_UA, 320);
        }
        graphSetYMode(GRAPH_YMODE_AUTO);
        pollUntilIdle();
        a = graphGetDebug();
        CHECK(a.axisHigh >= 0.365 && a.axisLow <= 0.280 + 1e-9, "AUTO: the spike sets the scale");
        graphSetYMode(GRAPH_YMODE_ROBUST);
        pollUntilIdle();
        r = graphGetDebug();
        int hi = 0, lo = 0;
        for (int i = 0; i < FB_GRAPH_COLUMNS; i++)
        {
            if (graphScene().cols[i].flags & FB_GRAPH_COL_CLIP_HI) hi++;
            if (graphScene().cols[i].flags & FB_GRAPH_COL_CLIP_LO) lo++;
        }
        CHECK(r.axisLow >= 0.290 - 1e-9 && r.axisHigh <= 0.296 + 1e-9, "ROBUST: the axis follows the noise band");
        CHECK(hi >= 1 && hi <= 3 && lo <= 3, "ROBUST: the spike column(s) clipped and marked");
        CHECK(strcmp(graphScene().headMode, "ROBUST") == 0, "ROBUST: name in the head line");
        printf("15. ROBUST: AUTO %.3f..%.3f, ROBUST %.3f..%.3f, clipped %d up / %d down\n", a.axisLow, a.axisHigh, r.axisLow, r.axisHigh, hi, lo);
        composeGraph("graph_robust");
    }

    if (failures == 0) printf("ALL OK\n");
    else printf("%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
