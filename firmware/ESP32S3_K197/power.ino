// power.ino
// See power.h.

#include "power.h"
#include "config.h"
#include "display.h"
#include "settings.h"
#include "serial_out.h"
#include "debuglog.h"
#include "ui.h" // v22: back to the main display before sleeping
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <esp_timer.h>

// dmm_bridge.ino (no header of its own - Arduino generates the prototypes)
bool dmmIsOff();
void dmmRequestBusRestart();

static const uint8_t powerKeyPins[4] = {BTN_REL_PIN, BTN_DB_PIN, BTN_STO_PIN, BTN_RCL_PIN};

static uint32_t powerAwakeSinceMs = 0; // last key, SCPI byte or wake-up
static uint32_t powerAwakeForMs = 0;   // how long to stay awake after it
static uint32_t powerLastCheckMs = 0;
static PowerStats powerStats = {0, 0, POWER_WAKE_NONE};
static uint64_t powerAsleepUs = 0;

void powerBegin()
{
    powerAwakeSinceMs = (uint32_t)millis();
    powerAwakeForMs = CONFIG_POWER_KEY_AWAKE_MS;
}

static bool powerAnyKeyDown()
{
    for (uint8_t i = 0; i < sizeof(powerKeyPins); i++)
    {
        if (digitalRead(powerKeyPins[i]) == LOW)
        {
            return true;
        }
    }
    return false;
}

static void powerStayAwake(uint32_t nowMs, uint32_t forMs)
{
    powerAwakeSinceMs = nowMs;
    powerAwakeForMs = forMs;
}

// One light sleep, everything around it. Returns false if the display did not become idle in time (not slept).
static bool powerSleepOnce()
{
    displaySetPaused(true);
    uint32_t startMs = (uint32_t)millis();
    while (!displayIsIdle())
    {
        if ((uint32_t)(millis() - startMs) > CONFIG_POWER_IDLE_WAIT_MS)
        {
            displaySetPaused(false);
            return false;
        }
        vTaskDelay(1);
    }
    debugLogDrain(); // the messages so far still go out

    // Level wake-ups. The interrupts of these pins are switched off first: the wake-up needs the pin's interrupt
    // type set to a level, and a level interrupt left enabled would fire without end after the wake-up.
    for (uint8_t i = 0; i < sizeof(powerKeyPins); i++)
    {
        gpio_intr_disable((gpio_num_t)powerKeyPins[i]);
        gpio_wakeup_enable((gpio_num_t)powerKeyPins[i], GPIO_INTR_LOW_LEVEL);
    }
    gpio_intr_disable((gpio_num_t)DMM_CS_PIN);
    gpio_wakeup_enable((gpio_num_t)DMM_CS_PIN, GPIO_INTR_HIGH_LEVEL);
#if CONFIG_POWER_WAKE_ON_UART
    gpio_wakeup_enable((gpio_num_t)SERIAL_OUT_RX_PIN, GPIO_INTR_LOW_LEVEL);
#endif
    esp_sleep_enable_gpio_wakeup();
#if CONFIG_POWER_TIMER_WAKE_S > 0
    esp_sleep_enable_timer_wakeup((uint64_t)CONFIG_POWER_TIMER_WAKE_S * 1000000ULL);
#endif

    int64_t t0 = esp_timer_get_time();
    esp_light_sleep_start();
    int64_t t1 = esp_timer_get_time();

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    bool keyDown = powerAnyKeyDown();
    bool csHigh = digitalRead(DMM_CS_PIN) == HIGH;
#if CONFIG_POWER_WAKE_ON_UART
    bool rxLow = digitalRead(SERIAL_OUT_RX_PIN) == LOW;
    gpio_wakeup_disable((gpio_num_t)SERIAL_OUT_RX_PIN);
    gpio_set_intr_type((gpio_num_t)SERIAL_OUT_RX_PIN, GPIO_INTR_DISABLE);
#else
    bool rxLow = false;
#endif
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    gpio_wakeup_disable((gpio_num_t)DMM_CS_PIN);
    gpio_set_intr_type((gpio_num_t)DMM_CS_PIN, GPIO_INTR_NEGEDGE); // the CS edge counter (re-armed by the restart too)
    gpio_intr_enable((gpio_num_t)DMM_CS_PIN);

    for (uint8_t i = 0; i < sizeof(powerKeyPins); i++)
    {
        gpio_wakeup_disable((gpio_num_t)powerKeyPins[i]);
    }
    if (keyDown)
    {
        // the waking press does nothing else: the key interrupts come back once every key is up
        uint32_t waitMs = (uint32_t)millis();
        while (powerAnyKeyDown() && (uint32_t)(millis() - waitMs) < CONFIG_POWER_KEY_RELEASE_WAIT_MS)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_BUTTON_DEBOUNCE_MS)); // past the release bounce
    }
    for (uint8_t i = 0; i < sizeof(powerKeyPins); i++)
    {
        gpio_set_intr_type((gpio_num_t)powerKeyPins[i], GPIO_INTR_ANYEDGE); // attachInterrupt(CHANGE)
        gpio_intr_enable((gpio_num_t)powerKeyPins[i]);
    }

    uint8_t wake = POWER_WAKE_OTHER;
    uint32_t nowMs = (uint32_t)millis();
    if (cause == ESP_SLEEP_WAKEUP_TIMER)
    {
        wake = POWER_WAKE_TIMER;
        powerStayAwake(nowMs, CONFIG_POWER_TIMER_AWAKE_MS);
    }
    else if (keyDown)
    {
        wake = POWER_WAKE_KEY;
        powerStayAwake(nowMs, CONFIG_POWER_KEY_AWAKE_MS);
    }
    else if (csHigh)
    {
        wake = POWER_WAKE_DMM;
        powerStayAwake(nowMs, CONFIG_POWER_MIN_AWAKE_MS);
    }
    else if (rxLow)
    {
        wake = POWER_WAKE_UART;
        powerStayAwake(nowMs, CONFIG_POWER_KEY_AWAKE_MS);
    }
    else
    {
        powerStayAwake(nowMs, CONFIG_POWER_MIN_AWAKE_MS);
    }
    dmmRequestBusRestart(); // always: the SPI slave may not have survived the sleep

    powerAsleepUs += (uint64_t)(t1 - t0);
    powerStats.sleeps++;
    powerStats.asleepSeconds = (uint32_t)(powerAsleepUs / 1000000ULL);
    powerStats.lastWake = wake;
    displaySetPaused(false);
    displayWakeUi();
    debugLogAlways("power: woke after %lu ms (%s)", (unsigned long)((t1 - t0) / 1000), powerWakeName(wake));
    return true;
}

