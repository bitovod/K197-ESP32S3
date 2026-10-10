// rtc.h
// Thin project-specific wrapper around the third-party
// **DS3231Centisecond** library (install it - copy the
// DS3231Centisecond folder into your Arduino libraries directory -
// same "not bundled here, install separately" treatment this project
// already gives Vrekrer_scpi_parser). That library owns the actual
// I2C register access and the millis()-interpolated centisecond
// resolution; this file adds just the two things it doesn't provide:
//
//   - Oscillator Stop Flag ("lost power") reporting - the library's
//     own API has no equivalent, so rtcHasLostPower() below still
//     does its own small, direct read of the DS3231's status
//     register (0x0F), independent of the library object. This is
//     the one place this project still talks to the DS3231's
//     register map directly rather than through the library.
//   - a compact, fixed-size timestamp representation
//     (rtcCompactTimestamp()/rtcFormatCompactTimestamp() below) for
//     datalog.ino's ring buffer - see their own comments for the
//     format and why it's shaped the way it is.
//
// Backs SYSTem:TIME/SYSTem:TIME? in serial_out.ino, same as before -
// rtcGetDateTime()/rtcSetDateTime()'s signatures are unchanged from
// the previous, fully hand-rolled version of this file, so nothing
// in serial_out.ino needed to change. The RTC chip itself is the
// persistent store (battery/supercap-backed) - unlike every setting
// in settings.h, there is nothing to mirror into EEPROM here.
//
// Year handling: the DS3231 (and this library) only stores a 2-digit
// year (00-99) plus one century bit, never touched here - so this
// whole file assumes 2000-2099. rtcSetDateTime() rejects anything
// outside that range. Long enough not to matter for a bench
// instrument.
//
// NOT VERIFIED against a real build in this environment (no
// toolchain available) - the DS3231Centisecond library itself came
// from the author's own prior, separately-tested work, but its
// integration here (particularly rtcPoll()'s placement - see its own
// comment) has not been.

#ifndef RTC_H
#define RTC_H

#include <Arduino.h>

// Call once from setup(), after Wire.begin() - creates the
// DS3231Centisecond object's own initial sync. Safe to call even if
// no RTC is wired up (see docs/DEVELOPMENT.md - I2C bus is brought up
// regardless); everything else in this file checks rtcIsPresent()
// itself, so nothing else needs its own presence check.
void rtcBegin();

// Call every core1Task() iteration (ESP32S3_K197.ino), the same
// loop that already drains serialOutProcess()
// every ~1ms - NOT core0Task(): the DS3231Centisecond library needs
// poll() called often for its centisecond resolution to stay
// accurate (see its own header comment), and core0Task()'s loop only
// iterates once per DMM frame (or DMM_CAPTURE_TIMEOUT_MS, 200ms) -
// the same reason buttons ended up interrupt-driven rather than
// polled from there (see buttons.h). The library's own internal
// pollIntervalMs (4ms by default) already rate-limits the actual I2C
// traffic, so calling this every ~1ms costs nothing extra on the
// calls that don't do an I2C transaction.
//
// core0Task() (dmm_bridge.ino, via datalogUpdateReading()) only ever
// READS the library's cached fields (through rtcCompactTimestamp()
// below) - it never calls poll() or setDateTime() itself, so there is
// exactly one writer (core1) and this needs no mutex, the same
// "single writer, small POD reads" reasoning already used for the
// SCPI cache in serial_out.ino. One caveat that reasoning doesn't
// quite cover: rtcCompactTimestamp() reads several of the library's
// fields (year/month/.../centisecond) one after another rather than
// one single value, so - unlike the SCPI cache's single glob - it is
// theoretically possible for a read to land exactly as poll() is
// updating them and combine an old field with a new one (e.g. the old
// minute with a new second right at a minute boundary). The window
// for that is a handful of microseconds, once a second; for a logging
// timestamp (not a safety- or billing-critical one) that's an
// accepted, deliberate trade-off, not an oversight.
void rtcPoll();

