#include <Arduino.h>
#include <freertos/task.h>
#include <stdlib.h>
#include <vector>
#include <map>
#include <unordered_map>
#include <atomic>
#include <stdio.h>
#include "config.h"
#include "display.h"
#include <SPI.h>
SPIClass SPI;
// ---- stand-ins for ui.ino / buttons.ino (their own behavior is tested in test_ui.cpp) ----
#include "ui.h"
#include "buttons.h"
static volatile uint8_t gScreen = UI_SCREEN_LCD;
static FbStatsScene gStatsScene;
static volatile bool gUiChanged = false;
static int gButtonPolls = 0;
uint8_t uiGetScreen() { return gScreen; }
bool uiBusy() { return false; }
void uiFillStatsScene(FbStatsScene *scene) { *scene = gStatsScene; }
static int gHelpFills = 0;
void uiFillHelpScene(FbHelpScene *scene)
{
    gHelpFills++;
    memset(scene, 0, sizeof(*scene));
    snprintf(scene->lines[0], sizeof(scene->lines[0]), "HELP:");
    scene->underline[0] = true;
}
// v14 graph screens
static FbGraphScene gGraphScene;
static int gGraphSetupFills = 0;
const FbGraphScene &uiGetGraphScene() { return gGraphScene; }
void uiFillGraphSetupScene(FbSetupScene *scene)
{
    gGraphSetupFills++;
    memset(scene, 0, sizeof(*scene));
    snprintf(scene->title, sizeof(scene->title), "GRAF - NASTAVENI");
}
void uiFillDmmSetupScene(FbSetupScene *scene)
{
    memset(scene, 0, sizeof(*scene));
    snprintf(scene->title, sizeof(scene->title), "DMM - NASTAVENI");
    scene->rowCount = 3;
}
void uiFillAlarmSetupScene(FbSetupScene *scene) // v17
{
    memset(scene, 0, sizeof(*scene));
    snprintf(scene->title, sizeof(scene->title), "DCV - ALARM");
    scene->rowCount = 4;
}
static volatile unsigned long gLastPollUs = 0;
bool uiPoll() { gButtonPolls++; gLastPollUs = micros(); bool c = gUiChanged; gUiChanged = false; return c; }

#include "display.ino"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

struct State { uint8_t seg[6]; int8_t dp; uint16_t ind; uint8_t unit; uint8_t theme; int16_t y; };

static uint64_t hashPanel(const FakeTft &t)
{
    uint64_t h = 1469598103934665603ULL;
    for (int y = 0; y < FakeTft::H; y++)
        for (int x = 0; x < FakeTft::W; x++) { h ^= t.img[y][x]; h *= 1099511628211ULL; }
    return h;
}

// expected panel image for a state, computed with the framebuffer functions directly
static void expectedPanel(const State &s, uint16_t out[FakeTft::H][FakeTft::W])
{
    static uint8_t fbuf[FB_BYTES];
    uint16_t pal[64];
    FbLcdScene sc;
    memcpy(sc.segBits, s.seg, 6); sc.dpPos = s.dp; sc.indicatorBits = s.ind; sc.unit = s.unit;
    fbComposeLcd(fbuf, sc);
    fbBuildPalette(themes[s.theme], pal);
    for (int y = 0; y < FakeTft::H; y++) for (int x = 0; x < FakeTft::W; x++) out[y][x] = 0;
    static uint16_t rows[FB_WIDTH * FB_HEIGHT];
    for (int y0 = 0; y0 < FB_HEIGHT; y0 += 8)
    {
        int n = FB_HEIGHT - y0 < 8 ? FB_HEIGHT - y0 : 8;
        fbUnpackRows(fbuf, y0, n, pal, rows + y0 * FB_WIDTH);
    }
    for (int y = 0; y < FB_HEIGHT; y++) for (int x = 0; x < FB_WIDTH; x++) out[s.y + y][x] = rows[y * FB_WIDTH + x];
}

static State makeState(int k)
{
    State s;
    for (int i = 0; i < 6; i++) s.seg[i] = (uint8_t)((k * 37 + i * 91 + 13) & 0x7F);
    s.dp = (int8_t)((k % 7) - 1);
    s.ind = (uint16_t)((k * 53) & (0xFF));
    s.unit = (uint8_t)(k % KEITHLEY_UNIT_COUNT);
    s.theme = (uint8_t)(k % themeCount);
    s.y = (int16_t)((k % 3) * 24);
    return s;
}

