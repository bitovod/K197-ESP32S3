// framebuffer.h
// Pure framebuffer logic for the replacement LCD screen: a packed
// 6 bits-per-pixel indexed pixel buffer, the 64-entry palette built
// from a color theme, and the routines that compose the emulated
// Keithley 197 LCD (digits, indicators, minus sign, unit) into such
// a buffer.
//
// Nothing in here touches the TFT, FreeRTOS or any global state - it
// only needs keithley_shapes.h (artwork), font_data.h (text) and
// config.h (KeithleyTheme).
// That is deliberate: display.ino owns the buffers, the tasks and the
// panel, this file is just the math, so it can also be compiled and
// tested on a PC (see docs/DEVELOPMENT.md).
//
// Everything is header-only (static inline) because every .ino of a
// sketch is merged into one translation unit anyway, and because the
// Arduino auto-prototype pass is happier with types living in headers
// (see the notes in buttons.ino).
//
// ---------------------------------------------------------------
// Pixel format
// ---------------------------------------------------------------
// 428x94 pixels, 6 bits each. Four pixels share a 3-byte group
// (24 bits, most significant pixel first):
//     byte0 = p0[5:0] p1[5:4]
//     byte1 = p1[3:0] p2[5:2]
//     byte2 = p2[1:0] p3[5:0]
// 428 is divisible by 4, so a row is exactly 107 groups = 321 bytes
// with no padding, and a whole buffer is 321 * 94 = 30174 bytes.
//
// ---------------------------------------------------------------
// Palette layout (64 entries, rebuilt for every theme)
// ---------------------------------------------------------------
//   index 0          bg (exact)
//   index 1..21      bg -> ghost ramp,     21 = ghost (exact)
//   index 22..42     bg -> indicator ramp, 42 = indicator (exact)
//   index 43..63     bg -> bright ramp,    63 = bright (exact)
// The artwork's coverage masks (0-255, antialiased) are quantized
// onto the ramp of whichever ink color the element is drawn in. The
// solid digit segments only ever use the exact end points, so large
// flat areas have no quantization error at all. Max error on a ramp
// is half a step: 1/42 of (ink - bg), i.e. about 6 of 255.

#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <Arduino.h>
#include <math.h>
#include "keithley_shapes.h"
#include "font_data.h"
#include "font_pixel.h"
#include "tag_shape.h" // v18: "TAG" in place of REM
#include "config.h"

#define FB_WIDTH            KEITHLEY_SCREEN_WIDTH
#define FB_HEIGHT           KEITHLEY_SCREEN_HEIGHT
#define FB_GROUP_PIXELS     4
#define FB_GROUP_BYTES      3
#define FB_STRIDE_BYTES     ((FB_WIDTH / FB_GROUP_PIXELS) * FB_GROUP_BYTES)
#define FB_BYTES            (FB_STRIDE_BYTES * FB_HEIGHT)
#define FB_PALETTE_SIZE     64
#define FB_RAMP_STEPS       21

static_assert((FB_WIDTH % FB_GROUP_PIXELS) == 0, "framebuffer width must be a multiple of 4");
static_assert((1 + 3 * FB_RAMP_STEPS) == FB_PALETTE_SIZE, "palette = bg + three ramps");

// Which ink color an antialiased element is drawn with. The numeric
// values select the ramp inside the palette.
#define FB_INK_GHOST        0
#define FB_INK_INDICATOR    1
#define FB_INK_BRIGHT       2

// The minus sign's bit inside an indicator bitfield - must stay equal
// to KEITHLEY_BIT_MINUS in display.h (display.ino has a static_assert
// that checks this). Bit i (0..KEITHLEY_INDICATOR_COUNT-1) is
// indicator i of enum KeithleyIndicator.
#define FB_BIT_MINUS        (1u << KEITHLEY_INDICATOR_COUNT)

// v17: the limit alarm (alarm.h) on the LCD replica, in the free area above
// the minus sign (as wide as the minus, from the top edge of the digits down
// to a gap above it):
//   NONE         nothing (alarm off for the function being measured)
//   ARMED_UPPER  outline of a narrow triangle pointing up, indicator color -
//                the upper limit is watched, the reading is inside
//   ARMED_LOWER  the same pointing down - the lower limit is watched
//   ARMED_BOTH   outlines of two triangles above each other (up over down),
//                as wide as the single one and half as high, indicator color -
//                both limits are watched
//   HIGH / LOW   the narrow triangle filled, bright, pointing up / down - the
//                reading is above / below a limit (alarm.ino makes it blink by
//                switching between this and NONE)
#define FB_ALARM_NONE        0
#define FB_ALARM_ARMED_UPPER 1
#define FB_ALARM_ARMED_LOWER 2
#define FB_ALARM_ARMED_BOTH  3
#define FB_ALARM_HIGH        4
#define FB_ALARM_LOW         5
#define FB_ALARM_CODES       6
#define FB_ALARM_X       KEITHLEY_MINUS_X // 50: the drawing area is as wide as the minus sign
#define FB_ALARM_W       KEITHLEY_MINUS_W // 23
#define FB_ALARM_TOP     12 // top edge of the digits and of "AUTO"
#define FB_ALARM_H       27 // down to row 38: 4 rows of gap above the minus (row 43)
#define FB_ALARM_NARROW  15.0f // width of the single (narrow) triangle, pixels
#define FB_ALARM_GAP     3.0f  // gap between the two flat triangles of ARMED_BOTH
#define FB_ALARM_STROKE  1.5f  // line width of the outlines
static_assert(FB_ALARM_TOP + FB_ALARM_H < KEITHLEY_MINUS_Y, "alarm triangle stays above the minus sign");

// Everything the emulated LCD needs to draw one complete picture.
struct FbLcdScene
{
    uint8_t segBits[KEITHLEY_DIGIT_COUNT]; // raw 7-segment bits per digit, see display.h
    int8_t dpPos;                           // digit that shows the decimal point, -1 = none
    uint16_t indicatorBits;                 // indicators + minus, see FB_BIT_MINUS
    uint8_t unit;                           // KEITHLEY_UNIT_*, KEITHLEY_UNIT_NONE = hidden
    uint8_t alarm = FB_ALARM_NONE;          // v17: the limit alarm's triangle above the minus, FB_ALARM_*
    bool tagLit = false;                    // v18: "TAG" (a marker was just set) lit instead of ghost
};

// Everything the quick statistics screen needs to draw one picture:
// pre-formatted text, so the pixel code never deals with numbers, units
// or rounding (see ui.ino for how the strings are produced). All
// strings are plain C strings; the unit string may contain
// FONT_CH_MICRO / FONT_CH_OHM (font_data.h).
#define FB_STATS_TEXT_LEN 20
struct FbStatsScene
{
    char live[FB_STATS_TEXT_LEN];     // the live reading, number only (e.g. "-1.23456")
    char average[FB_STATS_TEXT_LEN];
    char median[FB_STATS_TEXT_LEN];
    char maximum[FB_STATS_TEXT_LEN];
    char minimum[FB_STATS_TEXT_LEN];
    char unit[8];                     // shared by all five numbers, e.g. "mV"
    char interval[12];                // the statistics interval, e.g. "15 MIN" (at most 6 characters are laid out)
    char dateTime[40];                // e.g. "2026-10-06  14:03:07", empty = nothing shown (size: room for any value the formatter could be given)
};

// One complete frame travelling from the render task to the blit
// task: the pixels, the palette they were rendered with (so a theme
// change can never be shown with the wrong palette) and where on the
// panel they go.
struct FbSlot
{
    uint16_t palette[FB_PALETTE_SIZE];
    int16_t yOffset;
    uint8_t px[FB_BYTES];
};

// ---------------------------------------------------------------
// Palette
// ---------------------------------------------------------------

// Unpacks a RGB565 color into 8-bit channels (low bits zero).
static inline void fbUnpack565(uint16_t c, uint8_t &r, uint8_t &g, uint8_t &b)
{
    r = (c >> 8) & 0xF8;
    g = (c >> 3) & 0xFC;
    b = (c << 3) & 0xF8;
}

// Blends bg -> ink proportionally to coverage (0-255). Same math as
// the blend565() the direct-drawing display.ino of v6
// used, so the two renderers can be compared pixel by pixel.
static inline uint16_t fbBlend565(uint16_t bg, uint16_t ink, uint8_t coverage)
{
    uint8_t br, bgc, bb, ir, ig, ib;
    fbUnpack565(bg, br, bgc, bb);
    fbUnpack565(ink, ir, ig, ib);
    uint8_t r = br + ((int16_t)(ir - br) * coverage) / 255;
    uint8_t g = bgc + ((int16_t)(ig - bgc) * coverage) / 255;
    uint8_t b = bb + ((int16_t)(ib - bb) * coverage) / 255;
    return RGB565(r, g, b);
}

// Palette index for an antialiased pixel: ink selects the ramp,
// coverage (0-255) the position on it. Coverage 0 is always the
// shared bg entry 0, coverage 255 always the exact ink color.
static inline uint8_t fbRampIndex(uint8_t ink, uint8_t coverage)
{
    uint8_t step = (uint8_t)(((uint16_t)coverage * FB_RAMP_STEPS + 127) / 255);
    if (step == 0)
    {
        return 0;
    }
    return (uint8_t)(1 + ink * FB_RAMP_STEPS + (step - 1));
}

