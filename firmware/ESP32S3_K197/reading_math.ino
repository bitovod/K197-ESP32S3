// reading_math.ino
// See reading_math.h.

#include "reading_math.h"
#include <math.h>

// Per-unit multiplier that folds a displayed prefix (m/u/k/M) into a
// plain value in base SI units (volts, amps, or ohms) - used by
// keithleyReadingToDouble() below. Units with no prefix concept (dB,
// none) pass through unscaled.
static double keithleyUnitMultiplier(uint8_t unit)
{
    switch (unit)
    {
        case KEITHLEY_UNIT_MV:   return 1e-3;
        case KEITHLEY_UNIT_V:    return 1.0;
        case KEITHLEY_UNIT_UA:   return 1e-6;
        case KEITHLEY_UNIT_MA:   return 1e-3;
        case KEITHLEY_UNIT_A:    return 1.0;
        case KEITHLEY_UNIT_OHM:  return 1.0;
        case KEITHLEY_UNIT_KOHM: return 1e3;
        case KEITHLEY_UNIT_MOHM: return 1e6;
        default:                 return 1.0; // dB, none - no scaling
    }
}

// "OHM" covers all three resistance ranges (Ohm/kOhm/MOhm all fold
// into the same base-Ohm value in keithleyUnitMultiplier() above,
// same as a real SCPI resistance reading). " dB" (note the leading
// space) isn't one of the five functions named in the request that
// first added this prefix scheme, but the K197 can show dB, so it's
// covered here too rather than silently falling through to "" - the
// leading space keeps it the same 3-character width as every other
// prefix ("DCV", "ACV", "DCI", "ACI", "OHM"), for column alignment.
const char *keithleyFunctionPrefix(uint8_t unit, uint16_t indicatorBits)
{
    bool ac = (indicatorBits & KEITHLEY_BIT_AC) != 0;
    switch (unit)
    {
        case KEITHLEY_UNIT_MV:
        case KEITHLEY_UNIT_V:
            return ac ? "ACV" : "DCV";
        case KEITHLEY_UNIT_UA:
        case KEITHLEY_UNIT_MA:
        case KEITHLEY_UNIT_A:
            return ac ? "ACI" : "DCI";
        case KEITHLEY_UNIT_OHM:
        case KEITHLEY_UNIT_KOHM:
        case KEITHLEY_UNIT_MOHM:
            return "OHM";
        case KEITHLEY_UNIT_DB:
            return " dB";
        default:
            return "";
    }
}

bool keithleyIsOverload(const char digits[KEITHLEY_DIGIT_COUNT])
{
    for (uint8_t i = 0; i < KEITHLEY_DIGIT_COUNT; i++)
    {
        if (digits[i] == 'L')
            return true;
    }
    return false;
}

bool keithleyReadingToDouble(const char digits[KEITHLEY_DIGIT_COUNT], int8_t dpPos,
                              uint16_t indicatorBits, uint8_t unit, double *outValue)
{
    if (dpPos < 0)
        return false;

    for (uint8_t i = 0; i < KEITHLEY_DIGIT_COUNT; i++)
    {
        if (digits[i] != ' ' && (digits[i] < '0' || digits[i] > '9'))
            return false;
    }

    double value = 0;
    for (uint8_t i = 0; i < KEITHLEY_DIGIT_COUNT; i++)
    {
        // Blank counts as digit 0 here (rather than being skipped)
        // so a blank position can never shift the ones that follow
        // it out of alignment with dpPos - in practice real readings
        // never have a blank except possibly a leading run, but this
        // way the math is correct regardless.
        int d = (digits[i] == ' ') ? 0 : (digits[i] - '0');
        value = value * 10.0 + d;
    }
    int fractionalDigits = (KEITHLEY_DIGIT_COUNT - 1) - dpPos;
    for (int i = 0; i < fractionalDigits; i++)
        value /= 10.0;

    if (indicatorBits & KEITHLEY_BIT_MINUS)
        value = -value;

    *outValue = value * keithleyUnitMultiplier(unit);
    return true;
}

size_t keithleyFormatScientific(char *buf, size_t bufSize, double value, bool acMagnitude)
{
    if (bufSize == 0)
        return 0;
    bool negative = value < 0;
    double magnitude = fabs(value);

    int exponent = 0;
    if (magnitude != 0.0)
    {
        exponent = (int)floor(log10(magnitude));
        magnitude /= pow(10.0, exponent);
        // log10()/pow() rounding can occasionally land just outside
        // [1, 10) - nudge back in rather than print e.g. "10.000000".
        if (magnitude >= 10.0)
        {
            magnitude /= 10.0;
            exponent++;
        }
        else if (magnitude < 1.0)
        {
            magnitude *= 10.0;
            exponent--;
        }
    }

    char mantissaBuf[32];
    dtostrf(magnitude, 1, 5, mantissaBuf); // "D.ddddd", no sign
    // AC has no polarity - a blank keeps the alignment and implies none
    char sign = negative ? '-' : (acMagnitude ? ' ' : '+');
    int n = snprintf(buf, bufSize, "%c%sE%c%d", sign, mantissaBuf, exponent >= 0 ? '+' : '-', abs(exponent));
    if (n < 0)
    {
        buf[0] = '\0';
        return 0;
    }
    return ((size_t)n < bufSize) ? (size_t)n : bufSize - 1;
}

// v23: the text comes from keithleyFormatScientific(), so the SD card files
// and the SCPI replies can never differ.
void keithleyPrintScientific(Stream &interface, double value, bool acMagnitude)
{
    char text[24];
    keithleyFormatScientific(text, sizeof(text), value, acMagnitude);
    interface.print(text);
}

void keithleyPrintStatusPrefixValue(Stream &interface, double value, uint8_t unit,
                                     uint16_t indicatorBits, bool overload)
{
    char status = overload ? 'O' : (indicatorBits & KEITHLEY_BIT_REL) ? 'Z' : 'N';
    bool ac = (indicatorBits & KEITHLEY_BIT_AC) != 0;
    interface.print(status);
    interface.print(keithleyFunctionPrefix(unit, indicatorBits));
    keithleyPrintScientific(interface, value, ac);
}
