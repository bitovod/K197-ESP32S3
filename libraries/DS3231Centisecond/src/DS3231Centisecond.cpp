#include "DS3231Centisecond.h"

DS3231Centisecond::DS3231Centisecond(TwoWire &wire, uint8_t pollIntervalMs, uint8_t i2cAddress)
    : _wire(wire), _pollIntervalMs(pollIntervalMs), _i2cAddress(i2cAddress)
{
}

// Converts a BCD byte read from the DS3231 into a normal integer.
uint8_t DS3231Centisecond::bcdToDec(uint8_t bcd)
{
    return ((bcd / 16) * 10) + (bcd % 16);
}

// Converts a normal integer (0-99) into the BCD format the DS3231
// registers expect.
uint8_t DS3231Centisecond::decToBcd(uint8_t dec)
{
    return ((dec / 10) * 16) + (dec % 10);
}

// Sakamoto's algorithm - returns day of week (0=Sunday..6=Saturday)
// for a given Gregorian date. The DS3231 does not derive this on its
// own, it just stores whatever number is written to it, so this is
// only needed when writing a new date/time (see setDateTime()).
uint8_t DS3231Centisecond::dayOfWeek(uint16_t year, uint8_t month, uint8_t day)
{
    static const uint8_t t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (month < 3)
    {
        year -= 1;
    }
    return (year + year / 4 - year / 100 + year / 400 + t[month - 1] + day) % 7;
}

bool DS3231Centisecond::readNow(uint8_t &second, uint8_t &minute, uint8_t &hour,
                                 uint8_t &day, uint8_t &month, uint16_t &year) const
{
    _wire.beginTransmission(_i2cAddress);
    _wire.write((uint8_t)0x00);
    if (_wire.endTransmission() != 0)
    {
        return false;
    }

    _wire.requestFrom(_i2cAddress, (uint8_t)7);
    if (_wire.available() < 7)
    {
        return false;
    }

    second = bcdToDec(_wire.read() & 0x7F);
    minute = bcdToDec(_wire.read());
    hour   = bcdToDec(_wire.read() & 0x3F);
    _wire.read(); // day-of-week register - not needed here, re-derived on set
    day    = bcdToDec(_wire.read());
    month  = bcdToDec(_wire.read() & 0x1F);
    year   = 2000 + bcdToDec(_wire.read());
    return true;
}

bool DS3231Centisecond::setDateTime(uint16_t year, uint8_t month, uint8_t day,
                                     uint8_t hour, uint8_t minute, uint8_t second)
{
    uint8_t dow = dayOfWeek(year, month, day) + 1; // DS3231 wants 1-7

    _wire.beginTransmission(_i2cAddress);
    _wire.write((uint8_t)0x00);
    _wire.write(decToBcd(second));
    _wire.write(decToBcd(minute));
    _wire.write(decToBcd(hour));
    _wire.write(decToBcd(dow));
    _wire.write(decToBcd(day));
    _wire.write(decToBcd(month));
    _wire.write(decToBcd((uint8_t)(year - 2000)));
    bool ok = (_wire.endTransmission() == 0);

    resyncNow(); // pick up the new time immediately, not on the next tick
    return ok;
}

void DS3231Centisecond::resyncNow()
{
    _lastPolledSecond = 0xFF;
    _lastPollMillis = 0;
    poll();
}

bool DS3231Centisecond::begin()
{
    resyncNow();
    return _synced;
}

void DS3231Centisecond::poll()
{
    unsigned long now = millis();
    if (now - _lastPollMillis < _pollIntervalMs)
    {
        return;
    }
    _lastPollMillis = now;

    // Cheap single-byte check first - only pay for the full 7-byte
    // read (in readNow() below) on the second(s) that actually ticked.
    _wire.beginTransmission(_i2cAddress);
    _wire.write((uint8_t)0x00);
    if (_wire.endTransmission() != 0)
    {
        return; // RTC not answering right now - keep the last known sync
    }
    _wire.requestFrom(_i2cAddress, (uint8_t)1);
    if (_wire.available() < 1)
    {
        return;
    }
    uint8_t sec = bcdToDec(_wire.read() & 0x7F);

    if (sec == _lastPolledSecond)
    {
        return;
    }
    _lastPolledSecond = sec;

    if (readNow(_second, _minute, _hour, _day, _month, _year))
    {
        // We only sample the seconds register periodically rather than
        // catching an actual edge interrupt, so the moment we notice
        // the change is always a little AFTER the real hardware edge -
        // up to one whole poll interval late, averaging out to half an
        // interval late. Backdating by that expected average keeps
        // secondEdgeMillis tracking the true edge rather than
        // "whenever we happened to notice it".
        _secondEdgeMillis = now - (_pollIntervalMs / 2);
        _synced = true;
    }
}

uint8_t DS3231Centisecond::centisecond() const
{
    if (!_synced)
    {
        return 0xFF;
    }

    unsigned long elapsed = millis() - _secondEdgeMillis;
    if (elapsed > 999)
    {
        elapsed = 999; // a missed poll shouldn't roll this into the next second
    }
    return (uint8_t)(elapsed / 10);
}

void DS3231Centisecond::formatDateTime(char *buf, size_t bufSize) const
{
    snprintf(buf, bufSize, "%04u-%02u-%02u %02u:%02u:%02u.%02u",
              _year, _month, _day, _hour, _minute, _second, centisecond());
}
