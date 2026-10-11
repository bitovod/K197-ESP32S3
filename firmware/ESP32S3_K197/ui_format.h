// ui_format.h
// Pure text formatting for the user interface screens: unit names,
// numbers with a fixed number of decimals, and the live reading taken
// straight from the DMM's digit characters. Header-only and free of
// hardware/RTOS dependencies (only keithley_shapes.h/font_data.h/
// display.h for constants), so it can be compiled and tested on a PC -
// see host_tests/.
//
// All numeric inputs here are in BASE SI units (volts, amps, ohms) like
// everything the datalog stores, except uiFormatLiveText(), which works
// on the digits exactly as the DMM displays them.

#ifndef UI_FORMAT_H
#define UI_FORMAT_H

#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "keithley_shapes.h"
#include "font_data.h"
#include "display.h" // KEITHLEY_BIT_*

// What is shown instead of a number that is not available (yet): six
// dashes with a decimal point, the same width and shape as a reading, so
// the columns keep their look. Used by the statistics (no record in the
// interval, or the computation has not finished since the interval, unit
// or function changed) and by the live value when the display does not
// show a reading (the DMM's own self-test).
#define UI_PLACEHOLDER "--.----"

// Text for a unit code (KEITHLEY_UNIT_*), e.g. "mV", "uA" (with the real
// micro sign), "kOhm" (with the real omega). KEITHLEY_UNIT_NONE and
// anything unknown give an empty string.
static inline void uiUnitText(uint8_t unit, char *out, size_t size)
{
    switch (unit)
    {
        case KEITHLEY_UNIT_MV:   snprintf(out, size, "mV"); break;
        case KEITHLEY_UNIT_V:    snprintf(out, size, "V"); break;
        case KEITHLEY_UNIT_UA:   snprintf(out, size, "%cA", FONT_CH_MICRO); break;
        case KEITHLEY_UNIT_MA:   snprintf(out, size, "mA"); break;
        case KEITHLEY_UNIT_A:    snprintf(out, size, "A"); break;
        case KEITHLEY_UNIT_OHM:  snprintf(out, size, "%c", FONT_CH_OHM); break;
        case KEITHLEY_UNIT_KOHM: snprintf(out, size, "k%c", FONT_CH_OHM); break;
        case KEITHLEY_UNIT_MOHM: snprintf(out, size, "M%c", FONT_CH_OHM); break;
        case KEITHLEY_UNIT_DB:   snprintf(out, size, "dB"); break;
        default:                 out[0] = '\0'; break;
    }
}

// How many base units one displayed unit is worth (1 mV = 1e-3 base
// units). Same table as keithleyUnitMultiplier() in reading_math.ino,
// which is private to that file.
static inline double uiUnitMultiplier(uint8_t unit)
{
    switch (unit)
    {
        case KEITHLEY_UNIT_MV:   return 1e-3;
        case KEITHLEY_UNIT_UA:   return 1e-6;
        case KEITHLEY_UNIT_MA:   return 1e-3;
        case KEITHLEY_UNIT_KOHM: return 1e3;
        case KEITHLEY_UNIT_MOHM: return 1e6;
        default:                 return 1.0; // V, A, Ohm, dB
    }
}

// Number of digits after the decimal point the DMM shows for a given
// decimal point position (see setReadingSegments(): the dot sits to the
// right of digit dpPos). Without a decimal point (self-test content)
// there is no meaningful answer - 3 is a neutral fallback.
static inline uint8_t uiDecimalsFor(int8_t dpPos)
{
    if (dpPos < 0 || dpPos >= KEITHLEY_DIGIT_COUNT)
    {
        return 3;
    }
    return (uint8_t)((KEITHLEY_DIGIT_COUNT - 1) - dpPos);
}

// The K197 is a 5 1/2 digit meter: six digit positions, the first of
// which can only be blank or 1, so no reading ever has more than
// 199999 counts (1.99999 V, 19.9999 V, 199.999 mV, ...).
#define UI_DISPLAY_MAX_COUNTS 199999L