// Whether the DS3231Centisecond library has completed its first sync
// (i.e. whether a DS3231 answered on the bus and its second-edge has
// been caught at least once). Checked once meaningfully at boot via
// rtcBegin()'s own sync attempt and kept current by rtcPoll() -
// unlike the old hand-rolled version, this call is free (just reads
// the library's own cached flag, no I2C).
bool rtcIsPresent();

// True if the DS3231 is reporting that it lost power since it was
// last set (its Oscillator Stop Flag) - meaning whatever it currently
// reports is not trustworthy until rtcSetDateTime() is called again.
// Cleared automatically by that call. Does its own direct I2C read
// (see the file comment above) - cheap (one register), but unlike
// every other function in this file, not just reading the library's
// cache, so avoid calling this in a tight loop.
bool rtcHasLostPower();

// Reads the current date/time (whole seconds only - see
// rtcCompactTimestamp() below for centisecond resolution). Returns
// false (leaving every output unchanged) if no RTC is present/synced.
bool rtcGetDateTime(uint16_t *year, uint8_t *month, uint8_t *day,
                     uint8_t *hour, uint8_t *minute, uint8_t *second);

// Sets the date/time and clears the Oscillator Stop Flag. year must
// be 2000-2099; returns false (nothing written) on an out-of-range
// field or if no RTC is present - does NOT otherwise validate
// day-of-month against month/leap-year, that's serial_out.ino's job
// (see HandleSystemTimeSet()) so its error message can name exactly
// which field was wrong.
bool rtcSetDateTime(uint16_t year, uint8_t month, uint8_t day,
                     uint8_t hour, uint8_t minute, uint8_t second);

// A single reading's timestamp, packed into the smallest
// representation that still gives centisecond resolution across the
// DS3231's full supported range - see LogRecord (datalog.h) for
// where this actually gets stored, and why 5 bytes (not the naive 8
// you'd get from storing year/month/day/hour/minute/second/
// centisecond as separate fields) is "data-optimized" without losing
// any precision the DS3231Centisecond library itself provides:
//   secondsSince2000: whole seconds since 2000-01-01 00:00:00 (NOT
//     Unix epoch - shifted to line up with the DS3231's own
//     2000-2099 range, so all of it fits in a uint32_t with room to
//     spare) - see rtcCompactTimestamp()/rtcFormatCompactTimestamp()
//     below for the conversions. 0 is a reserved sentinel meaning "no
//     timestamp available when this was recorded" (RTC absent, or
//     not yet synced this session), never a real value - if it were,
//     the actual date 2000-01-01 00:00:00 would need special-casing
//     too, and nothing about this project's timeframe could ever
//     produce that date for real.
//   centisecond: 0-99, meaningless when secondsSince2000 == 0.
//
// Fills both outputs and returns true only if the RTC is currently
// synced; leaves them at the 0/0 sentinel and returns false
// otherwise, so a caller that ignores the return value still gets a
// well-defined, recognizably-invalid timestamp rather than stale or
// uninitialized data.
bool rtcCompactTimestamp(uint32_t *secondsSince2000, uint8_t *centisecond);

// Formats a timestamp previously produced by rtcCompactTimestamp()
// (or read back out of a LogRecord) as "YYYY-MM-DD HH:MM:SS.cc" into
// buf (needs at least 23 bytes, including the terminator) - the same
// layout DS3231Centisecond::formatDateTime() itself uses for a LIVE
// reading, but working from a stored compact pair instead, since a
// logged record's timestamp is history, not "right now". Writes a
// fixed placeholder instead if secondsSince2000 is the 0 sentinel
// (see rtcCompactTimestamp() above), so callers (datalog.ino) never
// need their own special-case for "this record predates RTC sync".
void rtcFormatCompactTimestamp(uint32_t secondsSince2000, uint8_t centisecond,
                                char *buf, size_t bufSize);

#endif // RTC_H
