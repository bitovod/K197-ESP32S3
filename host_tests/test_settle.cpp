// Host test for the settling rule of datalog.ino with the real constants (v20, v22): the first
// CONFIG_DATALOG_SETTLE_POWERON_READINGS readings after the ESP32 starts or the K197 was switched off
// (datalogNoteDmmOff()), the first CONFIG_DATALOG_SETTLE_READINGS after a change of the function or a
// non-numeric frame (self-test) - flagged DATALOG_BIT_SETTLING in the record, reported by
// datalogLastReadingSettling() for the alarm.
#include <Arduino.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <math.h>
#include "config.h"
#include "display.h"
#include "datalog.h"
#include "reading_math.h"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

uint16_t settingsGetAutoIntervalSeconds() { return 0; }
bool settingsGetDebugEnabled() { return false; }
void debugLogf(const char *, ...) {}
void debugLogAlways(const char *, ...) {}

#include "reading_math.ino"
#undef CONFIG_DATALOG_TEST_CAPACITY
#define CONFIG_DATALOG_TEST_CAPACITY 0
#include "datalog.ino"

static void feed(const char *digits6, uint8_t unit, int8_t dp = 0)
{
    fakeMillisOffset() += 300;
    fakeRtcSeconds() = fakeRtcSeconds() + 1;
    char d[6];
    memcpy(d, digits6, 6);
    datalogUpdateReading(d, dp, 0, unit);
}

// How many of the next `n` readings settle (the alarm's view, datalogLastReadingSettling()).
static int settlingOf(int n, const char *digits6, uint8_t unit)
{
    int s = 0;
    for (int i = 0; i < n; i++)
    {
        feed(digits6, unit);
        if (datalogLastReadingSettling()) s++;
    }
    return s;
}

// Settling records among the newest `n` of the log.
static int settlingRecords(int n)
{
    uint32_t serial = datalogEndSerial(NULL) - (uint32_t)n;
    int s = 0;
    for (int k = 0; k < n; k++)
    {
        LogRecord rec;
        uint32_t got = datalogReadSerial(&serial, &rec, 1, NULL);
        serial += got;
        if (got == 1 && (rec.indicatorBits & DATALOG_BIT_SETTLING)) s++;
    }
    return s;
}

int main()
{
    ESP.freePsram = (size_t)(20000.0 * sizeof(LogRecord)) + DATALOG_PSRAM_RESERVE_BYTES + 4;
    datalogBegin();
    fakeRtcSeconds() = 845208000;
    const int P = CONFIG_DATALOG_SETTLE_POWERON_READINGS, S = CONFIG_DATALOG_SETTLE_READINGS;
    CHECK(P > S, "power-on settling is the longer one");

    CHECK(settlingOf(P + 10, " 12345", KEITHLEY_UNIT_V) == P, "ESP32 start: the first POWERON readings settle");
    CHECK(settlingRecords(P + 10) == P, "... and are flagged in the log");
    CHECK(settlingOf(S + 10, " 54321", KEITHLEY_UNIT_KOHM) == S, "function change: the ordinary settling");
    feed("      ", KEITHLEY_UNIT_NONE, -1); // a self-test frame / an Err197 period: not a number
    CHECK(settlingOf(S + 10, " 54321", KEITHLEY_UNIT_KOHM) == S, "non-numeric frame (K197 running): the ordinary settling");
    datalogNoteDmmOff(); // oFF197 (dmmCheckNoData() then hands on the blank, like here)
    feed("      ", KEITHLEY_UNIT_NONE, -1);
    int on = settlingOf(P + 10, " 54321", KEITHLEY_UNIT_KOHM);
    CHECK(on == P, "K197 switched on again: the long settling");
    if (on != P) printf("   got %d\n", on);
    CHECK(settlingRecords(P + 10) == P, "... flagged in the log");
    CHECK(settlingOf(S + 10, " 12345", KEITHLEY_UNIT_V) == S, "the next function change: ordinary again");
    printf("settle: power-on %d readings, function change %d\n", P, S);
    printf(failures ? "FAILURES: %d\n" : "ALL OK\n", failures);
    return failures ? 1 : 0;
}