// Fills pal[] for one theme, see the layout at the top of this file.
static inline void fbBuildPalette(const KeithleyTheme &th, uint16_t pal[FB_PALETTE_SIZE])
{
    const uint16_t inks[3] = {th.ghost, th.indicator, th.bright};
    pal[0] = th.bg;
    for (uint8_t ink = 0; ink < 3; ink++)
    {
        for (uint8_t step = 1; step <= FB_RAMP_STEPS; step++)
        {
            uint8_t coverage = (uint8_t)(((uint16_t)step * 255 + FB_RAMP_STEPS / 2) / FB_RAMP_STEPS);
            pal[1 + ink * FB_RAMP_STEPS + (step - 1)] = fbBlend565(th.bg, inks[ink], coverage);
        }
    }
}

// Brightness of a RGB565 color (0-255, the usual weighting of the channels).
static inline uint16_t fbLuminance565(uint16_t c)
{
    uint8_t r, g, b;
    fbUnpack565(c, r, g, b);
    return (uint16_t)(((uint32_t)r * 299 + (uint32_t)g * 587 + (uint32_t)b * 114) / 1000);
}

// The theme as the user interface screens (statistics, help - not the
// emulated LCD) use it. Since v12 the background is ALWAYS black,
// whatever the theme says. Text (numbers, units, help text) uses the
// theme's `bright`, legends/triangles/date/time and help headings its
// `indicator`. A theme whose background is lighter than its text (the
// LCD look-alikes, and inverted themes like ORG2) is used the other way
// round on the black background: text in the theme's `bg`, legends etc.
// in its `ghost` (the closest relative of that light background). The
// colors themselves are tuned in themes[] (config.h), not here. The
// result is a theme like any other, handed to fbBuildPalette() - the
// screens themselves know nothing about it.
static inline KeithleyTheme fbStatsTheme(const KeithleyTheme &th)
{
    KeithleyTheme out = th;
    out.bg = RGB565(0, 0, 0);
    if (fbLuminance565(th.bg) > fbLuminance565(th.bright))
    {
        out.bright = th.bg;
        out.indicator = th.ghost;
        out.ghost = th.ghost;
    }
    return out;
}

// ---------------------------------------------------------------
// Packed 6bpp pixel access
// ---------------------------------------------------------------

// Sets one pixel. No bounds check - callers clip (see fbDrawMask()).
static inline void fbPut(uint8_t *fb, int16_t x, int16_t y, uint8_t idx)
{
    uint8_t *g = fb + (uint32_t)y * FB_STRIDE_BYTES + (uint32_t)(x >> 2) * FB_GROUP_BYTES;
    uint8_t shift = (uint8_t)(18 - 6 * (x & 3));
    uint32_t v = ((uint32_t)g[0] << 16) | ((uint32_t)g[1] << 8) | (uint32_t)g[2];
    v = (v & ~((uint32_t)0x3F << shift)) | ((uint32_t)(idx & 0x3F) << shift);
    g[0] = (uint8_t)(v >> 16);
    g[1] = (uint8_t)(v >> 8);
    g[2] = (uint8_t)v;
}

// Reads one pixel back (used by the PC-side tests, and handy for
// debugging).
static inline uint8_t fbGet(const uint8_t *fb, int16_t x, int16_t y)
{
    const uint8_t *g = fb + (uint32_t)y * FB_STRIDE_BYTES + (uint32_t)(x >> 2) * FB_GROUP_BYTES;
    uint8_t shift = (uint8_t)(18 - 6 * (x & 3));
    uint32_t v = ((uint32_t)g[0] << 16) | ((uint32_t)g[1] << 8) | (uint32_t)g[2];
    return (uint8_t)((v >> shift) & 0x3F);
}

// Sets every pixel to one palette index. Four identical pixels pack
// into the same 3 bytes in every group, so this is a plain repeat.
static inline void fbFill(uint8_t *fb, uint8_t idx)
{
    uint32_t p = idx & 0x3F;
    uint32_t v = p * 0x041041UL;
    uint8_t b0 = (uint8_t)(v >> 16);
    uint8_t b1 = (uint8_t)(v >> 8);
    uint8_t b2 = (uint8_t)v;
    for (uint32_t i = 0; i < FB_BYTES; i += FB_GROUP_BYTES)
    {
        fb[i] = b0;
        fb[i + 1] = b1;
        fb[i + 2] = b2;
    }
}

// Converts `rows` consecutive rows starting at row y0 into RGB565
// through the palette, ready to be handed to the TFT. out must hold
// FB_WIDTH * rows entries.
static inline void fbUnpackRows(const uint8_t *fb, uint16_t y0, uint16_t rows,
                                const uint16_t *pal, uint16_t *out)
{
    for (uint16_t row = 0; row < rows; row++)
    {
        const uint8_t *g = fb + (uint32_t)(y0 + row) * FB_STRIDE_BYTES;
        for (uint16_t gx = 0; gx < FB_WIDTH / FB_GROUP_PIXELS; gx++)
        {
            uint32_t v = ((uint32_t)g[0] << 16) | ((uint32_t)g[1] << 8) | (uint32_t)g[2];
            out[0] = pal[(v >> 18) & 0x3F];
            out[1] = pal[(v >> 12) & 0x3F];
            out[2] = pal[(v >> 6) & 0x3F];
            out[3] = pal[v & 0x3F];
            out += FB_GROUP_PIXELS;
            g += FB_GROUP_BYTES;
        }
    }
}

// ---------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------

// Draws a w x h coverage mask (PROGMEM, 0-255 per pixel) with the
// given ink at (x, y). Opaque, exactly like the old drawMasked(): a
// pixel with coverage 0 overwrites whatever was there with bg. Clips
// against the framebuffer.
static inline void fbDrawMask(uint8_t *fb, const uint8_t *mask, int16_t x, int16_t y,
                              uint8_t w, uint8_t h, uint8_t ink)
{
    for (uint8_t row = 0; row < h; row++)
    {
        int16_t py = y + row;
        if (py < 0 || py >= FB_HEIGHT)
        {
            continue;
        }
        for (uint8_t col = 0; col < w; col++)
        {
            int16_t px = x + col;
            if (px < 0 || px >= FB_WIDTH)
            {
                continue;
            }
            uint8_t coverage = pgm_read_byte(mask + (uint16_t)row * w + col);
            fbPut(fb, px, py, fbRampIndex(ink, coverage));
        }
    }
}

// One digit, straight from its raw segment bits (bit0=a, bit1=g,
// bit2=d, bit3=f, bit4=b, bit5=e, bit6=c - see display.h). The digit
// masks are segment-ID maps, not coverage: lit segment = exact
// bright, unlit = exact ghost, outside the glyph = bg. The decimal
// point pixels (KEITHLEY_SEG_DOT) inside the masks are deliberately
// NOT drawn - the dot is a separate, larger shape, see fbDrawDot().
static inline void fbDrawDigit(uint8_t *fb, uint8_t position, uint8_t segBits)
{
    int16_t x0 = (int16_t)pgm_read_word(&keithley_digit_x[position]);
    int16_t y0 = (int16_t)pgm_read_word(&keithley_digit_y[position]);
    const uint8_t *mask = keithley_mask_table[position];
    const uint8_t idxBright = fbRampIndex(FB_INK_BRIGHT, 255);
    const uint8_t idxGhost = fbRampIndex(FB_INK_GHOST, 255);

    for (uint8_t row = 0; row < KEITHLEY_GLYPH_HEIGHT; row++)
    {
        int16_t py = y0 + row;
        if (py < 0 || py >= FB_HEIGHT)
        {
            continue;
        }
        for (uint8_t col = 0; col < KEITHLEY_GLYPH_WIDTH; col++)
        {
            int16_t px = x0 + col;
            if (px < 0 || px >= FB_WIDTH)
            {
                continue;
            }
            uint8_t id = pgm_read_byte(mask + (uint16_t)row * KEITHLEY_GLYPH_WIDTH + col);
            uint8_t idx;
            if (id == KEITHLEY_SEG_BLACK || id == KEITHLEY_SEG_DOT)
            {
                idx = 0;
            }
            else
            {
                idx = (segBits & (1 << (id - 1))) ? idxBright : idxGhost;
            }
            fbPut(fb, px, py, idx);
        }
    }
}

