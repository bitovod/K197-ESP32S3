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

#endif // UI_FORMAT_H