void powerPoll()
{
    uint32_t nowMs = (uint32_t)millis();
    if ((uint32_t)(nowMs - powerLastCheckMs) < CONFIG_POWER_CHECK_MS)
    {
        return;
    }
    powerLastCheckMs = nowMs;

    if (powerAnyKeyDown())
    {
        powerStayAwake(nowMs, CONFIG_POWER_KEY_AWAKE_MS);
        return;
    }
    uint32_t rxMs = serialOutLastRxMs();
    if (rxMs != 0 && (uint32_t)(nowMs - rxMs) < CONFIG_POWER_KEY_AWAKE_MS && (int32_t)(rxMs - powerAwakeSinceMs) > 0)
    {
        powerStayAwake(rxMs, CONFIG_POWER_KEY_AWAKE_MS);
    }
#if CONFIG_POWER_SLEEP_ENABLE
    if (!dmmIsOff() || digitalRead(DMM_CS_PIN) != LOW)
    {
        return;
    }
    if ((uint32_t)(nowMs - powerAwakeSinceMs) < powerAwakeForMs)
    {
        return;
    }
    if (settingsSavePending())
    {
        return; // the deferred EEPROM write first (settingsPoll() in the same loop)
    }
    if (uiGetScreen() != UI_SCREEN_LCD)
    {
        uiRequestHome(); // a screen opened after a key wake-up: back to oFF197 first, sleep on a later pass
        return;
    }
    powerSleepOnce();
#endif
}

PowerStats powerGetStats()
{
    return powerStats;
}

const char *powerWakeName(uint8_t wake)
{
    switch (wake)
    {
        case POWER_WAKE_DMM:
            return "DMM";
        case POWER_WAKE_KEY:
            return "key";
        case POWER_WAKE_UART:
            return "SCPI UART";
        case POWER_WAKE_TIMER:
            return "timer";
        case POWER_WAKE_OTHER:
            return "other";
        default:
            return "none";
    }
}