// The decimal point to the right of digit `position`, drawn on top of
// that digit's cell: exact bright when lit, exact ghost when not (no
// antialiasing, like the segments). Shape data: keithley_dot_row_x/
// keithley_dot_row_width in keithley_shapes.h. The shape extends
// below the digit's glyph rectangle, which is why it cannot live in
// the digit masks.
static inline void fbDrawDot(uint8_t *fb, uint8_t position, bool lit)
{
    int16_t x0 = (int16_t)pgm_read_word(&keithley_digit_x[position]);
    int16_t y0 = (int16_t)pgm_read_word(&keithley_digit_y[position]);
    uint8_t idx = fbRampIndex(lit ? FB_INK_BRIGHT : FB_INK_GHOST, 255);

    for (uint8_t row = 0; row < KEITHLEY_DOT_ROW_COUNT; row++)
    {
        int16_t py = y0 + KEITHLEY_DOT_Y + row;
        if (py < 0 || py >= FB_HEIGHT)
        {
            continue;
        }
        int16_t xStart = x0 + (int16_t)pgm_read_byte(&keithley_dot_row_x[row]);
        int16_t w = pgm_read_byte(&keithley_dot_row_width[row]);
        for (int16_t px = xStart; px < xStart + w; px++)
        {
            if (px >= 0 && px < FB_WIDTH)
            {
                fbPut(fb, px, py, idx);
            }
        }
    }
}

// v17: one triangle for fbAlarmCoverage(), in pixel-corner coordinates of the
// alarm area (0,0 = its top left corner).
struct FbAlarmTri
{
    float x[3];
    float y[3];
};

// Signed distance of (px,py) from the edge a->b, positive on the side of the
// third vertex c.
static inline float fbAlarmEdgeDistance(float ax, float ay, float bx, float by, float cx, float cy, float px, float py)
{
    float nx = ay - by;
    float ny = bx - ax;
    float length = sqrtf(nx * nx + ny * ny);
    if (length <= 0.0f)
    {
        return 0.0f;
    }
    float d = ((px - ax) * nx + (py - ay) * ny) / length;
    float side = (cx - ax) * nx + (cy - ay) * ny;
    return (side < 0.0f) ? -d : d;
}

// True if the point is inside the triangle and - for an outline - closer
// than FB_ALARM_STROKE to one of its edges.
static inline bool fbAlarmInside(const FbAlarmTri &t, float px, float py, bool outline)
{
    float minimum = 1e9f;
    for (uint8_t e = 0; e < 3; e++)
    {
        uint8_t a = e, b = (uint8_t)((e + 1) % 3), c = (uint8_t)((e + 2) % 3);
        float d = fbAlarmEdgeDistance(t.x[a], t.y[a], t.x[b], t.y[b], t.x[c], t.y[c], px, py);
        if (d < 0.0f)
        {
            return false;
        }
        if (d < minimum)
        {
            minimum = d;
        }
    }
    return !outline || minimum < FB_ALARM_STROKE;
}

// Coverage masks (0..255, FB_ALARM_W x FB_ALARM_H) of the five symbols, 8x8
// supersampled, built on first use (the render task only; 3 kB of RAM).
static inline const uint8_t *fbAlarmMask(uint8_t code)
{
    static uint8_t masks[FB_ALARM_CODES][FB_ALARM_H * FB_ALARM_W];
    static bool built = false;
    if (!built)
    {
        const float h = FB_ALARM_H, cx = FB_ALARM_W / 2.0f, half = FB_ALARM_NARROW / 2.0f;
        const float mid = h / 2.0f;
        const FbAlarmTri up = {{cx - half, cx + half, cx}, {h, h, 0.0f}};
        const FbAlarmTri down = {{cx - half, cx + half, cx}, {0.0f, 0.0f, h}};
        // the two of ARMED_BOTH: as wide as the single triangle, each half as high
        const FbAlarmTri upperFlat = {{cx - half, cx + half, cx}, {mid - FB_ALARM_GAP / 2.0f, mid - FB_ALARM_GAP / 2.0f, 0.0f}};
        const FbAlarmTri lowerFlat = {{cx - half, cx + half, cx}, {mid + FB_ALARM_GAP / 2.0f, mid + FB_ALARM_GAP / 2.0f, h}};
        const uint8_t samples = 8;
        for (uint8_t code = 1; code < FB_ALARM_CODES; code++)
        {
            for (uint8_t row = 0; row < FB_ALARM_H; row++)
            {
                for (uint8_t col = 0; col < FB_ALARM_W; col++)
                {
                    uint16_t hits = 0;
                    for (uint8_t sy = 0; sy < samples; sy++)
                    {
                        for (uint8_t sx = 0; sx < samples; sx++)
                        {
                            float px = col + (sx + 0.5f) / samples;
                            float py = row + (sy + 0.5f) / samples;
                            bool in = false;
                            switch (code)
                            {
                                case FB_ALARM_ARMED_UPPER: in = fbAlarmInside(up, px, py, true); break;
                                case FB_ALARM_ARMED_LOWER: in = fbAlarmInside(down, px, py, true); break;
                                case FB_ALARM_ARMED_BOTH:
                                    in = fbAlarmInside(upperFlat, px, py, true) || fbAlarmInside(lowerFlat, px, py, true);
                                    break;
                                case FB_ALARM_HIGH: in = fbAlarmInside(up, px, py, false); break;
                                default: in = fbAlarmInside(down, px, py, false); break; // FB_ALARM_LOW
                            }
                            if (in)
                            {
                                hits++;
                            }
                        }
                    }
                    masks[code][row * FB_ALARM_W + col] =
                        (uint8_t)((hits * 255u + (samples * samples) / 2) / (samples * samples));
                }
            }
        }
        built = true;
    }
    return masks[code];
}

static inline void fbDrawAlarm(uint8_t *fb, uint8_t code)
{
    if (code == FB_ALARM_NONE || code >= FB_ALARM_CODES)
    {
        return;
    }
    uint8_t ink = (code == FB_ALARM_HIGH || code == FB_ALARM_LOW) ? FB_INK_BRIGHT : FB_INK_INDICATOR;
    fbDrawMask(fb, fbAlarmMask(code), FB_ALARM_X, FB_ALARM_TOP, FB_ALARM_W, FB_ALARM_H, ink);
}

// Composes the whole emulated LCD into fb. Same drawing order as the
// old redrawEverything(): background, indicators, minus sign, unit,
// digits (each followed by its decimal point). Because every frame is composed from scratch in this fixed
// order, the minus sign/AC-indicator overlap that the old incremental
// redraw could get wrong (it depended on which of the two had been
// redrawn last) is now always resolved the same way.
static inline void fbComposeLcd(uint8_t *fb, const FbLcdScene &scene)
{
    fbFill(fb, 0);

    for (uint8_t i = 0; i < KEITHLEY_INDICATOR_COUNT; i++)
    {
        int16_t x = (int16_t)pgm_read_word(&keithley_ind_x[i]);
        int16_t y = (int16_t)pgm_read_word(&keithley_ind_y[i]);
        uint8_t w = pgm_read_byte(&keithley_ind_w[i]);
        uint8_t h = pgm_read_byte(&keithley_ind_h[i]);
        bool on = (scene.indicatorBits & (1u << i)) != 0;
        if (i == KEITHLEY_IND_REM && !on)
        {
            // v18: this K197 never gets the GPIB board, so REM is free: unless the K197 lights it (its
            // self-test lights everything), "TAG" stands there - ghost, lit for a moment after a marker.
            fbDrawMask(fb, keithley_tag_mask, (int16_t)(x + (w - KEITHLEY_TAG_W) / 2), y, KEITHLEY_TAG_W,
                       KEITHLEY_TAG_H, scene.tagLit ? FB_INK_INDICATOR : FB_INK_GHOST);
            continue;
        }
        // Off: ghost. On: the dimmer indicator color, except AC,
        // which uses the full digit brightness (same rule as the old
        // drawIndicator()).
        uint8_t ink = on ? ((i == KEITHLEY_IND_AC) ? FB_INK_BRIGHT : FB_INK_INDICATOR)
                         : FB_INK_GHOST;
        fbDrawMask(fb, keithley_ind_mask_table[i], x, y, w, h, ink);
    }

    bool minusOn = (scene.indicatorBits & FB_BIT_MINUS) != 0;
    fbDrawMask(fb, keithley_minus_mask, KEITHLEY_MINUS_X, KEITHLEY_MINUS_Y,
               KEITHLEY_MINUS_W, KEITHLEY_MINUS_H, minusOn ? FB_INK_BRIGHT : FB_INK_GHOST);
    fbDrawAlarm(fb, scene.alarm); // v17

    // A hidden unit is simply not drawn - the buffer is already bg
    // there (the old code did an explicit fillRect(bg) for the same
    // effect).
    if (scene.unit != KEITHLEY_UNIT_NONE && scene.unit < KEITHLEY_UNIT_COUNT)
    {
        fbDrawMask(fb, keithley_unit_mask_table[scene.unit], KEITHLEY_UNIT_X, KEITHLEY_UNIT_Y,
                   KEITHLEY_UNIT_W, KEITHLEY_UNIT_H, FB_INK_BRIGHT);
    }

    for (uint8_t i = 0; i < KEITHLEY_DIGIT_COUNT; i++)
    {
        fbDrawDigit(fb, i, scene.segBits[i]);
        // The last digit has no decimal point (the DMM never lights
        // one there, and its mask never contained one).
        if (i < KEITHLEY_DIGIT_COUNT - 1)
        {
            fbDrawDot(fb, i, i == scene.dpPos);
        }
    }
}

