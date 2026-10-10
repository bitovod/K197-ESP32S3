#pragma once
// Stand-in for the RTC module (rtc.h) in the host tests: a clock the test can set.
#include <Arduino.h>

inline uint32_t &fakeRtcSeconds() // seconds since 2000-01-01, 0 = "no RTC time"
{
    static uint32_t s = 0;
    return s;
}
inline uint8_t &fakeRtcCentis()
{
    static uint8_t c = 0;
    return c;
}

inline bool rtcCompactTimestamp(uint32_t *seconds2000, uint8_t *centiseconds)
{
    *seconds2000 = fakeRtcSeconds();
    *centiseconds = fakeRtcCentis();
    return fakeRtcSeconds() != 0;
}

inline bool rtcGetDateTime(uint16_t *year, uint8_t *month, uint8_t *day, uint8_t *hour, uint8_t *minute, uint8_t *second)
{
    uint32_t s = fakeRtcSeconds();
    if (s == 0)
        return false;
    uint32_t days = s / 86400UL;
    uint32_t rem = s % 86400UL;
    *hour = (uint8_t)(rem / 3600);
    *minute = (uint8_t)((rem % 3600) / 60);
    *second = (uint8_t)(rem % 60);
    // civil date from days since 2000-01-01 (days since 1970-01-01 = days + 10957)
    long z = (long)days + 10957 + 719468;
    long era = z / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = yoe + era * 400;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    long d = doy - (153 * mp + 2) / 5 + 1;
    long m = mp < 10 ? mp + 3 : mp - 9;
    *year = (uint16_t)(y + (m <= 2));
    *month = (uint8_t)m;
    *day = (uint8_t)d;
    return true;
}

inline void rtcFormatCompactTimestamp(uint32_t, uint8_t, char *out, size_t size)
{
    snprintf(out, size, "NO-RTC-TIME");
}
