// Renders the screens of the firmware into PPM pictures with the real drawing
// code (framebuffer.h) - the screenshots in docs/images are made with it, see
// tools/make_screenshots.sh. The graph pictures come from test_graph (it
// writes its pictures into the folder given as its argument).
// Usage: render_screens <output folder>
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "framebuffer.h"
#include "help_text.h"

static uint8_t gFb[FB_BYTES];
static const char *gDir = ".";

static void save(const char *name, int theme, bool uiScreen)
{
    uint16_t pal[FB_PALETTE_SIZE];
    fbBuildPalette(uiScreen ? fbStatsTheme(themes[theme]) : themes[theme], pal);
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.ppm", gDir, name);
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", FB_WIDTH, FB_HEIGHT);
    for (int y = 0; y < FB_HEIGHT; y++)
    {
        for (int x = 0; x < FB_WIDTH; x++)
        {
            uint8_t r, g, b;
            fbUnpack565(pal[fbGet(gFb, x, y)], r, g, b);
            fputc(r, f);
            fputc(g, f);
            fputc(b, f);
        }
    }
    fclose(f);
}

static void lcd(const char *name, int theme, const char *digits, int dp, uint16_t ind, uint8_t unit, uint8_t alarm,
                bool tag)
{
    FbLcdScene s;
    for (int i = 0; i < KEITHLEY_DIGIT_COUNT; i++)
    {
        s.segBits[i] = keithley_char_segbits[(digits[i] == ' ') ? 10 : digits[i] - '0'];
    }
    s.dpPos = (int8_t)dp;
    s.indicatorBits = ind;
    s.unit = unit;
    s.alarm = alarm;
    s.tagLit = tag;
    fbComposeLcd(gFb, s);
    save(name, theme, false);
}

static void setup(const char *name, const char *title, const char *const *labels, const char *const *values,
                  int rows, int cursor, const char *keys)
{
    FbSetupScene s;
    memset(&s, 0, sizeof(s));
    snprintf(s.title, sizeof(s.title), "%s", title);
    for (int i = 0; i < rows; i++)
    {
        snprintf(s.labels[i], sizeof(s.labels[i]), "%s", labels[i]);
        snprintf(s.values[i], sizeof(s.values[i]), "%s", values[i]);
    }
    snprintf(s.keys, sizeof(s.keys), "%s", keys);
    s.rowCount = (uint8_t)rows;
    s.cursorRow = (uint8_t)cursor;
    fbComposeSetup(gFb, s);
    save(name, 1, true);
}

int main(int argc, char **argv)
{
    if (argc > 1)
    {
        gDir = argv[1];
    }
    const uint16_t aut = 1u << KEITHLEY_IND_AUT;

    // the LCD replica in a few themes
    lcd("lcd_grn", 1, "123456", 0, aut, KEITHLEY_UNIT_V, FB_ALARM_NONE, false);
    lcd("lcd_lcd1", 5, "123456", 0, aut, KEITHLEY_UNIT_V, FB_ALARM_NONE, false);
    lcd("lcd_ohm", 1, "100000", 2, aut, KEITHLEY_UNIT_KOHM, FB_ALARM_NONE, false);
    lcd("lcd_ac", 1, " 23016", 2, aut | (1u << KEITHLEY_IND_AC), KEITHLEY_UNIT_MV, FB_ALARM_NONE, false);
    // limit alarm and marker
    lcd("lcd_alarm_both", 1, "123456", 0, aut, KEITHLEY_UNIT_V, FB_ALARM_ARMED_BOTH, false);
    lcd("lcd_alarm_high", 1, "130000", 0, aut, KEITHLEY_UNIT_V, FB_ALARM_HIGH, false);
    lcd("lcd_alarm_low", 1, "150000", 0, aut | FB_BIT_MINUS, KEITHLEY_UNIT_V, FB_ALARM_LOW, false);
    lcd("lcd_tag", 1, "123456", 0, aut, KEITHLEY_UNIT_V, FB_ALARM_NONE, true);

    // statistics
    {
        FbStatsScene s;
        memset(&s, 0, sizeof(s));
        snprintf(s.live, sizeof(s.live), "1.23461");
        snprintf(s.average, sizeof(s.average), "1.23456");
        snprintf(s.median, sizeof(s.median), "1.23457");
        snprintf(s.maximum, sizeof(s.maximum), "1.23488");
        snprintf(s.minimum, sizeof(s.minimum), "1.23419");
        snprintf(s.unit, sizeof(s.unit), "V");
        snprintf(s.interval, sizeof(s.interval), "1 MIN");
        snprintf(s.dateTime, sizeof(s.dateTime), "14:03:07 10.10.26");
        fbComposeStats(gFb, s);
        save("stats_grn", 1, true);
        save("stats_lcd1", 5, true);
    }

    // help, first page of the DMM section in both languages
    for (int lang = 0; lang < HELP_LANG_COUNT; lang++)
    {
        FbHelpScene h;
        memset(&h, 0, sizeof(h));
        snprintf(h.lines[0], sizeof(h.lines[0]), "KEITHLEY 197R, fw. %s", SCPI_IDN_FIRMWARE);
        snprintf(h.lines[1], sizeof(h.lines[1]), "build 10.10.26 12:00:00");
        h.rightAlign[1] = true;
        h.underline[1] = true;
        const char *p = helpTexts[lang][HELP_SECTION_DMM];
        for (int i = 2; i < FB_HELP_LINES && *p; i++)
        {
            const char *nl = strchr(p, '\n');
            int n = (int)(nl - p);
            memcpy(h.lines[i], p, (size_t)n);
            h.lines[i][n] = '\0';
            p = nl + 1;
        }
        fbComposeHelp(gFb, h);
        save(lang == HELP_LANG_CZ ? "help_cz" : "help_en", 1, true);
    }

    // settings screens (English)
    {
        const char *l[3] = {"LANGUAGE", "THEME", "SOUND"};
        const char *v[3] = {"EN - ENGLISH", "GRN (2/8)", "ON"};
        setup("setup_dmm", "DMM SETTINGS", l, v, 3, 1, "REL/dB: change  RCL: next  STO: back");
    }
    {
        const char *l[4] = {"ALARM", "LOW LIMIT", "HIGH LIMIT", "SIGNAL"};
        const char *v[4] = {"BOTH LIMITS", "1.23400 V", "1.23500 V", "BEEP + ARROW"};
        setup("setup_alarm", "DCV - ALARM", l, v, 4, 2, "REL/dB: -/+  RCL 1s: = reading  STO: back");
    }
    printf("screens written to %s\n", gDir);
    return 0;
}