// ---------------------------------------------------------------
// Text
// ---------------------------------------------------------------

// Glyph index inside a font for one character (see font_data.h).
static inline uint8_t fbGlyphIndex(char c)
{
    uint8_t u = (uint8_t)c;
    if (u >= 32 && u <= 126)
    {
        return (uint8_t)(u - 32);
    }
    if (u == (uint8_t)FONT_CH_MICRO)
    {
        return 95;
    }
    if (u == (uint8_t)FONT_CH_OHM)
    {
        return 96;
    }
    return 0; // anything else is a space
}

// Width of a string in pixels (monospaced fonts).
static inline int16_t fbTextWidth(const FontDef &font, const char *text)
{
    return (int16_t)(strlen(text) * font.advance);
}

// Draws text with its baseline at baselineY, first character's cell
// starting at x. Pixels with coverage 0 are left alone (the buffer is
// expected to already hold the background there), so neighbouring
// cells and earlier drawing are never overwritten by a glyph's empty
// corners. Clips against the framebuffer.
static inline void fbDrawText(uint8_t *fb, const FontDef &font, int16_t x, int16_t baselineY,
                              const char *text, uint8_t ink)
{
    int16_t top = baselineY - font.ascent;
    uint16_t cellSize = (uint16_t)font.cellW * font.cellH;
    for (; *text != '\0'; text++, x += font.advance)
    {
        uint8_t glyph = fbGlyphIndex(*text);
        if (glyph == 0)
        {
            continue; // space
        }
        const uint8_t *cell = font.cells + (uint32_t)glyph * cellSize;
        for (uint8_t row = 0; row < font.cellH; row++)
        {
            int16_t py = top + row;
            if (py < 0 || py >= FB_HEIGHT)
            {
                continue;
            }
            for (uint8_t col = 0; col < font.cellW; col++)
            {
                uint8_t coverage = pgm_read_byte(cell + (uint16_t)row * font.cellW + col);
                if (coverage == 0)
                {
                    continue;
                }
                int16_t px = x + col;
                if (px < 0 || px >= FB_WIDTH)
                {
                    continue;
                }
                fbPut(fb, px, py, fbRampIndex(ink, coverage));
            }
        }
    }
}

// Same, but the text ends at rightEdge (exclusive) instead of starting
// at x - for numbers, so the decimal point and the last digit stay in
// the same column as the value changes.
static inline void fbDrawTextRight(uint8_t *fb, const FontDef &font, int16_t rightEdge,
                                   int16_t baselineY, const char *text, uint8_t ink)
{
    fbDrawText(fb, font, (int16_t)(rightEdge - fbTextWidth(font, text)), baselineY, text, ink);
}

// ---------------------------------------------------------------
// Triangles (the interval selector's arrows)
// ---------------------------------------------------------------

// Draws a filled, antialiased triangle: `depth` pixels from the tip to
// the base, `height` pixels along the base, vertically centred on cy
// (which may be fractional, so it can sit exactly on the middle of a
// glyph of even height). pointsLeft: tip on the left, base on the
// right; otherwise the other way round. x is the left edge of the
// bounding box. Coverage is computed by supersampling (8x8 per pixel)
// and goes through the same palette ramps as the glyphs.
static inline void fbDrawTriangle(uint8_t *fb, int16_t x, float cy, uint8_t depth, uint8_t height,
                                  bool pointsLeft, uint8_t ink)
{
    const uint8_t samples = 8;
    float top = cy - height / 2.0f;
    for (int16_t py = (int16_t)floorf(top); py <= (int16_t)ceilf(top + height); py++)
    {
        for (int16_t px = x; px <= x + depth; px++)
        {
            uint16_t hits = 0;
            for (uint8_t sy = 0; sy < samples; sy++)
            {
                for (uint8_t sx = 0; sx < samples; sx++)
                {
                    float fx = (px - x) + (sx + 0.5f) / samples;  // 0 .. depth
                    float fy = (py - top) + (sy + 0.5f) / samples; // 0 .. height
                    if (fx < 0.0f || fx > depth || fy < 0.0f || fy > height)
                    {
                        continue;
                    }
                    float t = pointsLeft ? (fx / depth) : (1.0f - fx / depth); // 0 at the tip, 1 at the base
                    if (fabsf(fy - height / 2.0f) <= t * height / 2.0f)
                    {
                        hits++;
                    }
                }
            }
            if (hits == 0 || px < 0 || px >= FB_WIDTH || py < 0 || py >= FB_HEIGHT)
            {
                continue;
            }
            uint8_t coverage = (uint8_t)((hits * 255u + (samples * samples) / 2) / (samples * samples));
            fbPut(fb, px, py, fbRampIndex(ink, coverage));
        }
    }
}

// ---------------------------------------------------------------
// Quick statistics screen
// ---------------------------------------------------------------
// Meant for reading the statistics, so they get the big type:
//
//   < 15 MIN >         14:03:07 06.10.26           -1.23456  mV
//
//   AVG -1.23401 mV                            MAX -1.20000 mV
//   MED -1.23450 mV                            MIN -1.25000 mV
//
// The top edge holds three things, all touching row 0: on the left the
// interval the statistics are computed over (REL = shorter, dB =
// longer) between two filled triangles, on the right the live reading
// in the same type, and between them, centred in the free space and
// moved one character to the right, date and time in small type. The live reading takes a FIXED width for the
// purpose of centring (an 8 character number and a 2 character unit), so
// the date does not move when the reading gets shorter or longer.
// Below that the four statistics in two cells that are packed against
// the left and right edge of the screen (what the number fields do not
// need is the space between them).
//
// Every unit - the live one and the four statistics - ends flush at the
// right edge of its cell, and each number ends right before its unit,
// so the right edges of the units line up (the live unit and the right
// column's units end at the right edge of the screen) and the decimal
// points of a column line up too. The number field is always 8
// characters wide (sign, 6 digits, dot - the longest a 5 1/2 digit
// value can be), so the layout does not jump when a sign appears.
//
// Colors follow the theme as turned by fbStatsTheme(): background = bg;
// numbers, units and the selected interval = bright; legends, triangles
// and date/time = indicator.
//
// All positions are computed from the metrics of the fonts in
// font_data.h (advance, digit height), none of them is hard-coded for
// one particular font - regenerate font_data.h with another font or
// other digit heights (tools/gen_font.py) and the layout follows.
//
//   FONT_STAT (+ STAT2, STAT3 as ever smaller fallbacks)
//                 the statistics: numbers and units
//   FONT_LIVE     the live number and unit, and the interval text
//   FONT_LEGEND   the legends AVG, MED, MAX, MIN
//   FONT_TINY     date and time

#define FB_STATS_CELL_LIMIT     (FB_WIDTH / 2) // a statistics cell must not be wider than half the screen
#define FB_STATS_NUMBER_CHARS   8   // width of the number field, in characters
#define FB_STATS_UNIT_CHARS     2   // width reserved for the live unit when centring the date, in characters
#define FB_STATS_GAP_LEGEND     2   // legend -> number field
#define FB_STATS_GAP_UNIT       4   // number -> unit
#define FB_STATS_MARGIN_BOTTOM  4   // below the last row (room for the tail of the micro sign)
#define FB_STATS_ARROW_DEPTH    5   // interval selector triangles, tip to base
#define FB_STATS_ARROW_HEIGHT   11  // ... and along the base
#define FB_STATS_ARROW_GAP      5   // triangle <-> interval text
#define FB_STATS_INTERVAL_CHARS 6   // width reserved for the interval text, in characters

// Where the lines of text sit vertically (baselines: the digits end on
// the row above the baseline).
struct FbStatsRows
{
    int16_t baselineTop;  // interval selector and live reading, touching the top edge
    int16_t baselineDate; // date and time, touching the top edge
    int16_t baselineRow1; // AVG / MAX
    int16_t baselineRow2; // MED / MIN
};

static inline FbStatsRows fbStatsRows()
{
    FbStatsRows rows;
    rows.baselineTop = FONT_LIVE.digitHeight;
    rows.baselineDate = FONT_TINY.digitHeight;
    // The free height is split evenly between the three gaps: top line ->
    // row 1, row 1 -> row 2 (the bottom margin is fixed).
    int16_t freeRows = (int16_t)(FB_HEIGHT - FB_STATS_MARGIN_BOTTOM - FONT_LIVE.digitHeight - 2 * FONT_STAT.digitHeight);
    int16_t gap = (freeRows > 2) ? (int16_t)(freeRows / 2) : 1;
    rows.baselineRow1 = (int16_t)(rows.baselineTop + gap + FONT_STAT.digitHeight);
    rows.baselineRow2 = (int16_t)(rows.baselineRow1 + gap + FONT_STAT.digitHeight);
    return rows;
}

// Horizontal extent of the interval selector (triangle, text field,
// triangle) - where the free space for the date begins.
static inline int16_t fbStatsSelectorEnd()
{
    return (int16_t)(FB_STATS_ARROW_DEPTH + FB_STATS_ARROW_GAP + FB_STATS_INTERVAL_CHARS * FONT_LIVE.advance +
                     FB_STATS_ARROW_GAP + FB_STATS_ARROW_DEPTH);
}