static void apply(const State &s)
{
    setTheme(s.theme);
    setDisplayYOffset(s.y);
    setReadingSegments(s.seg, s.dp);
    setIndicators(s.ind);
    setUnit(s.unit);
}

// ---- threaded stress ----
static std::unordered_map<uint64_t, int> knownHashes;
static std::atomic<int> framesSeen{0}, framesKnown{0}, framesUnknown{0};
static uint64_t lastFrameHash = 0;
static void onFrame(FakeTft &t)
{
    uint64_t h = hashPanel(t);
    framesSeen++;
    lastFrameHash = h;
    if (knownHashes.count(h)) framesKnown++; else framesUnknown++;
}

int main()
{
    CHECK(getDisplayMaxYOffset() == 142 - 94 || true, "dummy");

    // ---- A. synchronous path vs expected ----
    displayBegin();
    CHECK(getDisplayMaxYOffset() == 48, "max y offset 48");
    {
        static uint16_t expect[FakeTft::H][FakeTft::W];
        State s0; memset(&s0, 0, sizeof(s0)); s0.dp = -1; s0.unit = KEITHLEY_UNIT_NONE; s0.theme = CONFIG_DEFAULT_THEME; s0.y = CONFIG_DEFAULT_DISPLAY_Y_OFFSET;
        expectedPanel(s0, expect);
        int bad = 0;
        for (int y = 0; y < FakeTft::H; y++) for (int x = 0; x < FakeTft::W; x++) if (tft.t.img[y][x] != expect[y][x]) bad++;
        printf("A: boot frame mismatches vs expected: %d\n", bad);
        CHECK(bad == 0, "boot frame");
        // top strip rows black
        for (int y = 0; y < 24; y++) for (int x = 0; x < 428; x++) if (tft.t.img[y][x] != 0) { bad++; }
        CHECK(bad == 0, "top strip black");
    }
    for (int k = 1; k < 40; k++)
    {
        State s = makeState(k);
        apply(s);
        displayRenderOnce(false);
        displayBlitFront();
        static uint16_t expect[FakeTft::H][FakeTft::W];
        expectedPanel(s, expect);
        int bad = 0;
        for (int y = 0; y < FakeTft::H; y++) for (int x = 0; x < FakeTft::W; x++) if (tft.t.img[y][x] != expect[y][x]) bad++;
        if (bad) printf("A: state %d mismatches %d\n", k, bad);
        CHECK(bad == 0, "sync state frame");
        CHECK(getDisplayYOffset() == s.y, "getYOffset");
        CHECK(getIndicators() == s.ind, "getIndicators");
        CHECK(strcmp(currentThemeName(), themes[s.theme].name) == 0, "theme name");
    }
    // out of range handling
    setDisplayYOffset(500); CHECK(getDisplayYOffset() == 48, "clamp hi");
    setDisplayYOffset(-5);  CHECK(getDisplayYOffset() == 0, "clamp lo");
    setTheme(200);          CHECK(strcmp(currentThemeName(), themes[themeCount - 1].name) == 0, "theme clamp");
    setUnit(99);            // ignored
    setReading(String("12L 4"), 2);
    displayRenderOnce(false); displayBlitFront();
    printf("A: done\n");

    // ---- A2. the quick statistics screen goes through the same pipeline ----
    {
        memset(&gStatsScene, 0, sizeof(gStatsScene));
        strcpy(gStatsScene.live, "-1.23456");
        strcpy(gStatsScene.average, "-1.23401");
        strcpy(gStatsScene.median, "-1.23450");
        strcpy(gStatsScene.maximum, "-1.20000");
        strcpy(gStatsScene.minimum, "-1.25000");
        snprintf(gStatsScene.unit, sizeof(gStatsScene.unit), "%cA", FONT_CH_MICRO);
        strcpy(gStatsScene.interval, "15 MIN");
        strcpy(gStatsScene.dateTime, "14:03:07 06.10.26");
        gScreen = UI_SCREEN_QUICKSTATS;
        for (int th = 0; th < (int)themeCount; th++)
        {
            setTheme(th);
            setDisplayYOffset(24);
            displayRenderOnce(false);
            displayBlitFront();
            // expected: the same scene composed directly with the framebuffer functions
            static uint8_t fbuf[FB_BYTES];
            static uint16_t rows[FB_WIDTH * FB_HEIGHT];
            uint16_t pal[64];
            fbBuildPalette(fbStatsTheme(themes[th]), pal); // the user interface screens turn light themes around
            fbComposeStats(fbuf, gStatsScene);
            for (int y0 = 0; y0 < FB_HEIGHT; y0 += 8)
            {
                int n = FB_HEIGHT - y0 < 8 ? FB_HEIGHT - y0 : 8;
                fbUnpackRows(fbuf, y0, n, pal, rows + y0 * FB_WIDTH);
            }
            int bad = 0;
            for (int y = 0; y < FB_HEIGHT; y++) for (int x = 0; x < FB_WIDTH; x++) if (tft.t.img[24 + y][x] != rows[y * FB_WIDTH + x]) bad++;
            CHECK(bad == 0, "stats screen frame equals direct composition");
        }
        printf("A2: stats screen on %d themes checked\n", (int)themeCount);
        gScreen = UI_SCREEN_LCD;
        setTheme(0);
        displayRenderOnce(false); displayBlitFront();
    }

    // ---- B. threaded stress ----
    const int POOL = 16;
    static uint16_t expectBuf[FakeTft::H][FakeTft::W];
    std::vector<State> pool;
    for (int k = 100; k < 100 + POOL; k++)
    {
        State s = makeState(k);
        pool.push_back(s);
        expectedPanel(s, expectBuf);
        FakeTft tmp; memcpy(tmp.img, expectBuf, sizeof(expectBuf));
        knownHashes[hashPanel(tmp)] = k;
    }
    // settle the display to the first pool state before starting tasks, as real setup() does
    apply(pool[0]);
    tft.t.onFrame = onFrame;
    tft.t.slowUs = 150; // slow panel: ~1.8 ms per frame, forces back-buffer contention
    displayStartTasks();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    printf("B: initial frames seen %d known %d unknown %d\n", framesSeen.load(), framesKnown.load(), framesUnknown.load());
    CHECK(framesUnknown == 0, "initial unknown frames");
    // initial render must show pool[0]
    {
        expectedPanel(pool[0], expectBuf);
        FakeTft tmp; memcpy(tmp.img, expectBuf, sizeof(expectBuf));
        CHECK(lastFrameHash == hashPanel(tmp), "initial state shown after task start");
    }

    srand(5);
    int updates = 0;
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(4))
    {
        const State &s = pool[rand() % POOL];
        apply(s);
        updates++;
        int gapUs = (rand() % 4 == 0) ? 0 : (rand() % 3000);
        if (gapUs) std::this_thread::sleep_for(std::chrono::microseconds(gapUs));
    }
    // final state and quiescence
    const State &fin = pool[7];
    apply(fin);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    expectedPanel(fin, expectBuf);
    FakeTft tmp; memcpy(tmp.img, expectBuf, sizeof(expectBuf));
    DisplayStats st = displayGetStats();
    printf("B: updates=%d frames=%d known=%d unknown=%d overlapErrors=%d\n", updates, framesSeen.load(), framesKnown.load(), framesUnknown.load(), tft.t.overlapErrors);
    printf("B: renders=%u (max %u us) blits=%u (max %u us) renderWaitedForBlit=%u stackFree r=%u b=%u\n",
           st.renderCount, st.renderMaxUs, st.blitCount, st.blitMaxUs, st.renderWaitedForBlit, st.renderStackMinFreeBytes, st.blitStackMinFreeBytes);
    CHECK(tft.t.overlapErrors == 0, "tft never used by two threads at once");
    CHECK(framesUnknown <= framesSeen / 50, "torn/mixed frames under 2%");
    CHECK(lastFrameHash == hashPanel(tft.t), "last frame consistent");
    CHECK(hashPanel(tft.t) == hashPanel(tmp), "final state shown (no lost update)");

    // ---- C. tear detection: a single setter changes the WHOLE picture, so any frame that is not
    // exactly one of the known images has rows from two different renders (a real tear). Very slow
    // panel (~12 ms per blit) so the render task regularly finds the back slot still being read.
    knownHashes.clear();
    State base = fin; base.y = 24;
    for (int th = 0; th < (int)themeCount; th++)
    {
        State s = base; s.theme = th;
        expectedPanel(s, expectBuf);
        FakeTft t2; memcpy(t2.img, expectBuf, sizeof(expectBuf));
        knownHashes[hashPanel(t2)] = th;
    }
    apply(base);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    framesSeen = 0; framesKnown = 0; framesUnknown = 0;
    tft.t.slowUs = 1000;
    DisplayStats before = displayGetStats();
    t0 = std::chrono::steady_clock::now();
    int themeUpdates = 0;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5))
    {
        setTheme((uint8_t)(rand() % themeCount));
        themeUpdates++;
        std::this_thread::sleep_for(std::chrono::microseconds(rand() % 5000));
    }
    setTheme(3);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    State fin2 = base; fin2.theme = 3;
    expectedPanel(fin2, expectBuf);
    FakeTft t3; memcpy(t3.img, expectBuf, sizeof(expectBuf));
    DisplayStats after = displayGetStats();
    printf("C: updates=%d frames=%d known=%d unknown=%d overlapErrors=%d renderWaitedForBlit=%u\n", themeUpdates,
           framesSeen.load(), framesKnown.load(), framesUnknown.load(), tft.t.overlapErrors, after.renderWaitedForBlit - before.renderWaitedForBlit);
    CHECK(framesUnknown == 0, "no torn frames");
    CHECK(after.renderWaitedForBlit > before.renderWaitedForBlit, "contention path was exercised");
    CHECK(hashPanel(tft.t) == hashPanel(t3), "final theme shown");
    CHECK(tft.t.overlapErrors == 0, "tft single user");
    // ---- D. the render task polls the buttons/UI by itself and re-renders when the UI says so ----
    {
        int pollsBefore = gButtonPolls;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        int polls = gButtonPolls - pollsBefore;
        printf("D: button polls in 200 ms: %d (expect ~20 at %d ms)\n", polls, CONFIG_UI_POLL_MS);
        CHECK(polls >= 10 && polls <= 40, "UI/button polling rate");

        // displayWakeUi() (what the button task calls) makes the UI task look at the UI at once, not at the next poll
        {
            unsigned long worst = 0, sum = 0;
            for (int i = 0; i < 20; i++)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(7 + i % 5)); // different phases of the 10 ms poll
                unsigned long t0 = micros();
                displayWakeUi();
                while (gLastPollUs < t0 && micros() - t0 < 100000) {}
                unsigned long took = gLastPollUs - t0;
                if (took > worst) worst = took;
                sum += took;
            }
            printf("D: displayWakeUi() -> uiPoll(): mean %lu us, worst %lu us over 20 tries (the poll period is %d ms, so waiting for it would average 5 ms)\n", sum / 20, worst, CONFIG_UI_POLL_MS);
            CHECK(sum / 20 < 2500 && worst < 9000, "a UI wake-up is served at once, not at the next poll");
        }

        // switch to the statistics screen the way ui.ino does: change state, return true from uiPoll()
        tft.t.slowUs = 0;
        setTheme(2);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        gScreen = UI_SCREEN_QUICKSTATS;
        gUiChanged = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        static uint8_t fbuf[FB_BYTES];
        static uint16_t rows[FB_WIDTH * FB_HEIGHT];
        uint16_t pal[64];
        fbBuildPalette(fbStatsTheme(themes[2]), pal);
        fbComposeStats(fbuf, gStatsScene);
        for (int y0 = 0; y0 < FB_HEIGHT; y0 += 8)
        {
            int n = FB_HEIGHT - y0 < 8 ? FB_HEIGHT - y0 : 8;
            fbUnpackRows(fbuf, y0, n, pal, rows + y0 * FB_WIDTH);
        }
        int yOff = getDisplayYOffset();
        int bad = 0;
        for (int y = 0; y < FB_HEIGHT; y++) for (int x = 0; x < FB_WIDTH; x++) if (tft.t.img[yOff + y][x] != rows[y * FB_WIDTH + x]) bad++;
        CHECK(bad == 0, "statistics screen shown after uiPoll() reported a change");

        // the help screen (v12): composed from uiFillHelpScene() on the same black-background palette
        gScreen = UI_SCREEN_HELP;
        gUiChanged = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        FbHelpScene hs;
        uiFillHelpScene(&hs);
        fbComposeHelp(fbuf, hs);
        for (int y0 = 0; y0 < FB_HEIGHT; y0 += 8)
        {
            int n = FB_HEIGHT - y0 < 8 ? FB_HEIGHT - y0 : 8;
            fbUnpackRows(fbuf, y0, n, pal, rows + y0 * FB_WIDTH);
        }
        int badHelp = 0;
        for (int y = 0; y < FB_HEIGHT; y++) for (int x = 0; x < FB_WIDTH; x++) if (tft.t.img[yOff + y][x] != rows[y * FB_WIDTH + x]) badHelp++;
        CHECK(gHelpFills > 1 && badHelp == 0, "help screen shown after uiPoll() reported a change");
        int lineY = fbHelpUnderlineY(0);
        {
            // v15: dashed CONFIG_UI_SEPARATOR_ON / _OFF across the whole width
            const int period = CONFIG_UI_SEPARATOR_ON + CONFIG_UI_SEPARATOR_OFF;
            int wrong = 0;
            for (int x = 0; x < FB_WIDTH; x++)
            {
                bool on = (x % period) < CONFIG_UI_SEPARATOR_ON;
                if ((fbGet(fbuf, x, lineY) == fbRampIndex(FB_INK_BRIGHT, 255)) != on) wrong++;
            }
            CHECK(wrong == 0, "dashed underline across the whole width");
        }

        // v13 layout: ink of the first line reaches row 0, of the last line the bottom row, even spacing, no overlap
        {
            int16_t above = 0, below = 0;
            fbHelpInkExtent(&above, &below);
            CHECK(fbHelpBaseline(0) - above == 0, "first line's ink starts at the top row");
            CHECK(fbHelpBaseline(FB_HELP_LINES - 1) + below - 1 == FB_HEIGHT - 1, "last line's ink ends at the bottom row");
            int minPitch = 1000, maxPitch = 0;
            for (int i = 1; i < FB_HELP_LINES; i++)
            {
                int p = fbHelpBaseline(i) - fbHelpBaseline(i - 1);
                if (p < minPitch) minPitch = p;
                if (p > maxPitch) maxPitch = p;
                CHECK(fbHelpBaseline(i) - FONT_TINY.digitHeight > fbHelpBaseline(i - 1) + below - 1, "capitals start below the descenders of the line above");
                int u = fbHelpUnderlineY(i - 1);
                CHECK(u >= fbHelpBaseline(i - 1) && u < fbHelpBaseline(i) - FONT_TINY.digitHeight, "underline between the (descender-free) line and the next capitals");
            }
            CHECK(maxPitch - minPitch <= 1, "even spacing (rounding only)");
            printf("help layout: ink %d above / %d below the baseline, baselines", above, below);
            for (int i = 0; i < FB_HELP_LINES; i++) printf(" %d", fbHelpBaseline(i));
            printf(", underline under line 1 at row %d\n", fbHelpUnderlineY(1));
            // a full page of glyphs with the tallest/deepest ink: nothing outside the frame is lost
            FbHelpScene full;
            memset(&full, 0, sizeof(full));
            for (int i = 0; i < FB_HELP_LINES; i++) snprintf(full.lines[i], sizeof(full.lines[i]), "(Kgjpqy|$)");
            fbComposeHelp(fbuf, full);
            bool topInk = false, bottomInk = false;
            for (int x = 0; x < FB_WIDTH; x++) { if (fbGet(fbuf, x, 0)) topInk = true; if (fbGet(fbuf, x, FB_HEIGHT - 1)) bottomInk = true; }
            CHECK(topInk && bottomInk, "the text really touches the top and the bottom edge");
        }
        CHECK(pal[0] == RGB565(0, 0, 0), "help/stats background black");

        gScreen = UI_SCREEN_LCD;
        gUiChanged = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        // back on the LCD: the picture must be exactly the LCD state requested before (base, theme 2)
        State lcdState = base;
        lcdState.theme = 2;
        expectedPanel(lcdState, expectBuf);
        int badLcd = 0;
        for (int y = 0; y < FakeTft::H; y++) for (int x = 0; x < FakeTft::W; x++) if (tft.t.img[y][x] != expectBuf[y][x]) badLcd++;
        CHECK(badLcd == 0, "LCD screen restored exactly after leaving the statistics screen");
    }
    printf(failures ? "FAILURES: %d\n" : "ALL OK\n", failures);
    fflush(stdout);
    std::_Exit(failures != 0);
}