// Formats a value given in base units the way the meter itself would
// show it in `unit`: at most 5 1/2 digits, i.e. never more precision
// than the main display has. `maxDecimals` is the number of decimals of
// the live reading (the current range); a value that is too large to
// fit that many decimals into 199999 counts gets fewer of them, exactly
// like the meter switching to a higher range: with maxDecimals 5,
// 0.5 -> "0.50000", 19.5 -> "19.5000", 1.999996 -> "2.0000". A value
// that does not fit even without decimals (it would need another unit
// prefix) is shown as "OL" / "-OL", like an overload on the meter. A
// result of zero never gets a minus sign ("-0.0000" looks like a
// malfunction). Not-a-number gives the placeholder. The result is at most 8
// characters ("-199999." never happens, "-1.99999" is the longest).
static inline void uiFormatNumber(char *out, size_t size, double baseValue, uint8_t unit, uint8_t maxDecimals)
{
    if (isnan(baseValue) || isinf(baseValue))
    {
        snprintf(out, size, UI_PLACEHOLDER);
        return;
    }

    double shown = baseValue / uiUnitMultiplier(unit);
    double magnitude = fabs(shown);

    // The most decimals (up to maxDecimals) with which the value still
    // fits into the display's counts.
    int decimals = (int)maxDecimals;
    double scale = 1.0;
    for (int i = 0; i < decimals; i++)
    {
        scale *= 10.0;
    }
    while (decimals > 0 && floor(magnitude * scale + 0.5) > (double)UI_DISPLAY_MAX_COUNTS)
    {
        decimals--;
        scale /= 10.0;
    }
    double counts = floor(magnitude * scale + 0.5);
    if (counts > (double)UI_DISPLAY_MAX_COUNTS)
    {
        snprintf(out, size, "%s", (shown < 0) ? "-OL" : "OL");
        return;
    }

    // Rounds to zero: show it without a sign.
    if (counts == 0.0)
    {
        shown = 0.0;
    }

    int length = snprintf(out, size, "%.*f", decimals, shown);
    if (length < 0 || (size_t)length >= size)
    {
        snprintf(out, size, UI_PLACEHOLDER); // cannot happen with the limit above and a sane buffer
    }
}

// The live reading as text, built from the DMM's own digit characters
// (so it shows exactly what the original display shows, with no float
// rounding in between). digits[] are the six characters the capture
// code decodes: '0'-'9', ' ' (blank) or 'L' (overload). The first digit
// is the blank/"1" half digit of the K197.
//
// Leading zeros of the integer part are dropped (one digit always stays
// before the decimal point): the DMM's "00.4327" becomes "0.4327", the same
// form uiFormatNumber() gives the statistics.
//
// Returns true and writes e.g. "-1.23456", "0.12345" or "OL" when there
// is something sensible to show; returns false and writes the placeholder for
// content that is not a reading (no decimal point, i.e. the DMM's
// self-test, or characters other than digits).
static inline bool uiFormatLiveText(char *out, size_t size, const char digits[KEITHLEY_DIGIT_COUNT],
                                    int8_t dpPos, uint16_t indicatorBits)
{
    bool negative = (indicatorBits & KEITHLEY_BIT_MINUS) != 0;

    for (uint8_t i = 0; i < KEITHLEY_DIGIT_COUNT; i++)
    {
        if (digits[i] == 'L')
        {
            snprintf(out, size, "%s", negative ? "-OL" : "OL");
            return true;
        }
    }

    if (dpPos < 0)
    {
        snprintf(out, size, UI_PLACEHOLDER);
        return false;
    }
    for (uint8_t i = 0; i < KEITHLEY_DIGIT_COUNT; i++)
    {
        if (digits[i] != ' ' && (digits[i] < '0' || digits[i] > '9'))
        {
            snprintf(out, size, UI_PLACEHOLDER);
            return false;
        }
    }

    char text[16];
    uint8_t length = 0;
    if (negative)
    {
        text[length++] = '-';
    }
    bool started = false;
    for (uint8_t i = 0; i < KEITHLEY_DIGIT_COUNT; i++)
    {
        char c = digits[i];
        if (!started && i < dpPos && (c == ' ' || c == '0'))
        {
            // Leading blank or zero before the integer part: dropped, so the
            // statistics screen shows "-0.4327" like the computed values, not
            // the DMM's "-00.4327". The digit right before the decimal point
            // (i == dpPos) is always kept, so at least one zero remains. Only
            // this screen uses this function - the LCD replica is untouched.
            continue;
        }
        if (c == ' ')
        {
            c = '0'; // blank at/after the decimal point position counts as zero
        }
        started = true;
        text[length++] = c;
        if (i == dpPos)
        {
            text[length++] = '.';
        }
    }
    text[length] = '\0';

    snprintf(out, size, "%s", text);
    return true;
}

