// rtc.ino
// See rtc.h.

#include "rtc.h"
#include <Wire.h>
#include <DS3231Centisecond.h>
#include "settings.h" // v22: stored aging offset
#include "debuglog.h"

#define DS3231_I2C_ADDR 0x68

// Status register - only used for rtcHasLostPower() below, since the
// DS3231Centisecond library's own API has no equivalent. Everything
// else in this file goes through `rtc` (the library object), never
// touches the DS3231's register map directly.
#define DS3231_REG_STATUS 0x0F
#define DS3231_STATUS_OSF_BIT 0x80 // Oscillator Stop Flag
// v22: aging offset (SYSTem:TIME:AGINg) - also direct register access
#define DS3231_REG_CONTROL 0x0E
#define DS3231_CONTROL_CONV_BIT 0x20 // start a temperature conversion (and so apply the aging offset)
#define DS3231_STATUS_BSY_BIT 0x04   // a conversion is running
#define DS3231_REG_AGING 0x10

static DS3231Centisecond rtc;

void rtcBegin()
{
    rtc.begin();
}

void rtcPoll()
{
    rtc.poll();
}

bool rtcIsPresent()
{
    return rtc.isSynced();
}

bool rtcHasLostPower()
{
    if (!rtc.isSynced())
        return false; // nothing to report - HandleSystemTimeQuery() (serial_out.ino) checks rtcIsPresent() itself for that case

    Wire.beginTransmission(DS3231_I2C_ADDR);
    Wire.write((uint8_t)DS3231_REG_STATUS);
    if (Wire.endTransmission(false) != 0) // repeated start, no stop
        return false; // read failed - treat as "can't tell", not as "lost power"; a real problem here would also fail the next actual RTC access and report itself that way
    if (Wire.requestFrom(DS3231_I2C_ADDR, 1) != 1)
        return false;

    uint8_t status = Wire.read();
    return (status & DS3231_STATUS_OSF_BIT) != 0;
}

// v22: one register of the DS3231, read / written directly (like the status register above).
static bool rtcReadRegister(uint8_t reg, uint8_t *value)
{
    Wire.beginTransmission(DS3231_I2C_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0)
        return false;
    if (Wire.requestFrom(DS3231_I2C_ADDR, 1) != 1)
        return false;
    *value = (uint8_t)Wire.read();
    return true;
}

