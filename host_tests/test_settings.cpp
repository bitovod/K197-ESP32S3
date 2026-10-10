// Host test for settings.ino: deferred EEPROM write of the statistics
// interval, immediate SCPI saves taking a pending change along, skipped
// writes of unchanged content, migration from the v9/v10 layout (0xA9).
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <stdio.h>
#include "EEPROM.h"
#include "config.h"
#include "settings.ino"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static void advance(uint32_t ms) { fakeMillisOffset() += ms; }

int main()
{
    // ---- first boot: defaults written once ----
    EEPROM.reset();
    settingsBegin();
    CHECK(EEPROM.commits == 1, "first boot writes defaults");
    CHECK(settingsGetStatsInterval() == CONFIG_UI_STATS_DEFAULT_INTERVAL_INDEX, "default interval");
    CHECK(!settingsSavePending(), "nothing pending after boot");

    // ---- several quick changes -> one write, only after the quiet time ----
    settingsSetStatsIntervalDeferred(3);
    advance(4000);
    settingsSetStatsIntervalDeferred(4);
    advance(4000);
    settingsSetStatsIntervalDeferred(5);
    settingsPoll();
    CHECK(EEPROM.commits == 1, "no write while still changing");
    CHECK(settingsSavePending(), "pending");
    advance(CONFIG_SETTINGS_DEFERRED_SAVE_MS - 100);
    settingsPoll();
    CHECK(EEPROM.commits == 1, "no write before the delay since the LAST change");
    advance(200);
    settingsPoll();
    CHECK(EEPROM.commits == 2, "exactly one write after the delay");
    CHECK(!settingsSavePending(), "nothing pending after the write");
    settingsPoll();
    advance(CONFIG_SETTINGS_DEFERRED_SAVE_MS * 2);
    settingsPoll();
    CHECK(EEPROM.commits == 2, "no further writes");

    // ---- reboot restores the value ----
    settingsBegin();
    CHECK(settingsGetStatsInterval() == 5, "interval survives a reboot");
    CHECK(EEPROM.commits == 2, "valid content is not rewritten at boot");

    // ---- change and change back within the delay: no flash write ----
    settingsSetStatsIntervalDeferred(1);
    settingsSetStatsIntervalDeferred(5);
    advance(CONFIG_SETTINGS_DEFERRED_SAVE_MS + 1);
    settingsPoll();
    CHECK(EEPROM.commits == 2, "unchanged content is not written");

    // ---- an immediate SCPI save takes the pending change along ----
    settingsSetStatsIntervalDeferred(0);
    settingsSetTheme(2);
    CHECK(EEPROM.commits == 3, "SCPI setter writes at once");
    CHECK(!settingsSavePending(), "pending change went along");
    advance(CONFIG_SETTINGS_DEFERRED_SAVE_MS + 1);
    settingsPoll();
    CHECK(EEPROM.commits == 3, "no second write for the same change");
    settingsBegin();
    CHECK(settingsGetStatsInterval() == 0 && settingsGetTheme() == 2, "both survive");

    // ---- power-off inside the window keeps the old value ----
    settingsSetStatsIntervalDeferred(6);
    advance(1000);
    settingsBegin(); // "reboot" without the poll ever firing
    CHECK(settingsGetStatsInterval() == 0, "unsaved change lost, old value kept");

    // ---- millis() wrap-around ----
    fakeMillisOffset() = (uint32_t)(0xFFFFFFFFu - (uint32_t)(micros() / 1000UL) - 2000u);
    settingsSetStatsIntervalDeferred(2);
    advance(CONFIG_SETTINGS_DEFERRED_SAVE_MS + 10);
    settingsPoll();
    CHECK(!settingsSavePending(), "write fires across the millis() wrap");
    fakeMillisOffset() = 0;

    // ---- migration from 0xA9 (v9/v10 layout) keeps the old fields ----
    EEPROM.reset();
    KeithleySettings old;
    memset(&old, 0xFF, sizeof(old)); // the appended byte is erased flash
    old.magic = 0xA9;
    old.readingFormat = 1;
    old.autoEnabled = 1;
    old.autoIntervalSeconds = 30;
    old.theme = 3;
    old.debugEnabled = 1;
    old.beepEnabled = 0;
    old.commPort = SETTINGS_COMM_USB;
    old.displayYOffset = 17;
    EEPROM.put(0, old);
    EEPROM.commit();
    int before = EEPROM.commits;
    settingsBegin();
    CHECK(settingsGetTheme() == 3 && settingsGetDisplayYOffset() == 17 && settingsGetCommPort() == SETTINGS_COMM_USB, "migration keeps old fields");
    CHECK(settingsGetAutoIntervalSeconds() == 30 && settingsGetAutoEnabled() && !settingsGetBeepEnabled(), "migration keeps old fields 2");
    CHECK(settingsGetStatsInterval() == CONFIG_UI_STATS_DEFAULT_INTERVAL_INDEX, "new field gets its default");
    CHECK(settingsGetDebugPort() == CONFIG_DEFAULT_DEBUG_PORT, "v13 field (debug port) gets its default too");
    CHECK(EEPROM.commits == before + 1, "migration written once");
    settingsBegin();
    CHECK(EEPROM.commits == before + 1, "and not again");

    // ---- migration from 0xAA (v11/v12 layout): old fields and the interval kept, debug port defaulted ----
    EEPROM.reset();
    {
        KeithleySettings v12;
        memset(&v12, 0xFF, sizeof(v12)); // the appended byte is erased flash
        v12.magic = SETTINGS_MAGIC_V11;
        v12.readingFormat = 2;
        v12.autoEnabled = 0;
        v12.autoIntervalSeconds = 5;
        v12.theme = 4;
        v12.debugEnabled = 1;
        v12.beepEnabled = 1;
        v12.commPort = SETTINGS_COMM_UART;
        v12.displayYOffset = 21;
        v12.statsInterval = 5;
        EEPROM.put(0, v12);
        EEPROM.commit();
        settingsBegin();
        CHECK(settingsGetTheme() == 4 && settingsGetDisplayYOffset() == 21 && settingsGetStatsInterval() == 5, "0xAA migration keeps old fields");
        CHECK(settingsGetDebugPort() == CONFIG_DEFAULT_DEBUG_PORT, "0xAA migration: debug port default");
        settingsSetDebugPort(SETTINGS_COMM_USB);
        settingsBegin();
        CHECK(settingsGetDebugPort() == SETTINGS_COMM_USB, "debug port persists");
    }

    // ---- v14: migration from 0xAB (v13 layout): debug port kept, graph settings defaulted; deferred graph save ----
    EEPROM.reset();
    {
        KeithleySettings v13;
        memset(&v13, 0xFF, sizeof(v13));
        v13.magic = SETTINGS_MAGIC_V13;
        v13.readingFormat = 1;
        v13.autoEnabled = 0;
        v13.autoIntervalSeconds = 0;
        v13.theme = 2;
        v13.debugEnabled = 0;
        v13.beepEnabled = 1;
        v13.commPort = SETTINGS_COMM_UART;
        v13.displayYOffset = 23;
        v13.statsInterval = 4;
        v13.debugPort = SETTINGS_COMM_USB;
        EEPROM.put(0, v13);
        EEPROM.commit();
        settingsBegin();
        CHECK(settingsGetTheme() == 2 && settingsGetDisplayYOffset() == 23 && settingsGetStatsInterval() == 4, "0xAB migration keeps old fields");
        CHECK(settingsGetDebugPort() == SETTINGS_COMM_USB, "0xAB migration keeps the debug port");
        CHECK(settingsGetGraphInterval() == CONFIG_GRAPH_DEFAULT_INTERVAL_INDEX && settingsGetGraphYMode() == CONFIG_GRAPH_DEFAULT_YMODE &&
              settingsGetGraphTrace() == CONFIG_GRAPH_DEFAULT_TRACE && settingsGetGraphSpan() == CONFIG_GRAPH_DEFAULT_SPAN_INDEX,
              "0xAB migration: graph settings get their defaults");
        CHECK(settingsGetLanguage() == CONFIG_DEFAULT_LANGUAGE, "0xAB migration: language default");
        int commits = EEPROM.commits;
        settingsSetGraphDeferred(5, 2, 1, 7);
        CHECK(EEPROM.commits == commits && settingsSavePending(), "graph settings: deferred");
        fakeMillisOffset() += CONFIG_SETTINGS_DEFERRED_SAVE_MS + 10;
        settingsPoll();
        CHECK(EEPROM.commits == commits + 1 && !settingsSavePending(), "graph settings: written after the delay");
        settingsBegin();
        CHECK(settingsGetGraphInterval() == 5 && settingsGetGraphYMode() == 2 && settingsGetGraphTrace() == 1 && settingsGetGraphSpan() == 7,
              "graph settings persist");
    }

    // ---- v15: migration from 0xAC (v14 layout): graph settings kept, language defaulted; SYSTem:LANGuage saves at once ----
    EEPROM.reset();
    {
        KeithleySettings v14;
        memset(&v14, 0xFF, sizeof(v14));
        v14.magic = SETTINGS_MAGIC_V14;
        v14.readingFormat = 1;
        v14.autoEnabled = 0;
        v14.autoIntervalSeconds = 0;
        v14.theme = 1;
        v14.debugEnabled = 0;
        v14.beepEnabled = 1;
        v14.commPort = SETTINGS_COMM_UART;
        v14.displayYOffset = 22;
        v14.statsInterval = 3;
        v14.debugPort = SETTINGS_COMM_UART;
        v14.graphInterval = 4;
        v14.graphYMode = 2;
        v14.graphTrace = 1;
        v14.graphSpan = 6;
        EEPROM.put(0, v14);
        EEPROM.commit();
        settingsBegin();
        CHECK(settingsGetGraphInterval() == 4 + GRAPH_INTERVALS_ADDED_V15 && settingsGetGraphYMode() == 2 && settingsGetGraphTrace() == 1 &&
              settingsGetGraphSpan() == 6, "0xAC migration keeps the graph settings (interval index moved past 10/30 SEC)");
        CHECK(settingsGetLanguage() == CONFIG_DEFAULT_LANGUAGE && settingsGetDisplayYOffset() == 22, "0xAC migration: language default, old fields kept");
        int commits = EEPROM.commits;
        settingsSetLanguage(1);
        CHECK(EEPROM.commits == commits + 1, "language saved at once");
        settingsBegin();
        CHECK(settingsGetLanguage() == 1, "language persists");
    }

    // ---- v15: 0xAD -> 0xAE: same layout, the graph interval index moves past the new 10 SEC / 30 SEC, language kept ----
    EEPROM.reset();
    {
        KeithleySettings v15;
        memset(&v15, 0, sizeof(v15));
        v15.magic = SETTINGS_MAGIC_V15;
        v15.theme = 2;
        v15.displayYOffset = 20;
        v15.statsInterval = 2;
        v15.graphInterval = 3; // 1 HOUR in the 0xAD numbering
        v15.graphYMode = 1;
        v15.language = 1;
        EEPROM.put(0, v15);
        EEPROM.commit();
        settingsBegin();
        CHECK(settingsGetGraphInterval() == 5 && settingsGetGraphYMode() == 1, "0xAD migration: 1 HOUR stays 1 HOUR (index 3 -> 5)");
        CHECK(settingsGetLanguage() == 1 && settingsGetDisplayYOffset() == 20 && settingsGetStatsInterval() == 2, "0xAD migration keeps the rest");
        settingsBegin();
        CHECK(settingsGetGraphInterval() == 5, "migrated once, not again");
    }

    // ---- v15: theme / sound / language from the DMM settings screen: deferred; SCPI setters stay immediate ----
    {
        int commits = EEPROM.commits;
        settingsSetThemeDeferred(5);
        settingsSetBeepEnabledDeferred(false);
        settingsSetLanguageDeferred(0);
        CHECK(EEPROM.commits == commits && settingsSavePending() && settingsGetTheme() == 5 && !settingsGetBeepEnabled(), "front panel: in RAM at once, flash later");
        fakeMillisOffset() += CONFIG_SETTINGS_DEFERRED_SAVE_MS + 10;
        settingsPoll();
        CHECK(EEPROM.commits == commits + 1, "one write after the delay");
        settingsBegin();
        CHECK(settingsGetTheme() == 5 && !settingsGetBeepEnabled() && settingsGetLanguage() == 0, "persisted");
        settingsSetTheme(2);
        CHECK(EEPROM.commits == commits + 2, "SCPI setter saves at once");
    }

    // ---- v17: 0xAE -> 0xAF: everything kept, the alarm defaulted (no limits, BOTH, signal default); deferred/immediate ----
    EEPROM.reset();
    {
        KeithleySettings v16;
        memset(&v16, 0, sizeof(v16));
        v16.magic = SETTINGS_MAGIC_V16;
        v16.theme = 4;
        v16.displayYOffset = 19;
        v16.graphInterval = 6;
        v16.language = 1;
        EEPROM.put(0, v16);
        EEPROM.commit();
        settingsBegin();
        CHECK(settingsGetTheme() == 4 && settingsGetDisplayYOffset() == 19 && settingsGetGraphInterval() == 6 && settingsGetLanguage() == 1,
              "0xAE migration keeps everything (graph interval NOT moved again)");
        uint8_t mode; float low, high;
        settingsGetAlarm(0, &mode, &low, &high);
        CHECK(mode == 3 && isnan(low) && isnan(high) && settingsGetAlarmSignal() == CONFIG_ALARM_DEFAULT_SIGNAL, "alarm defaults after the migration");
        int commits = EEPROM.commits;
        settingsSetAlarmDeferred(4, 1, -5.0f, 12.5f);
        CHECK(EEPROM.commits == commits && settingsSavePending(), "alarm from the front panel: deferred");
        fakeMillisOffset() += CONFIG_SETTINGS_DEFERRED_SAVE_MS + 10;
        settingsPoll();
        CHECK(EEPROM.commits == commits + 1, "one write");
        settingsSetAlarm(2, 2, 0.001f, NAN);
        settingsSetAlarmSignal(1);
        CHECK(EEPROM.commits == commits + 3, "SCPI alarm setters save at once");
        settingsBegin();
        settingsGetAlarm(4, &mode, &low, &high);
        CHECK(mode == 1 && low == -5.0f && high == 12.5f, "OHM alarm persists");
        settingsGetAlarm(2, &mode, &low, &high);
        CHECK(mode == 2 && low == 0.001f && isnan(high) && settingsGetAlarmSignal() == 1, "DCI alarm + signal persist (NAN = not set)");
        settingsGetAlarm(9, &mode, &low, &high);
        CHECK(mode == 0 && isnan(low), "function out of range");
        printf("settings 0xAF: alarm migration and persistence, struct %u bytes\n", (unsigned)sizeof(KeithleySettings));
    }

    // ---- unknown magic -> defaults ----
    EEPROM.reset();
    EEPROM.data[0] = 0x42;
    settingsBegin();
    CHECK(settingsGetDisplayYOffset() == CONFIG_DEFAULT_DISPLAY_Y_OFFSET, "unknown layout -> defaults");

    printf(failures ? "settings: %d FAILURES\n" : "settings: all OK\n", failures);
    return failures ? 1 : 0;
}