// Where the live display (number field + unit) begins when it has its
// fixed width - where the free space for the date ends.
static inline int16_t fbStatsLiveStart()
{
    return (int16_t)(FB_WIDTH - (FB_STATS_NUMBER_CHARS * FONT_LIVE.advance + FB_STATS_GAP_UNIT +
                                 FB_STATS_UNIT_CHARS * FONT_LIVE.advance));
}

// Left edge of the date text: centred in the free space between the
// selector and the live display, then moved one character (of the date's
// own font) to the right, which the author asked for after seeing it on
// the instrument. Never further left than the selector, never so far right
// that it would touch the live display's fixed area (the margin to it
// stays at least FB_STATS_GAP_UNIT).
static inline int16_t fbStatsDateX(const char *text)
{
    int16_t left = fbStatsSelectorEnd();
    int16_t right = fbStatsLiveStart();
    int16_t width = fbTextWidth(FONT_TINY, text);
    int16_t x = (int16_t)(left + ((right - left) - width) / 2 + FONT_TINY.advance);
    if (x + width > right - FB_STATS_GAP_UNIT)
    {
        x = (int16_t)(right - FB_STATS_GAP_UNIT - width);
    }
    return (x < left) ? left : x;
}

// One statistics cell: where it starts and where its parts go.
struct FbStatsCell
{
    const FontDef *font; // the largest of the STAT fonts in which the cell fits
    int16_t x;           // left edge of the cell (the legend starts here)
    int16_t numberRight; // right edge of the number
    int16_t unitX;       // left edge of the unit
};

// Lays out the cell of one column: the left column's cells start at the
// left edge, the right column's end at the right edge of the screen.
// The largest STAT font in which legend + number field + unit fit into
// half the screen is used; if even the smallest does not fit, it is
// used anyway.
static inline FbStatsCell fbStatsCellLayout(bool rightColumn, const char *number, const char *unit)
{
    const FontDef *chain[3] = {&FONT_STAT, &FONT_STAT2, &FONT_STAT3};
    int16_t legendWidth = fbTextWidth(FONT_LEGEND, "MAX");
    int16_t chars = (int16_t)strlen(number);
    if (chars < FB_STATS_NUMBER_CHARS)
    {
        chars = FB_STATS_NUMBER_CHARS;
    }

    FbStatsCell cell;
    cell.font = chain[2];
    int16_t width = 0;
    for (uint8_t i = 0; i < 3; i++)
    {
        width = (int16_t)(legendWidth + FB_STATS_GAP_LEGEND + chars * chain[i]->advance + FB_STATS_GAP_UNIT +
                          fbTextWidth(*chain[i], unit));
        if (width <= FB_STATS_CELL_LIMIT)
        {
            cell.font = chain[i];
            break;
        }
    }
    int16_t unitWidth = fbTextWidth(*cell.font, unit);
    width = (int16_t)(legendWidth + FB_STATS_GAP_LEGEND + chars * cell.font->advance + FB_STATS_GAP_UNIT + unitWidth);
    cell.x = rightColumn ? (int16_t)(FB_WIDTH - width) : 0;
    cell.unitX = (int16_t)(cell.x + width - unitWidth);
    cell.numberRight = (int16_t)(cell.unitX - FB_STATS_GAP_UNIT);
    return cell;
}

static inline void fbDrawStatsCell(uint8_t *fb, bool rightColumn, int16_t baselineY,
                                   const char *legend, const char *number, const char *unit)
{
    FbStatsCell cell = fbStatsCellLayout(rightColumn, number, unit);
    fbDrawText(fb, FONT_LEGEND, cell.x, baselineY, legend, FB_INK_INDICATOR);
    fbDrawTextRight(fb, *cell.font, cell.numberRight, baselineY, number, FB_INK_BRIGHT);
    fbDrawText(fb, *cell.font, cell.unitX, baselineY, unit, FB_INK_BRIGHT);
}

// The interval selector in the top left corner: a triangle pointing
// left, the interval text centred in a fixed field, a triangle pointing
// right. The triangles are centred on the middle of the digits.
static inline void fbDrawIntervalSelector(uint8_t *fb, int16_t baselineY, const char *text)
{
    float cy = baselineY - FONT_LIVE.digitHeight / 2.0f;
    int16_t fieldX = (int16_t)(FB_STATS_ARROW_DEPTH + FB_STATS_ARROW_GAP);
    int16_t fieldWidth = (int16_t)(FB_STATS_INTERVAL_CHARS * FONT_LIVE.advance);
    int16_t textWidth = fbTextWidth(FONT_LIVE, text);
    if (textWidth > fieldWidth)
    {
        textWidth = fieldWidth;
    }
    fbDrawTriangle(fb, 0, cy, FB_STATS_ARROW_DEPTH, FB_STATS_ARROW_HEIGHT, true, FB_INK_INDICATOR);
    fbDrawText(fb, FONT_LIVE, (int16_t)(fieldX + (fieldWidth - textWidth) / 2), baselineY, text, FB_INK_BRIGHT);
    fbDrawTriangle(fb, (int16_t)(fieldX + fieldWidth + FB_STATS_ARROW_GAP), cy, FB_STATS_ARROW_DEPTH,
                   FB_STATS_ARROW_HEIGHT, false, FB_INK_INDICATOR);
}

static inline void fbComposeStats(uint8_t *fb, const FbStatsScene &scene)
{
    fbFill(fb, 0);
    FbStatsRows rows = fbStatsRows();

    // top edge: selector (left), date and time (centre), live reading (right)
    fbDrawIntervalSelector(fb, rows.baselineTop, scene.interval);
    fbDrawText(fb, FONT_TINY, fbStatsDateX(scene.dateTime), rows.baselineDate, scene.dateTime, FB_INK_INDICATOR);
    int16_t liveUnitX = (int16_t)(FB_WIDTH - fbTextWidth(FONT_LIVE, scene.unit));
    fbDrawText(fb, FONT_LIVE, liveUnitX, rows.baselineTop, scene.unit, FB_INK_BRIGHT);
    fbDrawTextRight(fb, FONT_LIVE, (int16_t)(liveUnitX - FB_STATS_GAP_UNIT), rows.baselineTop, scene.live, FB_INK_BRIGHT);

    fbDrawStatsCell(fb, false, rows.baselineRow1, "AVG", scene.average, scene.unit);
    fbDrawStatsCell(fb, true, rows.baselineRow1, "MAX", scene.maximum, scene.unit);
    fbDrawStatsCell(fb, false, rows.baselineRow2, "MED", scene.median, scene.unit);
    fbDrawStatsCell(fb, true, rows.baselineRow2, "MIN", scene.minimum, scene.unit);
}

// ---------------------------------------------------------------
// The help screen (v12, layout reworked in v13)
// ---------------------------------------------------------------
// FB_HELP_LINES lines of plain text in FONT_TINY (the date/time font),
// one color (`bright` of the theme as fbStatsTheme() turns it - i.e. the
// theme's `bg` for an inverted theme), no header, no page number. Each line
// is drawn exactly as given from the left edge; whatever does not fit is
// cut off at the right edge (no wrapping - the help text is written to
// fit, see help_text.h). Which lines are shown is decided by ui.ino.
//
// Vertical layout (v13): the lines fill the whole height. The ink extent
// of the font (highest and lowest inked row over ALL its glyphs, brackets
// and descenders included) is measured once; the first line's highest
// possible ink touches the top row, the last line's lowest possible ink the
// bottom row, and the space left over is shared evenly between the lines
// (baselines rounded to whole pixels). Nothing here is tied to one font -
// regenerate font_data.h and the layout follows.
//
// `underline` draws a 1 px line across the WHOLE width in the gap below
// that line (used under the version/build header), `rightAlign` puts the
// line flush with the right edge (the build stamp).
//
// 6 lines (help_text.h is written for 6 per page): 94 rows leave little
// room - capitals of one line start 1-2 rows below the descenders of the
// line above. Brackets reach 2 rows higher than capitals and may touch.

#define FB_HELP_LINES       6
#define FB_HELP_LINE_CHARS  48 // 47 characters fit (428 / FONT_TINY advance 9) + NUL; anything longer is cut anyway

struct FbHelpScene
{
    char lines[FB_HELP_LINES][FB_HELP_LINE_CHARS]; // "" = empty line
    bool underline[FB_HELP_LINES];
    bool rightAlign[FB_HELP_LINES];
};

// Rows of ink above the baseline (`above`, the baseline row itself not
// counted) and from the baseline down (`below`, baseline row included),
// over every glyph of the font.
static inline void fbFontInkExtent(const FontDef &font, int16_t *above, int16_t *below)
{
    int16_t top = font.cellH;
    int16_t bottom = -1;
    uint32_t cellSize = (uint32_t)font.cellW * font.cellH;
    for (uint16_t g = 0; g < FONT_GLYPH_COUNT; g++)
    {
        const uint8_t *cell = font.cells + g * cellSize;
        for (int16_t row = 0; row < font.cellH; row++)
        {
            for (int16_t col = 0; col < font.cellW; col++)
            {
                if (pgm_read_byte(cell + (uint32_t)row * font.cellW + col) != 0)
                {
                    if (row < top)
                    {
                        top = row;
                    }
                    if (row > bottom)
                    {
                        bottom = row;
                    }
                }
            }
        }
    }
    if (bottom < 0)
    {
        top = 0;
        bottom = 0; // empty font: cannot happen with a generated one
    }
    *above = (int16_t)(font.ascent - top);
    *below = (int16_t)(bottom - font.ascent + 1);
}

