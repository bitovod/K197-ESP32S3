// serial_out.h
// Public interface to the SCPI command port, on either the dedicated
// second UART (Serial1) or the USB/debug Serial, chosen at runtime
// and persisted via SYSTem:COMM (see serial_out.ino and
// ESP32S3_K197.ino). Implemented with the Vrekrer_scpi_parser
// library (https://github.com/Vrekrer/Vrekrer_scpi_parser) - install
// it via the Arduino Library Manager before building. See
// serial_out.ino's own header comment (or send `HELP?`/`*HLP?` to the
// device itself) for the full, current command list - this header
// only exposes the small set of functions other files need to feed
// it data or query its debug-trace state, not the commands themselves.
//
// This header intentionally exposes no SCPI-library types (SCPI_C,
// SCPI_Parser, ...) - only plain types - so files that just need to
// feed it the current reading (dmm_bridge.ino) don't have to know or
// care that SCPI is involved at all.

#ifndef SERIAL_OUT_H
#define SERIAL_OUT_H

#include <Arduino.h>
#include "keithley_shapes.h"
#include "config.h" // SCPI_IDN_* macros

// Call once from setup(), after settingsBegin() (needs SYSTem:COMM's
// persisted value) and after SERIAL_OUT_TX_PIN/SERIAL_OUT_RX_PIN are
// #defined in the main sketch file (same pattern as displayBegin()
// relying on TFT_* - see ESP32S3_K197.ino). Begins Serial1 (Serial
// is already begun elsewhere), sets the active port from SYSTem:COMM,
// and registers the SCPI commands.
void serialOutBegin();

// The port SYSTem:COMM currently has the SCPI interface routed to
// (Serial or Serial1) - only meaningful after serialOutBegin() has
// run. Exposed so ESP32S3_K197.ino's setup() can print the startup
// *IDN? banner (via serialOutPrintIdn() below) to the same port SCPI
// itself is about to use, instead of a port hardcoded independently
// of that choice.
Stream &serialOutActiveStream();

// Prints the *IDN? identification string to `interface` - the same
// text HandleIdentify() (serial_out.ino) replies with. Exposed here
// specifically so ESP32S3_K197.ino's setup() can print it once at
// boot, instead of a plain "starting up" banner. Doesn't need
// serialOutBegin() to have run first - it's just the *IDN? fields, no
// SCPI machinery involved - but its caller, if it wants the banner on
// the right port, does (see serialOutActiveStream() above).
void serialOutPrintIdn(Stream &interface);

// Updates the reading that READ? answers with. `digits` is exactly
// what's shown on the 6 digit positions, dpPos/indicatorBits/unit use
// the same encoding as setReadingSegments()/setIndicators()/setUnit().
// Called once per successfully decoded frame from dmmApplyFrame() -
// this only updates the cached state, it does not send anything by
// itself (unlike an earlier, pre-SCPI version of this file, which
// streamed every frame unprompted - see docs/DEVELOPMENT.md).
void serialOutUpdateReading(const char digits[KEITHLEY_DIGIT_COUNT], int8_t dpPos,
                             uint16_t indicatorBits, uint8_t unit);

// Call every loop() iteration (regardless of whether a DMM frame was
// captured that pass) to drain and dispatch any pending SCPI command.
// Cheap to call when there's nothing waiting.
void serialOutProcess();

// v22: millis() of the last byte received on the SCPI port (0 = none yet) -
// power.ino stays awake for a while after SCPI traffic.
uint32_t serialOutLastRxMs();

// True if SYSTem:DEBUG is ON *and* nothing is currently sitting
// half-typed in the SCPI line buffer (same reasoning as
// serialOutProcess()'s own auto-send guard - a debug trace line
// shouldn't land in the middle of a command someone hasn't finished
// typing yet). dmm_bridge.ino checks this before dumping the raw
// captured frame bytes (dmmDumpFrame()); serialOutUpdateReading()
// uses the same check internally before printing the decoded-reading
// trace, so both debug outputs are gated identically.
bool serialOutDebugTraceAllowed();

#endif // SERIAL_OUT_H