static bool rtcWriteRegister(uint8_t reg, uint8_t value)
{
    Wire.beginTransmission(DS3231_I2C_ADDR);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

bool rtcGetAging(int8_t *aging)
{
    if (!rtc.isSynced())
        return false;
    uint8_t value;
    if (!rtcReadRegister(DS3231_REG_AGING, &value))
        return false;
    *aging = (int8_t)value;
    return true;
}

bool rtcSetAging(int8_t aging)
{
    if (!rtc.isSynced())
        return false;
    if (!rtcWriteRegister(DS3231_REG_AGING, (uint8_t)aging))
        return false;
    int8_t check;
    if (!rtcGetAging(&check) || check != aging)
        return false;
    // start a temperature conversion so the new offset applies now (skipped while one is running anyway)
    uint8_t status;
    uint8_t control;
    if (rtcReadRegister(DS3231_REG_STATUS, &status) && (status & DS3231_STATUS_BSY_BIT) == 0 &&
        rtcReadRegister(DS3231_REG_CONTROL, &control))
    {
        rtcWriteRegister(DS3231_REG_CONTROL, (uint8_t)(control | DS3231_CONTROL_CONV_BIT));
    }
    return true;
}

void rtcApplyStoredAging()
{
    bool isSet = false;
    int8_t stored = settingsGetRtcAging(&isSet);
    int8_t chip;
    if (!isSet || !rtcGetAging(&chip))
        return;
    if (chip != stored)
    {
        bool ok = rtcSetAging(stored);
        debugLogAlways("boot: RTC aging %d -> %d from the settings%s", (int)chip, (int)stored, ok ? "" : " - write FAILED");
    }
}

bool rtcGetDateTime(uint16_t *year, uint8_t *month, uint8_t *day,
                     uint8_t *hour, uint8_t *minute, uint8_t *second)
{
    if (!rtc.isSynced())
        return false;

    *year = rtc.year();
    *month = rtc.month();
    *day = rtc.day();
    *hour = rtc.hour();
    *minute = rtc.minute();
    *second = rtc.second();
    return true;
}

bool rtcSetDateTime(uint16_t year, uint8_t month, uint8_t day,
                     uint8_t hour, uint8_t minute, uint8_t second)
{
    if (year < 2000 || year > 2099)
        return false;
    return rtc.setDateTime(year, month, day, hour, minute, second);
}

// rtc-prefixed (not bare isLeapYear()/daysInMonth()) specifically
// because serial_out.ino already has its own static functions with
// those exact names (for SYSTem:TIME's day-of-month validation) -
// Arduino concatenates every .ino file into ONE translation unit, so
// two file-scope `static` functions with the same name in different
// .ino files collide as a straight redefinition error, the same way
// they would if both were written in a single .cpp file - `static`
// only limits VISIBILITY to that translation unit, it does not carve
// out a separate namespace per source file the way it might seem to
// from how the files are organized on disk. Real error hit on the
// author's own build; small logic duplicated across the two files
// rather than shared, consistent with this project's general
// preference for self-contained files over cross-file coupling for
// something this small.
static bool rtcIsLeapYear(uint16_t year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static uint8_t rtcDaysInMonth(uint16_t year, uint8_t month)
{
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && rtcIsLeapYear(year))
        return 29;
    return days[month - 1];
}

// year/month/day (Gregorian, 2000-2099) -> whole days since
// 2000-01-01. Shared by rtcCompactTimestamp() (forward direction) and
// rtcFormatCompactTimestamp() (reverse, via the iterative unpacking
// below) so the epoch definition only lives in one place.
static uint32_t daysSince2000(uint16_t year, uint8_t month, uint8_t day)
{
    uint32_t days = 0;
    for (uint16_t y = 2000; y < year; y++)
        days += rtcIsLeapYear(y) ? 366 : 365;
    for (uint8_t m = 1; m < month; m++)
        days += rtcDaysInMonth(year, m);
    days += (day - 1);
    return days;
}

bool rtcCompactTimestamp(uint32_t *secondsSince2000, uint8_t *centisecond)
{
    if (!rtc.isSynced())
    {
        *secondsSince2000 = 0;
        *centisecond = 0;
        return false;
    }

    // Read every field the composite value needs in one tight burst -
    // see rtcPoll()'s own comment (rtc.h) for why this, rather than a
    // mutex, is this project's deliberate answer to the (tiny, once-
    // a-second) tear window between this and poll() running on the
    // other core.
    uint16_t year = rtc.year();
    uint8_t month = rtc.month();
    uint8_t day = rtc.day();
    uint8_t hour = rtc.hour();
    uint8_t minute = rtc.minute();
    uint8_t second = rtc.second();
    uint8_t cs = rtc.centisecond();

    uint32_t days = daysSince2000(year, month, day);
    *secondsSince2000 = days * 86400UL + (uint32_t)hour * 3600UL + (uint32_t)minute * 60UL + second;
    *centisecond = cs;
    return true;
}

void rtcFormatCompactTimestamp(uint32_t secondsSince2000, uint8_t centisecond,
                                char *buf, size_t bufSize)
{
    if (secondsSince2000 == 0)
    {
        snprintf(buf, bufSize, "NO-RTC-TIME");
        return;
    }

    uint32_t days = secondsSince2000 / 86400UL;
    uint32_t remainder = secondsSince2000 % 86400UL;
    uint8_t hour = (uint8_t)(remainder / 3600UL);
    uint8_t minute = (uint8_t)((remainder % 3600UL) / 60UL);
    uint8_t second = (uint8_t)(remainder % 60UL);

    uint16_t year = 2000;
    for (;;)
    {
        uint32_t yearDays = rtcIsLeapYear(year) ? 366UL : 365UL;
        if (days < yearDays)
            break;
        days -= yearDays;
        year++;
    }

    uint8_t month = 1;
    for (;;)
    {
        uint32_t monthDays = rtcDaysInMonth(year, month);
        if (days < monthDays)
            break;
        days -= monthDays;
        month++;
    }
    uint8_t day = (uint8_t)(days + 1);

    snprintf(buf, bufSize, "%04u-%02u-%02u %02u:%02u:%02u.%02u",
              year, month, day, hour, minute, second, centisecond);
}
