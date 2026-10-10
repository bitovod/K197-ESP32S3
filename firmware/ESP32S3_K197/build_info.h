// build_info.h
// Firmware version and build time, for the help screen preface (ui.ino)
// and anything else that wants to show them. New in v12.
//
// BUILD_DATE / BUILD_TIME are the compiler's __DATE__ ("Oct  7 2026") and
// __TIME__ ("16:55:12") of the translation unit that includes this file.
// Arduino merges all .ino files into ONE translation unit, which is
// recompiled whenever any .ino file changes, so the stamp follows every
// build of the sketch. (A change in a .h file alone also triggers that
// recompile in the Arduino IDE.)

#ifndef BUILD_INFO_H
#define BUILD_INFO_H

#include <stdio.h>
#include <string.h>
#include "config.h" // SCPI_IDN_FIRMWARE

#define BUILD_DATE __DATE__
#define BUILD_TIME __TIME__
#define BUILD_FIRMWARE SCPI_IDN_FIRMWARE

// "DD.MM.YY hh:mm:ss" - the same date order as the statistics screen.
static inline void buildInfoFormatStamp(char *out, size_t size)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *date = BUILD_DATE; // "Mmm dd yyyy", day padded with a space
    int month = 0;
    for (int m = 0; m < 12; m++)
    {
        if (strncmp(date, months + m * 3, 3) == 0)
        {
            month = m + 1;
            break;
        }
    }
    int day = (date[4] == ' ' ? 0 : (date[4] - '0') * 10) + (date[5] - '0');
    snprintf(out, size, "%02d.%02d.%c%c %s", day, month, date[9], date[10], BUILD_TIME);
}

#endif // BUILD_INFO_H
