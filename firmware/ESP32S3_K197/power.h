// power.h
// v22: low-power waiting while the K197 is switched off.
//
// The ESP32 may keep running from its own supply (USB, or a battery added
// later) while the K197 is off - that keeps the PSRAM log. When the DMM
// capture has seen a whole no-data period without a single CS edge and CS
// is low (dmm_bridge.ino: "oFF197" on the LCD replica), and no key has been
// touched for CONFIG_POWER_KEY_AWAKE_MS, the ESP32 goes into light sleep.
// PSRAM, RAM and every task survive it; the panel keeps its picture on its
// own (GRAM), so "oFF197" (or whatever screen was open) stays visible.
//
// Wake-up sources (levels, as light sleep needs):
//   - DMM CS high: the K197 is powered again (its bus idles high, the
//     input divider holds it low while the K197 is off)
//   - any front-panel key low: the screens can be used while the DMM is off;
//     the press that wakes does nothing else (the key interrupts stay off
//     until every key is released)
//   - the SCPI UART RX low (a start bit), CONFIG_POWER_WAKE_ON_UART - the
//     first command after a wake-up is usually lost, send it again
//   - a timer every CONFIG_POWER_TIMER_WAKE_S (0 = none), a safety net
// After a DMM wake-up the SPI slave is restarted (dmm_bridge.ino).
//
// In light sleep the native USB (CDC) stops answering - only relevant while
// developing; CONFIG_POWER_SLEEP_ENABLE 0 switches the sleeping off.
//
// Runs in the comm task (core1Task(), ESP32S3_K197.ino): powerPoll() every pass.

#ifndef POWER_H
#define POWER_H

#include <Arduino.h>

void powerBegin();
void powerPoll();

struct PowerStats
{
    uint32_t sleeps;        // light sleeps entered
    uint32_t asleepSeconds; // total time asleep
    uint8_t lastWake;       // POWER_WAKE_*
};

#define POWER_WAKE_NONE  0
#define POWER_WAKE_DMM   1
#define POWER_WAKE_KEY   2
#define POWER_WAKE_UART  3
#define POWER_WAKE_TIMER 4
#define POWER_WAKE_OTHER 5

PowerStats powerGetStats();
const char *powerWakeName(uint8_t wake);

#endif // POWER_H
