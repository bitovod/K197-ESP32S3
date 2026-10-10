// reading_math.h
// Converts a decoded DMM reading (digits[]/dpPos/indicatorBits/unit -
// the same four values setReadingSegments()/setIndicators()/setUnit()
// take) into a plain numeric value, a function-name prefix, and a
// scientific-notation text representation. Shared between
// serial_out.ino (SYSTem:READing 1/2) and datalog.ino (the PSRAM
// reading log's LOG:DATA*/LOG:MIN? etc.) - moved out of serial_out.ino
// into its own file once datalog.ino needed the exact same conversion
// serial_out.ino already had, rather than duplicate it in two places.

#ifndef READING_MATH_H
#define READING_MATH_H

#include <Arduino.h>
#include "keithley_shapes.h"
#include "display.h" // KEITHLEY_BIT_* - explicit even though Arduino's
                      // whole-sketch concatenation would make these
                      // visible anyway via ESP32S3_K197.ino's own
                      // include, same reasoning as serial_out.ino's
                      // explicit include of the same header.

// Reconstructs a decoded reading as a plain value in base SI units
// (volts, amps, or ohms - the unit's m/u/k/M prefix is folded into
// the returned number, not left as a separate multiplier). Returns
// false if the reading isn't an ordinary number to begin with: no
// decimal point at all (dpPos < 0, e.g. self-test content) or a
// non-digit character present ('L', for "OL" overload) - the caller
// decides what to do instead in that case.
bool keithleyReadingToDouble(const char digits[KEITHLEY_DIGIT_COUNT], int8_t dpPos,
                              uint16_t indicatorBits, uint8_t unit, double *outValue);

// True if any digit position is 'L' - i.e. the reading is "OL"
// (overload), which keithleyReadingToDouble() can't turn into a real
// value. Callers that need to special-case overload (a numeric
// sentinel instead of a real value, an 'O' status character, ...)
// check this instead of re-scanning digits[] themselves.
bool keithleyIsOverload(const char digits[KEITHLEY_DIGIT_COUNT]);

// The function-name prefix a reading's unit + AC indicator implies
// (e.g. "DCV", "ACI", "OHM", " dB" - note the leading space on " dB",
// intentional: it keeps every prefix exactly 3 characters wide, same
// as "DCV"/"ACV"/"DCI"/"ACI"/"OHM", for column alignment). See the
// .ino for the exact mapping.
const char *keithleyFunctionPrefix(uint8_t unit, uint16_t indicatorBits);

// Prints `value` in normalized scientific notation, mantissa always
// with 5 fractional digits: "[+-]D.ddddd E[+-]EE", e.g.
// "-1.23456E+2". `acMagnitude`: pass true for an AC voltage/current
// reading - AC quantities (RMS) have no sign of their own, so a
// non-negative value prints a leading space instead of '+' there,
// keeping column alignment without implying a polarity that doesn't
// apply. Pass false for DC/resistance/dB readings, and for anything
// that isn't tied to one specific function (e.g. a LOG:MIN?/MAX?/
// AVERage?/MEDian? statistic spanning a range that could in principle
// mix functions) - those always get a normal '+'/'-'.
void keithleyPrintScientific(Stream &interface, double value, bool acMagnitude);

// Overflow/overload sentinel magnitude used wherever a reading can't
// produce a real segment-derived value (OL) but still needs to fit
// into the numeric scientific-notation formats - the conventional
// SCPI-instrument choice of a deliberately huge, unmistakable
// placeholder rather than text a numeric parser would have to
// special-case. Sign is separate - a caller building an overload
// value negates this itself based on the minus indicator.
#define KEITHLEY_OVERLOAD_SENTINEL 9.99999e37

// Prints an already-decoded reading as
// "<status><3-char prefix><scientific value>", e.g. "NOHM+1.23456E+3"
// - the exact format SYSTem:READing=1 and LOG:DATA?/etc. both use, so
// this is the one place that formatting is defined. Status is 'O' if
// `overload` is true (the caller is expected to have already set
// `value` to +-KEITHLEY_OVERLOAD_SENTINEL in that case - this
// function only decides which letter to print, it doesn't substitute
// the sentinel itself), else 'Z' if indicatorBits has
// KEITHLEY_BIT_REL set, else 'N'.
void keithleyPrintStatusPrefixValue(Stream &interface, double value, uint8_t unit,
                                     uint16_t indicatorBits, bool overload);

#endif // READING_MATH_H
