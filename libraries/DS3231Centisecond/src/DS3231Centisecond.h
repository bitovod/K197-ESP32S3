// DS3231Centisecond.h
//
// Centisecond-resolution clock built on top of a DS3231 RTC.
//
// The DS3231 only exposes whole seconds over I2C - there is no
// sub-second register to read. This library resyncs a millis()-based
// fractional clock to the DS3231's second register EVERY time that
// register ticks over (not just once at startup), so:
//
//   - the whole seconds (hour()/minute()/second()/...) always come
//     straight from the RTC, never from millis() math
//   - millis() only fills in the fraction *since* that edge, and that
//     edge is at most ~1 second old by construction, so the MCU's own
//     clock drift never gets a large enough window to build up into
//     anything visible
//   - there is no periodic "resync every N seconds" timer to manage
//     and no carry/rollover arithmetic across minute/hour/date
//     boundaries to get right, because the two clocks are never
//     allowed to drift far enough apart for that to matter
//
// This needs poll() to run often (see its own comment) so the moment
// of the edge is caught promptly; for sub-millisecond accuracy you'd
// want the DS3231's SQW pin on a 1Hz square wave feeding a hardware
// interrupt instead - this I2C-polling approach is good to roughly
// +/-(pollIntervalMs/2), which is plenty for centisecond display.
//
// Typical use:
//
//   #include <Wire.h>
//   #include <DS3231Centisecond.h>
//
//   DS3231Centisecond rtc;
//
//   void setup()
//   {
//       Wire.begin();          // or Wire.begin(sda, scl) if your board needs it
//       rtc.begin();
//   }
//
//   void loop()
//   {
//       rtc.poll();            // call this every loop() iteration
//       if (rtc.isSynced())
//       {
//           // rtc.hour(), rtc.minute(), rtc.second(), rtc.centisecond() ...
//       }
//   }
//
// See examples/Basic for a complete sketch, and examples/SetTime for
// writing a new date/time (e.g. to test battery retention).

#ifndef DS3231_CENTISECOND_H
#define DS3231_CENTISECOND_H

#include <Arduino.h>
#include <Wire.h>

class DS3231Centisecond
{
public:
    // wire: which I2C bus to use (default the board's main Wire) -
    //   pass Wire1 etc. on boards with more than one I2C bus.
    // pollIntervalMs: how often poll() actually touches I2C (a cheap
    //   single-byte read). Smaller = catches the RTC's second edge
    //   sooner (less lag) at the cost of a bit more I2C bus traffic;
    //   the default (4ms) gives roughly +/-2ms accuracy, plenty for
    //   10ms/centisecond display resolution.
    // i2cAddress: defaults to the DS3231's fixed address (0x68) - the
    //   parameter exists mainly for pin-compatible clones that answer
    //   on a different address.
    explicit DS3231Centisecond(TwoWire &wire = Wire, uint8_t pollIntervalMs = 4,
                                uint8_t i2cAddress = 0x68);

    // Call once after Wire.begin(), before using anything else below.
    // Does an initial read and sync. Returns false if the RTC did not
    // answer on the bus (isSynced() will then also read false, and
    // poll() will keep retrying on its own on every subsequent call).
    bool begin();

    // Call on every single loop() iteration. Cheap - a single-byte
    // I2C read - except on the (roughly once-a-second) tick itself,
    // when it also does a full 7-byte read to refresh the cached
    // date/time. See the class comment above for why this needs to
    // run often rather than on a fixed once-a-second timer: a loop()
    // that blocks for a long stretch (a long delay(), a slow blocking
    // peripheral call) will just leave centisecond() stale until
    // poll() next gets to run.
    void poll();

    // Writes a new date/time to the RTC and resyncs immediately, so
    // centisecond()/hour()/etc. reflect it right away instead of
    // waiting up to a second. month/day/year are calendar values
    // (year as e.g. 2026, not an offset); day-of-week is derived
    // automatically. Returns false if the write failed.
    bool setDateTime(uint16_t year, uint8_t month, uint8_t day,
                      uint8_t hour, uint8_t minute, uint8_t second);

    // Forces the next poll() call to treat the RTC's current second
    // as a fresh edge. begin() and setDateTime() already call this
    // for you - you'd only call it directly after changing the RTC's
    // time some other way this class doesn't know about.
    void resyncNow();

    // True once the first second edge has been caught. hour()/
    // minute()/etc. and centisecond() are meaningless before that
    // (centisecond() returns 0xFF as a sentinel until then).
    bool isSynced() const { return _synced; }

    // Whole date/time as of the most recent synced second edge.
    uint8_t hour()   const { return _hour; }
    uint8_t minute() const { return _minute; }
    uint8_t second() const { return _second; }
    uint8_t day()    const { return _day; }
    uint8_t month()  const { return _month; }
    uint16_t year()  const { return _year; }

    // 0-99 fraction of the current second, derived from milliseconds
    // elapsed since the last synced edge. Returns 0xFF if not synced
    // yet - check isSynced() first if that matters to you.
    uint8_t centisecond() const;

    // One-shot read straight from the RTC, bypassing the centisecond
    // sync entirely - no sub-second resolution, but always exactly
    // what the RTC has right now, and it doesn't disturb the running
    // sync. Returns false if the RTC did not answer. Handy for a
    // single verification read (see the SetTime example).
    bool readNow(uint8_t &second, uint8_t &minute, uint8_t &hour,
                 uint8_t &day, uint8_t &month, uint16_t &year) const;

    // Formats the most recent synced reading as "YYYY-MM-DD
    // HH:MM:SS.cc" into buf (needs at least 23 bytes, including the
    // terminator). Convenience wrapper around the getters above, for
    // a quick Serial.println(buf).
    void formatDateTime(char *buf, size_t bufSize) const;

private:
    TwoWire &_wire;
    uint8_t _pollIntervalMs;
    uint8_t _i2cAddress;

    unsigned long _secondEdgeMillis = 0;
    unsigned long _lastPollMillis = 0;
    uint8_t _lastPolledSecond = 0xFF; // sentinel - forces a sync on the first call
    bool _synced = false;

    uint8_t _second = 0, _minute = 0, _hour = 0, _day = 0, _month = 0;
    uint16_t _year = 0;

    static uint8_t bcdToDec(uint8_t bcd);
    static uint8_t decToBcd(uint8_t dec);
    static uint8_t dayOfWeek(uint16_t year, uint8_t month, uint8_t day);
};

#endif
