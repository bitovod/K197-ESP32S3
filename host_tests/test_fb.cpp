#include <Arduino.h>
#include <stdlib.h>
#include <math.h>
#include "framebuffer.h"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); failures++; } } while (0)

static uint8_t bufA[FB_BYTES];

// Reference: the old direct renderer's math (blend565 per pixel), composed in the same order
static uint16_t refImg[FB_WIDTH * FB_HEIGHT];

static void refMask(const KeithleyTheme &th, const uint8_t *mask, int x, int y, int w, int h, uint16_t ink)
{
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            refImg[(y + r) * FB_WIDTH + x + c] = fbBlend565(th.bg, ink, mask[r * w + c]);
}

static void refCompose(const KeithleyTheme &th, const FbLcdScene &s)
{
    for (int i = 0; i < FB_WIDTH * FB_HEIGHT; i++) refImg[i] = th.bg;
    for (int i = 0; i < KEITHLEY_INDICATOR_COUNT; i++)
    {
        bool on = s.indicatorBits & (1u << i);
        if (i == KEITHLEY_IND_REM && !on)
        {
            // v18: "TAG" stands in place of REM unless the K197 lights REM
            refMask(th, keithley_tag_mask, keithley_ind_x[i] + (keithley_ind_w[i] - KEITHLEY_TAG_W) / 2, keithley_ind_y[i],
                    KEITHLEY_TAG_W, KEITHLEY_TAG_H, s.tagLit ? th.indicator : th.ghost);
            continue;
        }
        if ((i == KEITHLEY_IND_STO || i == KEITHLEY_IND_RCL) && !on)
        {
            // v24: "HOLD" / "AUTO" stand in place of STO / RCL unless the K197 lights them
            bool isHold = (i == KEITHLEY_IND_STO);
            bool lit = (s.hold & (isHold ? FB_HOLD_LIT : FB_HOLD_AUTO_LIT)) != 0;
            refMask(th, isHold ? keithley_hold_mask : keithley_auto_mask, keithley_ind_x[i], keithley_ind_y[i],
                    KEITHLEY_HOLD_W, KEITHLEY_HOLD_H, lit ? th.indicator : th.ghost);
            continue;
        }
        if (i == KEITHLEY_IND_BAT && !on)
        {
            // v23: "SD!" stands in place of BAT unless the K197 lights BAT
            refMask(th, keithley_sd_mask, keithley_ind_x[i] + (keithley_ind_w[i] - KEITHLEY_SD_W) / 2, keithley_ind_y[i],
                    KEITHLEY_SD_W, KEITHLEY_SD_H, s.sdLit ? th.indicator : th.ghost);
            continue;
        }
        uint16_t onColor = (i == KEITHLEY_IND_AC) ? th.bright : th.indicator;
        refMask(th, keithley_ind_mask_table[i], keithley_ind_x[i], keithley_ind_y[i], keithley_ind_w[i], keithley_ind_h[i], on ? onColor : th.ghost);
    }
    bool minusOn = s.indicatorBits & FB_BIT_MINUS;
    refMask(th, keithley_minus_mask, KEITHLEY_MINUS_X, KEITHLEY_MINUS_Y, KEITHLEY_MINUS_W, KEITHLEY_MINUS_H, minusOn ? th.bright : th.ghost);
    if (s.unit != KEITHLEY_UNIT_NONE)
        refMask(th, keithley_unit_mask_table[s.unit], KEITHLEY_UNIT_X, KEITHLEY_UNIT_Y, KEITHLEY_UNIT_W, KEITHLEY_UNIT_H, th.bright);
    for (int d = 0; d < KEITHLEY_DIGIT_COUNT; d++)
    {
        const uint8_t *mask = keithley_mask_table[d];
        for (int r = 0; r < KEITHLEY_GLYPH_HEIGHT; r++)
            for (int c = 0; c < KEITHLEY_GLYPH_WIDTH; c++)
            {
                uint8_t id = mask[r * KEITHLEY_GLYPH_WIDTH + c];
                uint16_t col;
                if (id == KEITHLEY_SEG_BLACK || id == KEITHLEY_SEG_DOT) col = th.bg; // dot is a separate shape now
                else col = (s.segBits[d] & (1 << (id - 1))) ? th.bright : th.ghost;
                refImg[(keithley_digit_y[d] + r) * FB_WIDTH + keithley_digit_x[d] + c] = col;
            }
        if (d <= 4)
        {
            // Egg-shaped dot, written out independently of the table in keithley_shapes.h
            static const int starts[9] = {43, 42, 41, 41, 40, 40, 40, 40, 41};
            static const int widths[9] = {4, 6, 8, 8, 9, 9, 9, 8, 6};
            uint16_t col = (d == s.dpPos) ? th.bright : th.ghost;
            for (int k = 0; k < 9; k++)
                for (int x = starts[k]; x < starts[k] + widths[k]; x++)
                    refImg[(keithley_digit_y[d] + 64 + k) * FB_WIDTH + keithley_digit_x[d] + x] = col;
        }
    }
}