// The function filter string datalogComputeStats() expects ("DCV",
// "ACV", "DCI", "ACI", "OHM", "DB"), from the prefix reading_math.ino
// derives. keithleyFunctionPrefix() pads " dB" with a leading space for
// column alignment, so this trims and upper-cases it the same way
// datalog.ino's functionMatches() does before comparing.
static inline void uiFunctionFilter(char *out, size_t size, const char *prefix)
{
    size_t length = 0;
    for (const char *p = prefix; *p != '\0' && length + 1 < size; p++)
    {
        if (*p == ' ')
        {
            continue;
        }
        char c = *p;
        if (c >= 'a' && c <= 'z')
        {
            c = (char)(c - 'a' + 'A');
        }
        out[length++] = c;
    }
    out[length] = '\0';
}

// ---- v25: the numbers of statistics pages 2 and 3 ----

// What pages 2 and 3 show instead of a number that is not available; the
// units stay (page 2 uses UI_PLACEHOLDER like page 1).
#define UI_PLACEHOLDER_SHORT "---"

// The base unit of a unit code, without a prefix: "V", "A", the omega, "dB";
// empty for KEITHLEY_UNIT_NONE.
static inline void uiBaseUnitText(uint8_t unit, char *out, size_t size)
{
    switch (unit)
    {
        case KEITHLEY_UNIT_MV:
        case KEITHLEY_UNIT_V:    snprintf(out, size, "V"); break;
        case KEITHLEY_UNIT_UA:
        case KEITHLEY_UNIT_MA:
        case KEITHLEY_UNIT_A:    snprintf(out, size, "A"); break;
        case KEITHLEY_UNIT_OHM:
        case KEITHLEY_UNIT_KOHM:
        case KEITHLEY_UNIT_MOHM: snprintf(out, size, "%c", FONT_CH_OHM); break;
        case KEITHLEY_UNIT_DB:   snprintf(out, size, "dB"); break;
        default:                 out[0] = '\0'; break;
    }
}

// A number with 3 significant digits and no exponent: "8.23", "82.3", "823",
// "0.123", "1500" (never fewer than 3 digits before a value gets that big,
// at most `maxDecimals` decimals). `withSign` adds "+" to a positive value
// (zero gets no sign). Not-a-number gives UI_PLACEHOLDER_SHORT.
static inline void uiFormatSignificant3(char *out, size_t size, double value, bool withSign, int maxDecimals)
{
    if (isnan(value) || isinf(value))
    {
        snprintf(out, size, UI_PLACEHOLDER_SHORT);
        return;
    }
    double magnitude = fabs(value);
    int decimals = 2;
    if (magnitude > 0.0)
    {
        decimals = 2 - (int)floor(log10(magnitude));
        // 9.996 rounds to 10.0 - one decimal fewer
        double scale = pow(10.0, decimals);
        if (decimals > 0 && floor(magnitude * scale + 0.5) >= 1000.0)
        {
            decimals--;
        }
    }
    if (decimals < 0)
    {
        decimals = 0;
    }
    if (decimals > maxDecimals)
    {
        decimals = maxDecimals;
    }
    double scale = pow(10.0, decimals);
    if (floor(magnitude * scale + 0.5) == 0.0)
    {
        value = 0.0; // no "-0.00", no "+0.00"
    }
    snprintf(out, size, (withSign && value > 0.0) ? "+%.*f" : "%.*f", decimals, value);
}

