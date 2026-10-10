// beeper.ino
// See beeper.h.

#include "beeper.h"
#include "config.h"
#include "settings.h"
#include <esp_timer.h>

// One-shot timer that ends an asynchronous beep (beeperBeepAsync()).
static esp_timer_handle_t beeperOffTimer = NULL;
// True while an asynchronous beep sounds - the blocking beeperBeep() (button
// clicks) then stays silent, otherwise its digitalWrite(LOW) would cut the
// long beep short.
static volatile bool beeperAsyncActive = false;

// esp_timer callback (esp_timer service task, not an ISR - digitalWrite()
// is fine here).
static void beeperOffCallback(void *arg)
{
    (void)arg;
    digitalWrite(BEEPER_PIN, LOW);
    beeperAsyncActive = false;
}

void beeperBegin()
{
    pinMode(BEEPER_PIN, OUTPUT);
    digitalWrite(BEEPER_PIN, LOW); // idle quiet

    esp_timer_create_args_t timerArgs = {};
    timerArgs.callback = &beeperOffCallback;
    timerArgs.arg = NULL;
    timerArgs.dispatch_method = ESP_TIMER_TASK;
    timerArgs.name = "beeper_off";
    esp_timer_create(&timerArgs, &beeperOffTimer);
}

void beeperBeep(uint16_t durationMs)
{
    if (!settingsGetBeepEnabled())
        return;
    if (beeperAsyncActive)
        return; // a long beep is sounding - it already tells the person something happened

    digitalWrite(BEEPER_PIN, HIGH);
    delay(durationMs);
    if (!beeperAsyncActive) // a long beep may have started meanwhile: leave it on
    {
        digitalWrite(BEEPER_PIN, LOW);
    }
}

void beeperBeepAsync(uint16_t durationMs)
{
    if (!settingsGetBeepEnabled())
        return;
    if (beeperOffTimer == NULL)
        return; // beeperBegin() not called / timer creation failed

    if (beeperAsyncActive)
    {
        esp_timer_stop(beeperOffTimer); // restart the duration (stop fails harmlessly if it just fired)
    }
    beeperAsyncActive = true;
    digitalWrite(BEEPER_PIN, HIGH);
    esp_timer_start_once(beeperOffTimer, (uint64_t)durationMs * 1000ULL);
}

void beeperPlayStartupChirp()
{
    beeperBeep(CONFIG_BEEP_STARTUP_DURATION_MS);
}

void beeperClick()
{
    beeperBeep(CONFIG_BEEP_CLICK_MS);
}

void beeperPlayLogCleared()
{
    beeperBeepAsync(CONFIG_BEEP_LOG_CLEAR_MS);
}