static void chan(uint16_t c, int &r, int &g, int &b)
{
    r = ((c >> 11) & 31) * 255 / 31; g = ((c >> 5) & 63) * 255 / 63; b = (c & 31) * 255 / 31;
}

int main()
{
    printf("FB_STRIDE_BYTES=%d FB_BYTES=%d sizeof(FbSlot)=%zu\n", FB_STRIDE_BYTES, FB_BYTES, sizeof(FbSlot));
    CHECK(FB_STRIDE_BYTES == 321, "stride 321");
    CHECK(FB_BYTES == 30174, "bytes 30174");

    // 1. pack / get / put roundtrip
    srand(1);
    static uint8_t shadow[FB_WIDTH * FB_HEIGHT];
    memset(bufA, 0, sizeof(bufA));
    for (int i = 0; i < 400000; i++)
    {
        int x = rand() % FB_WIDTH, y = rand() % FB_HEIGHT, v = rand() & 63;
        fbPut(bufA, x, y, v);
        shadow[y * FB_WIDTH + x] = v;
    }
    int bad = 0;
    for (int y = 0; y < FB_HEIGHT; y++)
        for (int x = 0; x < FB_WIDTH; x++)
            if (fbGet(bufA, x, y) != shadow[y * FB_WIDTH + x]) bad++;
    CHECK(bad == 0, "put/get roundtrip");
    printf("roundtrip mismatches: %d\n", bad);

    // 2. fill
    for (int v = 0; v < 64; v++)
    {
        fbFill(bufA, v);
        bad = 0;
        for (int y = 0; y < FB_HEIGHT; y++)
            for (int x = 0; x < FB_WIDTH; x++)
                if (fbGet(bufA, x, y) != v) bad++;
        CHECK(bad == 0, "fill");
    }

    // 3. unpack rows vs get
    static uint16_t pal[64];
    for (int i = 0; i < 64; i++) pal[i] = (uint16_t)(i * 1000 + 7);
    for (int i = 0; i < 20000; i++) fbPut(bufA, rand() % FB_WIDTH, rand() % FB_HEIGHT, rand() & 63);
    static uint16_t rowsOut[FB_WIDTH * 8];
    bad = 0;
    for (int y0 = 0; y0 < FB_HEIGHT; y0 += 8)
    {
        int rows = (FB_HEIGHT - y0 < 8) ? FB_HEIGHT - y0 : 8;
        fbUnpackRows(bufA, y0, rows, pal, rowsOut);
        for (int r = 0; r < rows; r++)
            for (int x = 0; x < FB_WIDTH; x++)
                if (rowsOut[r * FB_WIDTH + x] != pal[fbGet(bufA, x, y0 + r)]) bad++;
    }
    CHECK(bad == 0, "unpackRows");

    // 4. palette: exact end points and monotone ramp index mapping, every theme
    for (int t = 0; t < (int)themeCount; t++)
    {
        const KeithleyTheme &th = themes[t];
        uint16_t p[64];
        fbBuildPalette(th, p);
        CHECK(p[0] == th.bg, "pal bg");
        CHECK(p[fbRampIndex(FB_INK_GHOST, 255)] == th.ghost, "pal ghost exact");
        CHECK(p[fbRampIndex(FB_INK_INDICATOR, 255)] == th.indicator, "pal indicator exact");
        CHECK(p[fbRampIndex(FB_INK_BRIGHT, 255)] == th.bright, "pal bright exact");
        CHECK(fbRampIndex(FB_INK_BRIGHT, 255) == 63, "max index 63");
        for (int ink = 0; ink < 3; ink++)
        {
            int prev = 0;
            for (int c = 0; c <= 255; c++)
            {
                int ix = fbRampIndex(ink, c);
                CHECK(ix < 64, "index range");
                if (c > 0 && ix != 0) { CHECK(ix >= 1 + ink * 21 && ix <= 21 + ink * 21, "ramp range"); }
                CHECK(ix >= prev || ix == 0, "monotone");
                if (ix != 0) prev = ix;
            }
        }
    }

    // 5. compose vs reference, all themes, several scenes
    struct Case { uint8_t seg[6]; int dp; uint16_t ind; uint8_t unit; };
    Case cases[] = {
        {{125, 80, 55, 87, 90, 79}, 1, (1u << KEITHLEY_IND_AUT) | FB_BIT_MINUS, KEITHLEY_UNIT_V},
        {{127, 127, 127, 127, 127, 127}, 5, 0x7F | FB_BIT_MINUS, KEITHLEY_UNIT_KOHM},
        {{0, 0, 0, 0, 0, 0}, -1, 0, KEITHLEY_UNIT_NONE},
        {{44, 125, 0, 111, 95, 81}, 3, (1u << KEITHLEY_IND_AC) | (1u << KEITHLEY_IND_REL) | (1u << KEITHLEY_IND_BAT), KEITHLEY_UNIT_MV},
    };
    for (int t = 0; t < (int)themeCount; t++)
    {
        const KeithleyTheme &th = themes[t];
        uint16_t p[64];
        fbBuildPalette(th, p);
        for (unsigned ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++)
        {
            FbLcdScene s;
            memcpy(s.segBits, cases[ci].seg, 6);
            s.dpPos = cases[ci].dp; s.indicatorBits = cases[ci].ind; s.unit = cases[ci].unit;
            s.sdLit = (ci == 2 || ci == 3); // v23: lit SD! (case 3: BAT lit by the K197 wins)
            s.hold = (uint8_t)(ci == 0 ? FB_HOLD_LIT : (ci == 2 ? FB_HOLD_LIT | FB_HOLD_AUTO_LIT : 0)); // v24
            fbComposeLcd(bufA, s);
            refCompose(th, s);
            static uint16_t out[FB_WIDTH * FB_HEIGHT];
            for (int y0 = 0; y0 < FB_HEIGHT; y0 += 8)
            {
                int rows = (FB_HEIGHT - y0 < 8) ? FB_HEIGHT - y0 : 8;
                fbUnpackRows(bufA, y0, rows, p, out + y0 * FB_WIDTH);
            }
            int maxErr = 0, exact = 0, nonexact = 0; double sum = 0;
            for (int i = 0; i < FB_WIDTH * FB_HEIGHT; i++)
            {
                int r1, g1, b1, r2, g2, b2;
                chan(out[i], r1, g1, b1); chan(refImg[i], r2, g2, b2);
                int e = abs(r1 - r2); if (abs(g1 - g2) > e) e = abs(g1 - g2); if (abs(b1 - b2) > e) e = abs(b1 - b2);
                if (e > maxErr) maxErr = e;
                if (out[i] == refImg[i]) exact++; else { nonexact++; sum += e; }
            }
            printf("theme %-10s case %u: max channel err %3d/255, exact px %6d, approx px %5d, mean err of approx %.2f\n",
                   th.name, ci, maxErr, exact, nonexact, nonexact ? sum / nonexact : 0.0);
            CHECK(maxErr <= 14, "compose error bound");
        }
    }
    // 6. decimal point: exact pixel set for a lit dot, ghost dots elsewhere, none on the last digit
    {
        const KeithleyTheme &th = themes[0];
        uint16_t p[64];
        fbBuildPalette(th, p);
        FbLcdScene sc;
        memset(sc.segBits, 0, 6); sc.dpPos = 2; sc.indicatorBits = 0; sc.unit = KEITHLEY_UNIT_NONE;
        fbComposeLcd(bufA, sc);
        static const int starts[9] = {43, 42, 41, 41, 40, 40, 40, 40, 41};
        static const int widths[9] = {4, 6, 8, 8, 9, 9, 9, 8, 6};
        int litCount = 0, ghostCount = 0, wrong = 0;
        for (int d = 0; d < 6; d++)
        {
            int x0 = keithley_digit_x[d], y0 = keithley_digit_y[d];
            for (int k = 0; k < 9; k++)
                for (int x = 36; x <= 48; x++)
                {
                    bool inShape = (x >= starts[k]) && (x < starts[k] + widths[k]);
                    uint16_t c = p[fbGet(bufA, x0 + x, y0 + 64 + k)];
                    if (d <= 4 && inShape)
                    {
                        if (d == 2) { litCount++; if (c != th.bright) wrong++; }
                        else { ghostCount++; if (c != th.ghost) wrong++; }
                    }
                    else if (((d == 5 && x >= 41) || k >= 7) && c != th.bg) wrong++; // digit 5 (right of its segments) and the rows below the glyph (y>70): nothing but bg outside the shape
                }
        }
        printf("dot: lit pixels %d (expect 67), ghost pixels %d (expect 268), wrong %d\n", litCount, ghostCount, wrong);
        CHECK(litCount == 67, "lit dot has 67 pixels");
        CHECK(ghostCount == 268, "four ghost dots of 67 pixels");
        CHECK(wrong == 0, "dot region has nothing else");
    }
    // 7. statistics screen layout: units end flush, nothing overlaps the legend, the lines are in order and
    //    inside the screen, the whole screen width is used, the date is centred in its free space
    {
        FbStatsRows rows = fbStatsRows();
        printf("stats rows: top line baseline %d, date %d, row1 %d, row2 %d (screen height %d)\n",
               rows.baselineTop, rows.baselineDate, rows.baselineRow1, rows.baselineRow2, FB_HEIGHT);
        CHECK(rows.baselineDate - FONT_TINY.digitHeight == 0, "date touches the top edge");
        CHECK(rows.baselineTop - FONT_LIVE.digitHeight == 0, "selector and live value touch the top edge");
        CHECK(rows.baselineRow1 - FONT_STAT.digitHeight > rows.baselineTop, "row 1 below the top line");
        CHECK(rows.baselineRow2 - FONT_STAT.digitHeight > rows.baselineRow1, "row 2 below row 1");
        CHECK(rows.baselineRow2 < FB_HEIGHT, "last baseline inside the screen");
        int gap1 = (rows.baselineRow1 - FONT_STAT.digitHeight) - rows.baselineTop;
        int gap2 = (rows.baselineRow2 - FONT_STAT.digitHeight) - rows.baselineRow1;
        CHECK(gap1 == gap2, "the two gaps are equal");

        const char *numbers[] = {"0", "1.5", "0.50000", "1.23456", "-1.23456", "-1.99999", "199999", "-OL", "---", "19.5000"};
        char units[9][4];
        for (int u = 0; u < 9; u++)
        {
            char tmp[4] = {0};
            switch (u)
            {
                case 0: snprintf(tmp, 4, "mV"); break;
                case 1: snprintf(tmp, 4, "V"); break;
                case 2: snprintf(tmp, 4, "%cA", FONT_CH_MICRO); break;
                case 3: snprintf(tmp, 4, "mA"); break;
                case 4: snprintf(tmp, 4, "A"); break;
                case 5: snprintf(tmp, 4, "%c", FONT_CH_OHM); break;
                case 6: snprintf(tmp, 4, "k%c", FONT_CH_OHM); break;
                case 7: snprintf(tmp, 4, "M%c", FONT_CH_OHM); break;
                default: snprintf(tmp, 4, "dB"); break;
            }
            memcpy(units[u], tmp, 4);
        }
        int fallbacks = 0, probes = 0;
        for (int col = 0; col < 2; col++)
            for (const char *num : numbers)
                for (int u = 0; u < 9; u++)
                {
                    probes++;
                    FbStatsCell cell = fbStatsCellLayout(col == 1, num, units[u]);
                    int16_t unitRight = cell.unitX + fbTextWidth(*cell.font, units[u]);
                    if (col == 1)
                        CHECK(unitRight == FB_WIDTH, "right column: units end at the right edge of the screen");
                    else
                        CHECK(cell.x == 0, "left column starts at the left edge of the screen");
                    int16_t numberLeft = cell.numberRight - FB_STATS_NUMBER_CHARS * cell.font->advance;
                    int16_t legendRight = cell.x + fbTextWidth(FONT_LEGEND, "MAX");
                    CHECK(numberLeft >= legendRight + FB_STATS_GAP_LEGEND, "the number field does not touch the legend");
                    CHECK(cell.unitX - FB_STATS_GAP_UNIT == cell.numberRight, "number ends right before its unit");
                    CHECK(fbTextWidth(*cell.font, num) <= FB_STATS_NUMBER_CHARS * cell.font->advance, "the number fits its field");
                    if (cell.font != &FONT_STAT) fallbacks++;
                }
        printf("stats layout: %d of %d probes needed a smaller font\n", fallbacks, probes);
        CHECK(fallbacks == 0, "every value the formatter can produce fits the largest font");

        FbStatsCell left = fbStatsCellLayout(false, "-1.23456", "mV");
        FbStatsCell right = fbStatsCellLayout(true, "-1.23456", "mV");
        int leftEnd = left.unitX + fbTextWidth(*left.font, "mV");
        printf("space between the columns (2 character unit): %d px\n", right.x - leftEnd);
        CHECK(right.x - leftEnd >= 20, "a clear gap between the two columns");

        // the date: centred between the selector and the live display's fixed width, then moved one character right
        const char *when = "14:03:07 06.10.26";
        int16_t selectorEnd = fbStatsSelectorEnd();
        int16_t liveStart = fbStatsLiveStart();
        int16_t dateX = fbStatsDateX(when);
        int16_t dateW = fbTextWidth(FONT_TINY, when);
        int leftMargin = dateX - selectorEnd;
        int rightMargin = liveStart - (dateX + dateW);
        printf("date/time: free space %d..%d, text %d px wide, margins %d left / %d right\n", selectorEnd, liveStart, dateW, leftMargin, rightMargin);
        CHECK(leftMargin >= 8 && rightMargin >= FB_STATS_GAP_UNIT, "the date fits with room to spare");
        int shift = (leftMargin - rightMargin) / 2;
        CHECK(shift >= FONT_TINY.advance - 1 && shift <= FONT_TINY.advance, "the date sits one character right of the centre");
        // and the real worst-case live value (8 character number + 2 character unit) really starts at liveStart
        int16_t worstLive = FB_WIDTH - fbTextWidth(FONT_LIVE, "mV") - FB_STATS_GAP_UNIT - fbTextWidth(FONT_LIVE, "-1.23456");
        CHECK(worstLive == liveStart, "fixed live width = the widest real live display");
    }

    // 8. pictures: lit pixels, the triangles, the plain zero, the themes
    {
        FbStatsScene sc;
        memset(&sc, 0, sizeof(sc));
        strcpy(sc.live, "-1.23456"); strcpy(sc.average, "-1.23401"); strcpy(sc.median, "-1.23450");
        strcpy(sc.maximum, "-1.20000"); strcpy(sc.minimum, "-1.25000");
        snprintf(sc.unit, sizeof(sc.unit), "k%c", FONT_CH_OHM);
        strcpy(sc.interval, "15 MIN"); strcpy(sc.dateTime, "14:03:07 06.10.26");
        fbComposeStats(bufA, sc);
        int lit = 0;
        for (int y = 0; y < FB_HEIGHT; y++) for (int x = 0; x < FB_WIDTH; x++) if (fbGet(bufA, x, y) != 0) lit++;
        printf("statistics screen: %d lit pixels\n", lit);
        CHECK(lit > 3000, "something is drawn");
        // top edge: the first row of the screen carries ink (selector / date / live value touch it)
        int topInk = 0;
        for (int x = 0; x < FB_WIDTH; x++) if (fbGet(bufA, x, 0) != 0) topInk++;
        CHECK(topInk > 20, "text touches the top edge");

        // triangles: same size, mirror images
        memset(&sc, 0, sizeof(sc));
        strcpy(sc.interval, "1 HOUR");
        fbComposeStats(bufA, sc);
        int leftCoverage = 0, rightCoverage = 0;
        int rightX = FB_STATS_ARROW_DEPTH + FB_STATS_ARROW_GAP + FB_STATS_INTERVAL_CHARS * FONT_LIVE.advance + FB_STATS_ARROW_GAP;
        for (int y = 0; y < FONT_LIVE.digitHeight; y++)
            for (int dx = 0; dx <= FB_STATS_ARROW_DEPTH; dx++)
            {
                if (fbGet(bufA, dx, y) != 0) leftCoverage++;
                if (fbGet(bufA, rightX + dx, y) != 0) rightCoverage++;
            }
        CHECK(leftCoverage > 20 && leftCoverage == rightCoverage, "the two triangles have the same size");
        int tipLeft = 0, baseLeft = 0;
        int mid = FONT_LIVE.digitHeight / 2;
        for (int y = mid - 6; y <= mid + 5; y++)
        {
            if (fbGet(bufA, 0, y) != 0) tipLeft++;
            if (fbGet(bufA, FB_STATS_ARROW_DEPTH - 1, y) != 0) baseLeft++;
        }
        CHECK(baseLeft > tipLeft, "left triangle is wider at its right side (points left)");

        // the zero has no dot: in the middle of every font's '0' there is no ink, in the '8' the waist is inked
        const FontDef *fonts[] = {&FONT_STAT, &FONT_STAT2, &FONT_STAT3, &FONT_LIVE, &FONT_LEGEND, &FONT_TINY};
        const char *names[] = {"STAT", "STAT2", "STAT3", "LIVE", "LEGEND", "TINY"};
        for (int f = 0; f < 6; f++)
        {
            const FontDef &fd = *fonts[f];
            int cellSize = fd.cellW * fd.cellH;
            const uint8_t *zero = fd.cells + fbGlyphIndex('0') * cellSize;
            const uint8_t *eight = fd.cells + fbGlyphIndex('8') * cellSize;
            int midX = fd.cellW / 2;
            int midY = fd.ascent - fd.digitHeight / 2; // the vertical middle of the digit
            int zeroInk = 0, eightInk = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 0; dx++)
                {
                    zeroInk += zero[(midY + dy) * fd.cellW + midX + dx];
                    eightInk += eight[(midY + dy) * fd.cellW + midX + dx];
                }
            printf("font %-6s: ink in the middle of '0' = %d, of '8' = %d\n", names[f], zeroInk, eightInk);
            CHECK(zeroInk == 0, "the zero has an empty middle (no dot)");
            CHECK(eightInk > 0, "the eight has its waist in the middle");
        }
    }

    // 8b. v25 statistics pages 2 and 3: the top line is the same on every page, the columns never touch, page 3's
    // legends start where page 1's do
    {
        static uint8_t pages[3][FB_BYTES];
        FbStatsScene sc;
        memset(&sc, 0, sizeof(sc));
        strcpy(sc.live, "-1.23456"); strcpy(sc.unit, "V"); strcpy(sc.interval, "1 HOUR"); strcpy(sc.dateTime, "14:03:07 11.10.26");
        strcpy(sc.average, "-1.23451"); strcpy(sc.median, "-1.23452"); strcpy(sc.maximum, "-1.23438"); strcpy(sc.minimum, "-1.23467");
        // the widest texts each field can get
        strcpy(sc.sdev, "--.----"); snprintf(sc.sdevUnit, sizeof(sc.sdevUnit), "m%c", FONT_CH_OHM);
        strcpy(sc.rel, "--.----"); strcpy(sc.relUnit, "ppm");
        strcpy(sc.dif, "-1.99999"); strcpy(sc.pts, "75600");
        strcpy(sc.sem, "888"); snprintf(sc.semUnit, sizeof(sc.semUnit), "%cV", FONT_CH_MICRO);
        strcpy(sc.trend, "-888"); snprintf(sc.trendUnit, sizeof(sc.trendUnit), "k%c/h", FONT_CH_OHM);
        strcpy(sc.sdevDigits, "888"); strcpy(sc.difRel, "0.888"); strcpy(sc.difRelUnit, "ppm");
        for (int p = 0; p < FB_STATS_PAGE_COUNT; p++)
        {
            sc.page = (uint8_t)p;
            fbComposeStats(pages[p], sc);
        }
        FbStatsRows rows = fbStatsRows();
        int topRows = rows.baselineTop + 3;
        bool sameTop = true;
        for (int y = 0; y < topRows; y++)
            for (int x = 0; x < FB_WIDTH; x++)
                if (fbGet(pages[1], x, y) != fbGet(pages[0], x, y) || fbGet(pages[2], x, y) != fbGet(pages[0], x, y)) sameTop = false;
        CHECK(sameTop, "the top line is the same on all three pages");
        for (int p = 1; p < 3; p++)
        {
            int lit = 0, diff = 0;
            for (int y = topRows; y < FB_HEIGHT; y++)
                for (int x = 0; x < FB_WIDTH; x++)
                {
                    if (fbGet(pages[p], x, y) != 0) lit++;
                    if (fbGet(pages[p], x, y) != fbGet(pages[0], x, y)) diff++;
                }
            CHECK(lit > 2000 && diff > 1000, "pages 2 and 3 draw their own rows");
            // an empty gap of at least 8 columns between the two cells of each row
            for (int row = 0; row < 2; row++)
            {
                int base = row ? rows.baselineRow2 : rows.baselineRow1;
                int y0 = base - FONT_STAT.digitHeight - 2, y1 = base + 4;
                int run = 0, best = 0;
                for (int x = FB_WIDTH / 3; x < 2 * FB_WIDTH / 3; x++)
                {
                    bool ink = false;
                    for (int y = y0; y <= y1 && y < FB_HEIGHT; y++) if (fbGet(pages[p], x, y) != 0) ink = true;
                    run = ink ? 0 : run + 1;
                    if (run > best) best = run;
                }
                printf("page %d row %d: gap between the columns %d px\n", p + 1, row + 1, best);
                CHECK(best >= 8, "the two cells of a row do not touch");
            }
        }
        // the right legends stay at one fixed place (where a two-character unit puts MAX / MIN) on every page and for
        // every unit (flipping the pages / units on the instrument made them jump); the left numbers
        // end where page 1's end
        const char *units[] = {"V", "mV", "kO", "uA", "dB"};
        for (const char *u : units)
        {
            char unit[8];
            snprintf(unit, sizeof(unit), "%s", u);
            if (unit[1] == 'O') unit[1] = FONT_CH_OHM;
            if (unit[0] == 'u') unit[0] = FONT_CH_MICRO;
            snprintf(sc.unit, sizeof(sc.unit), "%s", unit);
            int legendX[3][2];
            for (int p = 0; p < 3; p++)
            {
                sc.page = (uint8_t)p;
                fbComposeStats(pages[p], sc);
                for (int row = 0; row < 2; row++)
                {
                    int base = row ? rows.baselineRow2 : rows.baselineRow1;
                    legendX[p][row] = -1;
                    // the leftmost ink of the right half, in the legend's rows (legend font, on the baseline)
                    for (int x = FB_WIDTH / 2 + 4; x < FB_WIDTH && legendX[p][row] < 0; x++)
                        for (int y = base - FONT_LEGEND.digitHeight; y < base; y++)
                            if (fbGet(pages[p], x, y) != 0) { legendX[p][row] = x; break; }
                }
            }
            int expected = fbStatsRightLegendX(); // v25: one fixed place for every unit and page
            for (int p = 0; p < 3; p++)
                for (int row = 0; row < 2; row++)
                {
                    if (abs(legendX[p][row] - expected) > 2)
                        printf("unit %s page %d row %d: legend ink at %d, page 1 legend at %d\n", u, p + 1, row + 1, legendX[p][row], expected);
                    CHECK(abs(legendX[p][row] - expected) <= 2, "the right legends start where MAX / MIN start, on every page");
                }
        }
        snprintf(sc.unit, sizeof(sc.unit), "V");
        FbStatsCell left = fbStatsCellLayout(false, "-1.23451", "V");
        int legendWidth = fbTextWidth(FONT_LEGEND, "MAX");
        CHECK(left.numberRight == legendWidth + FB_STATS_GAP_LEGEND + FB_STATS_NUMBER_CHARS * FONT_STAT.advance, "page 3 left number edge = page 1");
    }

    // 9. theme for the user interface screens: light text on dark, always
    {
        int turned = 0;
        for (int t = 0; t < (int)themeCount; t++)
        {
            KeithleyTheme in = themes[t];
            KeithleyTheme out = fbStatsTheme(in);
            CHECK(fbLuminance565(out.bg) <= fbLuminance565(out.bright), "stats theme: text is never darker than the background");
            if (fbLuminance565(in.bg) > fbLuminance565(in.bright))
            {
                turned++;
                CHECK(out.bright == in.bg, "light theme: text in the theme's background color");
                CHECK(out.indicator == in.ghost, "light theme: legends use the ghost color");
            }
            else
            {
                CHECK(out.bright == in.bright && out.indicator == in.indicator && out.ghost == in.ghost, "dark theme: colors unchanged");
            }
            CHECK(out.bg == RGB565(0, 0, 0), "v12: background always black");
            // the palette built from it still has exact end points
            uint16_t p[64];
            fbBuildPalette(out, p);
            CHECK(p[0] == out.bg && p[fbRampIndex(FB_INK_BRIGHT, 255)] == out.bright && p[fbRampIndex(FB_INK_INDICATOR, 255)] == out.indicator, "stats palette end points");
        }
        printf("themes turned around for the statistics screen: %d of %d\n", turned, (int)themeCount);
        CHECK(turned >= 1, "at least one light theme exists to exercise this");
    }
    // 10. v23: SD! in place of BAT - ghost / lit differ only inside the BAT box; BAT lit by the K197 wins
    {
        static uint8_t bufB[FB_BYTES];
        FbLcdScene s;
        memset(s.segBits, 0, 6); s.dpPos = -1; s.indicatorBits = 0; s.unit = KEITHLEY_UNIT_NONE;
        s.sdLit = false;
        fbComposeLcd(bufA, s);
        s.sdLit = true;
        fbComposeLcd(bufB, s);
        int bx = keithley_ind_x[KEITHLEY_IND_BAT], by = keithley_ind_y[KEITHLEY_IND_BAT];
        int bw = keithley_ind_w[KEITHLEY_IND_BAT], bh = keithley_ind_h[KEITHLEY_IND_BAT];
        int inside = 0, outside = 0;
        for (int y = 0; y < FB_HEIGHT; y++)
            for (int x = 0; x < FB_WIDTH; x++)
                if (fbGet(bufA, x, y) != fbGet(bufB, x, y))
                {
                    if (x >= bx && x < bx + bw && y >= by && y < by + bh) inside++; else outside++;
                }
        printf("SD! lit vs ghost: %d pixels differ inside the BAT box, %d outside\n", inside, outside);
        CHECK(inside > 100 && outside == 0, "SD! changes only the BAT place");
        s.indicatorBits = (1u << KEITHLEY_IND_BAT);
        s.sdLit = false;
        fbComposeLcd(bufA, s);
        s.sdLit = true;
        fbComposeLcd(bufB, s);
        CHECK(memcmp(bufA, bufB, FB_BYTES) == 0, "BAT lit by the K197: SD! does not show");
    }
    // 11. v23: scroll marks of the settings screens (top right = more above, bottom = more below)
    {
        static uint8_t bufB[FB_BYTES];
        FbSetupScene sc;
        memset(&sc, 0, sizeof(sc));
        strcpy(sc.title, "DMM - NASTAVENI");
        for (int r = 0; r < FB_SETUP_ROWS; r++) { strcpy(sc.labels[r], "X"); strcpy(sc.values[r], "Y"); }
        sc.rowCount = FB_SETUP_ROWS;
        fbComposeSetup(bufA, sc);
        sc.moreAbove = true;
        sc.moreBelow = true;
        fbComposeSetup(bufB, sc);
        int topDiff = 0, bottomDiff = 0, elsewhere = 0;
        int midY = (fbHelpBaseline(1) + fbHelpBaseline(FB_SETUP_ROWS)) / 2;
        for (int y = 0; y < FB_HEIGHT; y++)
            for (int x = 0; x < FB_WIDTH; x++)
                if (fbGet(bufA, x, y) != fbGet(bufB, x, y))
                {
                    if (x < FB_WIDTH - FB_SETUP_SCROLL_W - 3) elsewhere++;
                    else if (y < midY) topDiff++;
                    else bottomDiff++;
                }
        printf("scroll marks: %d px at the top, %d px at the bottom, %d elsewhere\n", topDiff, bottomDiff, elsewhere);
        CHECK(topDiff >= 15 && bottomDiff >= 15 && elsewhere == 0, "scroll marks at the right edge only");
    }
    printf(failures ? "FAILURES: %d\n" : "ALL OK\n", failures);
    return failures != 0;
}