// A value in base units in engineering form: 3 significant digits and the
// unit with an SI prefix ("8.23" + "uV", "75.3" + "nV", "1.20" + "kOhm"), the
// unit followed by `suffix` ("/h"). dB gets no prefix (a fixed number with up
// to 5 decimals). Zero is "0.00" in the base unit. Not-a-number gives
// UI_PLACEHOLDER_SHORT (or `placeholder`) and the base unit.
static inline void uiFormatEngineering(char *number, size_t numberSize, char *unitOut, size_t unitSize, double baseValue,
                                       uint8_t unit, bool withSign, const char *suffix, const char *placeholder)
{
    char base[4];
    uiBaseUnitText(unit, base, sizeof(base));
    if (isnan(baseValue) || isinf(baseValue))
    {
        snprintf(number, numberSize, "%s", placeholder);
        snprintf(unitOut, unitSize, "%s%s", base, suffix);
        return;
    }
    if (unit == KEITHLEY_UNIT_DB)
    {
        uiFormatSignificant3(number, numberSize, baseValue, withSign, 5);
        snprintf(unitOut, unitSize, "%s%s", base, suffix);
        return;
    }
    static const char prefixes[] = {'f', 'p', 'n', FONT_CH_MICRO, 'm', '\0', 'k', 'M', 'G'};
    const int prefixZero = 5; // index of "no prefix"
    double magnitude = fabs(baseValue);
    int group = 0; // power of 1000
    if (magnitude > 0.0)
    {
        group = (int)floor(log10(magnitude) / 3.0);
        // 999.6 n rounds to 1000 n: 1.00 u
        double mantissa = magnitude / pow(1000.0, group);
        int decimals = 2 - (int)floor(log10(mantissa));
        if (decimals < 0)
        {
            decimals = 0;
        }
        if (floor(mantissa * pow(10.0, decimals) + 0.5) >= 1000.0 * pow(10.0, decimals))
        {
            group++;
        }
    }
    if (group < -prefixZero)
    {
        group = -prefixZero;
    }
    if (group > (int)sizeof(prefixes) - 1 - prefixZero)
    {
        group = (int)sizeof(prefixes) - 1 - prefixZero;
    }
    uiFormatSignificant3(number, numberSize, baseValue / pow(1000.0, group), withSign, 2);
    char prefix = prefixes[group + prefixZero];
    if (prefix == '\0')
    {
        snprintf(unitOut, unitSize, "%s%s", base, suffix);
    }
    else
    {
        snprintf(unitOut, unitSize, "%c%s%s", prefix, base, suffix);
    }
}

// A ratio (a spread over |average|) in ppm below 0.1 %, in % from there on,
// 3 significant digits: "6.67" + "ppm", "0.235" + "%". Not-a-number (no
// average, average zero) gives the placeholder and "ppm".
static inline void uiFormatRatio(char *number, size_t numberSize, char *unitOut, size_t unitSize, double ratio,
                                 const char *placeholder)
{
    if (isnan(ratio) || isinf(ratio) || ratio < 0.0)
    {
        snprintf(number, numberSize, "%s", placeholder);
        snprintf(unitOut, unitSize, "ppm");
        return;
    }
    if (ratio < 1e-3)
    {
        uiFormatSignificant3(number, numberSize, ratio * 1e6, false, 3);
        snprintf(unitOut, unitSize, "ppm");
    }
    else
    {
        uiFormatSignificant3(number, numberSize, ratio * 100.0, false, 3);
        snprintf(unitOut, unitSize, "%%");
    }
}

// A spread in digits of the last displayed place (`decimals` decimals in
// the displayed `unit`): two decimals below 10 digits, then 3 significant
// digits - "0.82", "12.3", "123".
static inline void uiFormatDigits(char *out, size_t size, double baseValue, uint8_t unit, uint8_t decimals)
{
    if (isnan(baseValue) || isinf(baseValue))
    {
        snprintf(out, size, UI_PLACEHOLDER_SHORT);
        return;
    }
    double digits = baseValue / uiUnitMultiplier(unit) * pow(10.0, decimals);
    if (fabs(digits) < 9.995)
    {
        snprintf(out, size, "%.2f", digits);
    }
    else
    {
        uiFormatSignificant3(out, size, digits, false, 1);
    }
}

#endif // UI_FORMAT_H
