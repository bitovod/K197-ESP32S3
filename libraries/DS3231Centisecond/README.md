# DS3231Centisecond

Centisecond-resolution (10 ms) clock built on top of a DS3231 RTC, using only
its standard I2C interface - no SQW pin or extra wiring needed.

## Why

The DS3231 only exposes whole seconds over I2C. This library resyncs a
`millis()`-based fractional clock to the DS3231's second register **every**
time that register ticks over (not just once at startup):

- the whole seconds always come straight from the RTC, never from `millis()`
  math
- `millis()` only fills in the fraction since that edge, which is at most
  ~1 second old, so the MCU's own clock drift never gets a large enough
  window to build up into anything visible
- no periodic resync timer to manage, no carry/rollover arithmetic across
  minute/hour/date boundaries - the two clocks are never allowed to drift
  far enough apart for that to matter

Software-only accuracy is roughly +/-(poll interval / 2), a few ms by
default - plenty for a 10 ms display resolution. For tighter accuracy, wire
the DS3231's SQW pin (1 Hz square wave) to an interrupt-capable GPIO instead.

## Install

Copy this folder into your Arduino `libraries` directory (or use
Sketch → Include Library → Add .ZIP Library... on a zipped copy), then
restart the Arduino IDE.

## Usage

```cpp
#include <Wire.h>
#include <DS3231Centisecond.h>

DS3231Centisecond rtc;

void setup()
{
    Wire.begin();          // or Wire.begin(sda, scl) if your board needs it
    rtc.begin();
}

void loop()
{
    rtc.poll();            // call this every loop() iteration

    if (rtc.isSynced())
    {
        char buf[24];
        rtc.formatDateTime(buf, sizeof(buf));
        Serial.println(buf); // "2026-09-24 14:03:07.42"
    }
}
```

See `examples/Basic` for a complete sketch, and `examples/SetTime` for
writing a new date/time over Serial (handy for testing without a real-time
source, and for checking battery retention).

## API

- `DS3231Centisecond(TwoWire &wire = Wire, uint8_t pollIntervalMs = 4, uint8_t i2cAddress = 0x68)`
- `bool begin()`
- `void poll()` - call every `loop()` iteration
- `bool setDateTime(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second)`
- `void resyncNow()`
- `bool isSynced() const`
- `uint8_t hour() / minute() / second() / day() / month() const`, `uint16_t year() const`
- `uint8_t centisecond() const` - 0-99, or 0xFF if not synced yet
- `bool readNow(uint8_t &second, uint8_t &minute, uint8_t &hour, uint8_t &day, uint8_t &month, uint16_t &year) const` - one-shot read, bypasses the centisecond sync
- `void formatDateTime(char *buf, size_t bufSize) const`