// The help font's ink extent, measured on first use (about 18 kB of
// glyph cells walked once; afterwards free).
static inline void fbHelpInkExtent(int16_t *above, int16_t *below)
{
    static int16_t cachedAbove = -1;
    static int16_t cachedBelow = -1;
    if (cachedAbove < 0)
    {
        int16_t a = 0;
        int16_t b = 0;
        fbFontInkExtent(FONT_TINY, &a, &b);
        cachedBelow = b;
        cachedAbove = a; // written last: the "measured" flag
    }
    *above = cachedAbove;
    *below = cachedBelow;
}

// Baseline of help line i (0 = top): first line's ink starts at row 0,
// last line's ink ends at row FB_HEIGHT - 1, even spacing in between.
static inline int16_t fbHelpBaseline(uint8_t i)
{
    int16_t above = 0;
    int16_t below = 0;
    fbHelpInkExtent(&above, &below);
    int32_t span = (int32_t)FB_HEIGHT - above - below; // first baseline -> last baseline
    int32_t offset = (span * i * 2 + (FB_HELP_LINES - 1)) / (2 * (FB_HELP_LINES - 1)); // rounded span * i / (lines - 1)
    return (int16_t)(above + offset);
}

// Row of the underline below help line i. The underlined line (the build
// stamp) has no descenders, so the free rows run from its baseline row down
// to the row above the next line's capitals (digit height = capital
// height); the line sits in the middle of them, rounded towards the header.
static inline int16_t fbHelpUnderlineY(uint8_t i)
{
    int16_t firstFree = fbHelpBaseline(i);
    int16_t lastFree = (i + 1 < FB_HELP_LINES) ? (int16_t)(fbHelpBaseline((uint8_t)(i + 1)) - FONT_TINY.digitHeight - 1)
                                               : (int16_t)(FB_HEIGHT - 1);
    return (int16_t)((firstFree + lastFree) / 2);
}

// Solid horizontal line, clipped.
static inline void fbDrawHLine(uint8_t *fb, int16_t x0, int16_t x1, int16_t y, uint8_t ink)
{
    if (y < 0 || y >= FB_HEIGHT)
    {
        return;
    }
    if (x0 < 0)
    {
        x0 = 0;
    }
    if (x1 > FB_WIDTH)
    {
        x1 = FB_WIDTH;
    }
    uint8_t idx = fbRampIndex(ink, 255);
    for (int16_t x = x0; x < x1; x++)
    {
        fbPut(fb, x, y, idx);
    }
}

// v15: the separator lines of the text screens (help header, settings title)
// are dotted: CONFIG_UI_SEPARATOR_ON pixels on, then CONFIG_UI_SEPARATOR_OFF
// off, starting with "on" at x0 - lighter than a solid line (1 on, 1 off,
// chosen on the real panel). Clipped like fbDrawHLine().
static inline void fbDrawSeparator(uint8_t *fb, int16_t x0, int16_t x1, int16_t y, uint8_t ink)
{
    if (y < 0 || y >= FB_HEIGHT)
    {
        return;
    }
    const int16_t period = CONFIG_UI_SEPARATOR_ON + CONFIG_UI_SEPARATOR_OFF;
    uint8_t idx = fbRampIndex(ink, 255);
    for (int16_t x = x0; x < x1; x++)
    {
        if (x >= 0 && x < FB_WIDTH && ((x - x0) % period) < CONFIG_UI_SEPARATOR_ON)
        {
            fbPut(fb, x, y, idx);
        }
    }
}

static inline void fbComposeHelp(uint8_t *fb, const FbHelpScene &scene)
{
    fbFill(fb, 0);
    for (uint8_t i = 0; i < FB_HELP_LINES; i++)
    {
        if (scene.rightAlign[i])
        {
            fbDrawTextRight(fb, FONT_TINY, FB_WIDTH, fbHelpBaseline(i), scene.lines[i], FB_INK_BRIGHT);
        }
        else
        {
            fbDrawText(fb, FONT_TINY, 0, fbHelpBaseline(i), scene.lines[i], FB_INK_BRIGHT);
        }
        if (scene.underline[i])
        {
            fbDrawSeparator(fb, 0, FB_WIDTH, fbHelpUnderlineY(i), FB_INK_BRIGHT);
        }
    }
}

// ---------------------------------------------------------------
// v15: graph settings screen
// ---------------------------------------------------------------
// The help screen's 6-line layout (FONT_TINY, fbHelpBaseline()): a title
// (underlined), FB_SETUP_ROWS lines "label  value", a key hint (flush right,
// so it is not taken for one more setting). The line
// with the cursor is marked by a triangle pointing right at the start of
// the line and has the value framed by two triangles (pointing left/right:
// REL/dB change it): the left one at a fixed place just before the value
// column, the right one right after the value (follows its length - the
// user's choice after trying the fixed field). Values may be up to
// FB_SETUP_FIELD_CHARS characters long. Used by the graph settings (v14/15)
// and the DMM settings (v15, rowCount 3). Triangles are 5x11 (FB_STATS_ARROW_*) in `indicator`, the text in
// `bright` like the help.

#define FB_SETUP_ROWS        4
#define FB_SETUP_TEXT_LEN    24
#define FB_SETUP_LABEL_COL   1   // column (characters) of the labels: room for the cursor triangle
#define FB_SETUP_VALUE_COL   14  // column (characters) of the values
#define FB_SETUP_FIELD_CHARS 18  // the longest value allowed ("SUPP (SUPPR. ZERO)"; the tests check every one)
#define FB_SETUP_ARROW_GAP   4   // triangle <-> value field, pixels

static_assert(FB_SETUP_VALUE_COL + FB_SETUP_FIELD_CHARS <= FB_HELP_LINE_CHARS - 2, "value field + right triangle fit the line");

struct FbSetupScene
{
    char title[FB_HELP_LINE_CHARS];
    char labels[FB_SETUP_ROWS][FB_SETUP_TEXT_LEN];
    char values[FB_SETUP_ROWS][FB_SETUP_TEXT_LEN];
    char keys[FB_HELP_LINE_CHARS];
    uint8_t cursorRow; // 0..rowCount-1
    uint8_t rowCount;  // lines with a setting (1..FB_SETUP_ROWS); the lines below stay empty
};

static inline void fbComposeSetup(uint8_t *fb, const FbSetupScene &scene)
{
    fbFill(fb, 0);
    const int16_t adv = FONT_TINY.advance;
    fbDrawText(fb, FONT_TINY, 0, fbHelpBaseline(0), scene.title, FB_INK_BRIGHT);
    fbDrawSeparator(fb, 0, FB_WIDTH, fbHelpUnderlineY(0), FB_INK_BRIGHT);
    for (uint8_t r = 0; r < FB_SETUP_ROWS && r < scene.rowCount; r++)
    {
        int16_t baseline = fbHelpBaseline((uint8_t)(1 + r));
        fbDrawText(fb, FONT_TINY, (int16_t)(FB_SETUP_LABEL_COL * adv), baseline, scene.labels[r], FB_INK_BRIGHT);
        fbDrawText(fb, FONT_TINY, (int16_t)(FB_SETUP_VALUE_COL * adv), baseline, scene.values[r], FB_INK_BRIGHT);
        if (r == scene.cursorRow)
        {
            float cy = baseline - FONT_TINY.digitHeight / 2.0f;
            int16_t valueX = (int16_t)(FB_SETUP_VALUE_COL * adv);
            fbDrawTriangle(fb, 1, cy, FB_STATS_ARROW_DEPTH, FB_STATS_ARROW_HEIGHT, false, FB_INK_INDICATOR); // cursor
            fbDrawTriangle(fb, (int16_t)(valueX - FB_SETUP_ARROW_GAP - FB_STATS_ARROW_DEPTH), cy, FB_STATS_ARROW_DEPTH,
                           FB_STATS_ARROW_HEIGHT, true, FB_INK_INDICATOR);
            fbDrawTriangle(fb, (int16_t)(valueX + fbTextWidth(FONT_TINY, scene.values[r]) + FB_SETUP_ARROW_GAP), cy,
                           FB_STATS_ARROW_DEPTH, FB_STATS_ARROW_HEIGHT, false, FB_INK_INDICATOR);
        }
    }
    // the key hint flush right, so it does not read as one more setting (a separator line above it was tried
    // and dropped again: on the real panel it gave odd effects)
    fbDrawTextRight(fb, FONT_TINY, FB_WIDTH, fbHelpBaseline(FB_HELP_LINES - 1), scene.keys, FB_INK_BRIGHT);
}

// ---------------------------------------------------------------
// v14: one-bit 5x7 pixel font (font_pixel.h)
// ---------------------------------------------------------------
// The graph screen's labels must cost as little room as possible: a
// 5x7 pixel font with a 6 pixel advance and 8 rows per line (7 rows of
// capitals/digits, row 7 only for descenders), drawn in one solid
// palette index - no antialiasing, which at this size would only blur
// the glyphs. Same glyph set and order as font_data.h.

#define FB_PIXEL_FONT_ADVANCE 6
#define FB_PIXEL_FONT_ROWS    7 // rows of capitals and digits (descenders use an 8th row)

// Width of a string in pixels, without the empty column after the last
// glyph (0 for an empty string).
static inline int16_t fbPixelTextWidth(const char *text)
{
    size_t n = strlen(text);
    return (n == 0) ? 0 : (int16_t)(n * FB_PIXEL_FONT_ADVANCE - 1);
}

// Draws text with the top of its capitals at row y, first glyph at
// column x, in palette index idx. Only set pixels are drawn (the
// background stays). Clips against the framebuffer.
static inline void fbDrawPixelText(uint8_t *fb, int16_t x, int16_t y, const char *text, uint8_t idx)
{
    for (; *text != '\0'; text++, x += FB_PIXEL_FONT_ADVANCE)
    {
        uint8_t glyph = fbGlyphIndex(*text);
        if (glyph == 0)
        {
            continue; // space
        }
        const uint8_t *cols = font_pixel_5x7 + (uint16_t)glyph * FONT_PIXEL_GLYPH_BYTES;
        for (uint8_t c = 0; c < FONT_PIXEL_GLYPH_BYTES; c++)
        {
            int16_t px = x + c;
            if (px < 0 || px >= FB_WIDTH)
            {
                continue;
            }
            uint8_t bits = pgm_read_byte(cols + c);
            for (uint8_t r = 0; r < 8; r++)
            {
                int16_t py = y + r;
                if ((bits & (1u << r)) != 0 && py >= 0 && py < FB_HEIGHT)
                {
                    fbPut(fb, px, py, idx);
                }
            }
        }
    }
}

// Fills the rectangle [x0, x1) x [y0, y1) with palette index idx, clipped.
static inline void fbFillRect(uint8_t *fb, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t idx)
{
    if (x0 < 0)
    {
        x0 = 0;
    }
    if (y0 < 0)
    {
        y0 = 0;
    }
    if (x1 > FB_WIDTH)
    {
        x1 = FB_WIDTH;
    }
    if (y1 > FB_HEIGHT)
    {
        y1 = FB_HEIGHT;
    }
    for (int16_t y = y0; y < y1; y++)
    {
        for (int16_t x = x0; x < x1; x++)
        {
            fbPut(fb, x, y, idx);
        }
    }
}

// ---------------------------------------------------------------
// v14: graph screen
// ---------------------------------------------------------------
// Layout (428 x 94):
//   rows 0..6    header line in the pixel font: function + unit, interval,
//                Y mode (left); time and live value (right)
//   rows 9..84   the plot: FB_GRAPH_COLUMNS data columns right of the Y axis
//                line at x = FB_GRAPH_X0, Y labels left of it (right aligned,
//                up to FB_GRAPH_YLABEL_CHARS characters)
//   row 85       X axis line
//   rows 87..93  X labels (time relative to now, centred on their grid line)
// Everything is precomputed by graph.ino into an FbGraphScene - rows of
// every column, grid positions, label texts - so this part only draws.
//
// Colors: everything on the UI palette (fbStatsTheme(), black background).
// Labels and header legends in `indicator`, numbers in `bright`, the trace
// (average) in `bright`, the min..max band and the grid/axes as fixed steps
// of the ramps (CONFIG_GRAPH_*_LEVEL in config.h) - no new palette entries.

#define FB_GRAPH_COLUMNS      384
#define FB_GRAPH_X0           (FB_WIDTH - FB_GRAPH_COLUMNS - 1) // the Y axis line; column i is drawn at FB_GRAPH_X0 + 1 + i
#define FB_GRAPH_TOP          9   // first row of the plot
#define FB_GRAPH_BOTTOM       84  // last row of the plot
#define FB_GRAPH_AXIS_Y       85  // the X axis line
#define FB_GRAPH_XLABEL_Y     87  // top row of the X labels
#define FB_GRAPH_YLABEL_CHARS 7
#define FB_GRAPH_MAX_YTICKS   8
#define FB_GRAPH_MAX_XTICKS   12
#define FB_GRAPH_LABEL_LEN    12
#define FB_GRAPH_TEXT_LEN     24

static_assert(FB_GRAPH_X0 >= FB_GRAPH_YLABEL_CHARS * FB_PIXEL_FONT_ADVANCE, "room for the Y labels");

// FbGraphColumn.flags
#define FB_GRAPH_COL_DATA     0x01 // the column has values (yTop/yBottom/yAvg valid)
#define FB_GRAPH_COL_OL       0x02 // an overload (OL) reading fell into this column
#define FB_GRAPH_COL_CONNECT  0x04 // join the average to the previous column with data (no gap in between)
#define FB_GRAPH_COL_CLIP_HI  0x08 // values above the top of the plot (SUPP mode)
#define FB_GRAPH_COL_CLIP_LO  0x10 // values below the bottom of the plot
#define FB_GRAPH_COL_MARK     0x20 // v18: a marked record (TAG) fell into this column - triangle at the top edge
#define FB_GRAPH_MARK_W       9    // the marker triangle, pixels (odd: centred on its column)
#define FB_GRAPH_MARK_H       6

struct FbGraphColumn
{
    int16_t yTop;    // row of the maximum (clamped into the plot)
    int16_t yBottom; // row of the minimum (clamped)
    int16_t yAvg;    // row of the average (clamped)
    uint8_t flags;
};

struct FbGraphTick
{
    int16_t pos;                     // row (Y ticks) or column (X ticks) of the grid line
    char label[FB_GRAPH_LABEL_LEN];  // "" = grid line without a label
};

struct FbGraphScene
{
    char headLeft[FB_GRAPH_TEXT_LEN];     // function + unit, e.g. "DCV mV"
    char headInterval[FB_GRAPH_TEXT_LEN]; // e.g. "1 HOUR"
    char headMode[FB_GRAPH_TEXT_LEN];     // e.g. "AUTO", "SUPP ..." while the log is being read
    char headTime[FB_GRAPH_TEXT_LEN];     // e.g. "14:03:07"
    char headLive[FB_GRAPH_TEXT_LEN];     // live value + unit, e.g. "-1.23456 mV"
    char message[FB_GRAPH_TEXT_LEN];      // shown in the middle of the plot, "" = none (e.g. "NO DATA")
    bool drawEnvelope;                    // the min..max band
    bool drawAverage;                     // the average line
    uint8_t yTickCount;
    FbGraphTick yTicks[FB_GRAPH_MAX_YTICKS];
    uint8_t xTickCount;
    FbGraphTick xTicks[FB_GRAPH_MAX_XTICKS];
    FbGraphColumn cols[FB_GRAPH_COLUMNS];
};

static inline uint8_t fbGraphGridIndex()
{
    return fbRampIndex(FB_INK_INDICATOR, CONFIG_GRAPH_GRID_LEVEL);
}
static inline uint8_t fbGraphAxisIndex()
{
    return fbRampIndex(FB_INK_INDICATOR, CONFIG_GRAPH_AXIS_LEVEL);
}
static inline uint8_t fbGraphEnvelopeIndex()
{
    return fbRampIndex(FB_INK_BRIGHT, CONFIG_GRAPH_ENVELOPE_LEVEL);
}

// Vertical run of pixels from row ya to row yb (either order), column x.
static inline void fbGraphVRun(uint8_t *fb, int16_t x, int16_t ya, int16_t yb, uint8_t idx)
{
    if (ya > yb)
    {
        int16_t t = ya;
        ya = yb;
        yb = t;
    }
    for (int16_t y = ya; y <= yb; y++)
    {
        fbPut(fb, x, y, idx);
    }
}

// v18: coverage of the marker triangle (point down), 8x8 supersampled once.
static inline const uint8_t *fbGraphMarkMask()
{
    static uint8_t mask[FB_GRAPH_MARK_W * FB_GRAPH_MARK_H];
    static bool built = false;
    if (!built)
    {
        const uint8_t samples = 8;
        const float half = FB_GRAPH_MARK_W / 2.0f;
        for (uint8_t row = 0; row < FB_GRAPH_MARK_H; row++)
        {
            for (uint8_t col = 0; col < FB_GRAPH_MARK_W; col++)
            {
                uint16_t hits = 0;
                for (uint8_t sy = 0; sy < samples; sy++)
                {
                    float fy = row + (sy + 0.5f) / samples; // 0 = base (top) .. FB_GRAPH_MARK_H = tip
                    float width = half * (1.0f - fy / FB_GRAPH_MARK_H);
                    for (uint8_t sx = 0; sx < samples; sx++)
                    {
                        float fx = col + (sx + 0.5f) / samples - half;
                        if (fabsf(fx) <= width)
                        {
                            hits++;
                        }
                    }
                }
                mask[row * FB_GRAPH_MARK_W + col] = (uint8_t)((hits * 255u + (samples * samples) / 2) / (samples * samples));
            }
        }
        built = true;
    }
    return mask;
}

// The marker triangle over column x: only pixels it covers are written (the grid and the data around stay).
static inline void fbGraphDrawMark(uint8_t *fb, int16_t x)
{
    const uint8_t *mask = fbGraphMarkMask();
    for (uint8_t row = 0; row < FB_GRAPH_MARK_H; row++)
    {
        for (uint8_t col = 0; col < FB_GRAPH_MARK_W; col++)
        {
            uint8_t coverage = mask[row * FB_GRAPH_MARK_W + col];
            int16_t px = (int16_t)(x - FB_GRAPH_MARK_W / 2 + col);
            if (coverage != 0 && fbRampIndex(FB_INK_BRIGHT, coverage) != 0 && px > FB_GRAPH_X0 && px < FB_WIDTH)
            {
                fbPut(fb, px, (int16_t)(FB_GRAPH_TOP + row), fbRampIndex(FB_INK_BRIGHT, coverage));
            }
        }
    }
}

static inline void fbComposeGraph(uint8_t *fb, const FbGraphScene &scene)
{
    fbFill(fb, 0);
    const uint8_t legend = fbRampIndex(FB_INK_INDICATOR, 255);
    const uint8_t text = fbRampIndex(FB_INK_BRIGHT, 255);
    const uint8_t grid = fbGraphGridIndex();
    const uint8_t axis = fbGraphAxisIndex();
    const uint8_t band = fbGraphEnvelopeIndex();
    const int16_t gap = 2 * FB_PIXEL_FONT_ADVANCE; // two characters between header items

    // ---- header ----
    int16_t x = 0;
    fbDrawPixelText(fb, x, 0, scene.headLeft, text);
    x = (int16_t)(x + fbPixelTextWidth(scene.headLeft) + 1 + gap);
    fbDrawPixelText(fb, x, 0, scene.headInterval, legend);
    x = (int16_t)(x + fbPixelTextWidth(scene.headInterval) + 1 + gap);
    fbDrawPixelText(fb, x, 0, scene.headMode, legend);
    int16_t liveX = (int16_t)(FB_WIDTH - fbPixelTextWidth(scene.headLive));
    fbDrawPixelText(fb, liveX, 0, scene.headLive, text);
    int16_t timeX = (int16_t)(liveX - gap - 1 - fbPixelTextWidth(scene.headTime));
    fbDrawPixelText(fb, timeX, 0, scene.headTime, legend);

    // ---- grid and labels ----
    for (uint8_t i = 0; i < scene.yTickCount && i < FB_GRAPH_MAX_YTICKS; i++)
    {
        const FbGraphTick &t = scene.yTicks[i];
        if (t.pos < FB_GRAPH_TOP || t.pos > FB_GRAPH_BOTTOM)
        {
            continue;
        }
        for (int16_t gx = FB_GRAPH_X0 + 2; gx < FB_WIDTH; gx += 2)
        {
            fbPut(fb, gx, t.pos, grid);
        }
        int16_t top = (int16_t)(t.pos - FB_PIXEL_FONT_ROWS / 2);
        if (top < FB_GRAPH_TOP - 1)
        {
            top = FB_GRAPH_TOP - 1;
        }
        if (top > FB_GRAPH_AXIS_Y - FB_PIXEL_FONT_ROWS)
        {
            top = FB_GRAPH_AXIS_Y - FB_PIXEL_FONT_ROWS;
        }
        fbDrawPixelText(fb, (int16_t)(FB_GRAPH_X0 - 2 - fbPixelTextWidth(t.label)), top, t.label, legend);
    }
    int16_t lastLabelEnd = -100;
    for (uint8_t i = 0; i < scene.xTickCount && i < FB_GRAPH_MAX_XTICKS; i++)
    {
        const FbGraphTick &t = scene.xTicks[i];
        if (t.pos <= FB_GRAPH_X0 || t.pos >= FB_WIDTH)
        {
            continue;
        }
        for (int16_t gy = FB_GRAPH_AXIS_Y - 2; gy >= FB_GRAPH_TOP; gy -= 2)
        {
            fbPut(fb, t.pos, gy, grid);
        }
        int16_t w = fbPixelTextWidth(t.label);
        int16_t lx = (int16_t)(t.pos - w / 2);
        if (lx < FB_GRAPH_X0 - w / 2)
        {
            lx = (int16_t)(FB_GRAPH_X0 - w / 2);
        }
        if (lx > FB_WIDTH - w)
        {
            lx = (int16_t)(FB_WIDTH - w);
        }
        if (lx <= lastLabelEnd + FB_PIXEL_FONT_ADVANCE)
        {
            continue; // would touch the label before it (ticks are ordered by position)
        }
        fbDrawPixelText(fb, lx, FB_GRAPH_XLABEL_Y, t.label, legend);
        lastLabelEnd = (int16_t)(lx + w);
    }

    // ---- axes ----
    fbGraphVRun(fb, FB_GRAPH_X0, FB_GRAPH_TOP, FB_GRAPH_AXIS_Y, axis);
    for (int16_t ax = FB_GRAPH_X0; ax < FB_WIDTH; ax++)
    {
        fbPut(fb, ax, FB_GRAPH_AXIS_Y, axis);
    }

    // ---- data: band first, then the average line on top ----
    if (scene.drawEnvelope)
    {
        for (int16_t i = 0; i < FB_GRAPH_COLUMNS; i++)
        {
            const FbGraphColumn &c = scene.cols[i];
            if ((c.flags & FB_GRAPH_COL_DATA) != 0)
            {
                fbGraphVRun(fb, (int16_t)(FB_GRAPH_X0 + 1 + i), c.yTop, c.yBottom, band);
            }
        }
    }
    int16_t prevX = -1;
    int16_t prevY = 0;
    for (int16_t i = 0; i < FB_GRAPH_COLUMNS; i++)
    {
        const FbGraphColumn &c = scene.cols[i];
        int16_t cx = (int16_t)(FB_GRAPH_X0 + 1 + i);
        if ((c.flags & FB_GRAPH_COL_OL) != 0)
        {
            fbGraphVRun(fb, cx, FB_GRAPH_TOP, FB_GRAPH_TOP + 1, text); // OL mark at the top edge
        }
        if ((c.flags & FB_GRAPH_COL_DATA) == 0)
        {
            continue;
        }
        if (scene.drawAverage)
        {
            if ((c.flags & FB_GRAPH_COL_CONNECT) != 0 && prevX >= 0)
            {
                // straight line from the previous point, one vertical run per column
                int16_t dx = (int16_t)(cx - prevX);
                int16_t lastY = prevY;
                for (int16_t k = 1; k <= dx; k++)
                {
                    int16_t y = (int16_t)(prevY + ((int32_t)(c.yAvg - prevY) * k + (c.yAvg >= prevY ? dx / 2 : -dx / 2)) / dx);
                    int16_t from = lastY;
                    if (from != y)
                    {
                        from = (int16_t)(from + (y > from ? 1 : -1)); // the previous column already has lastY
                    }
                    fbGraphVRun(fb, (int16_t)(prevX + k), from, y, text);
                    lastY = y;
                }
            }
            else
            {
                fbPut(fb, cx, c.yAvg, text);
            }
        }
        else if (!scene.drawEnvelope)
        {
            fbPut(fb, cx, c.yAvg, text);
        }
        if ((c.flags & FB_GRAPH_COL_CLIP_HI) != 0)
        {
            fbPut(fb, cx, FB_GRAPH_TOP, text);
        }
        if ((c.flags & FB_GRAPH_COL_CLIP_LO) != 0)
        {
            fbPut(fb, cx, FB_GRAPH_BOTTOM, text);
        }
        prevX = cx;
        prevY = c.yAvg;
    }

    // ---- v18: markers (TAG) - a triangle pointing down at the top edge, over the data ----
    for (int16_t i = 0; i < FB_GRAPH_COLUMNS; i++)
    {
        if ((scene.cols[i].flags & FB_GRAPH_COL_MARK) != 0)
        {
            fbGraphDrawMark(fb, (int16_t)(FB_GRAPH_X0 + 1 + i));
        }
    }

    // ---- message in the middle of the plot, on a cleared box ----
    if (scene.message[0] != '\0')
    {
        int16_t w = fbPixelTextWidth(scene.message);
        int16_t mx = (int16_t)(FB_GRAPH_X0 + 1 + (FB_GRAPH_COLUMNS - w) / 2);
        int16_t my = (int16_t)((FB_GRAPH_TOP + FB_GRAPH_BOTTOM - FB_PIXEL_FONT_ROWS) / 2);
        fbFillRect(fb, (int16_t)(mx - 4), (int16_t)(my - 3), (int16_t)(mx + w + 4), (int16_t)(my + FB_PIXEL_FONT_ROWS + 3), 0);
        fbDrawPixelText(fb, mx, my, scene.message, text);
    }
}

#endif // FRAMEBUFFER_H
