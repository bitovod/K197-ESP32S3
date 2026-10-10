// beeper.h
// Acoustic signalling on BEEPER_PIN (config.h) - an active buzzer
// module (has its own oscillator built in), driven with a plain
// digitalWrite() HIGH/LOW, not tone()/PWM (see config.h's comment on
// BEEPER_PIN).
//
// Currently just the one-shot startup chirp - a single short beep
// once setup() finishes, gated by the persistent SYSTem:BEEP setting
// (settings.h/serial_out.ino). Written as its own small module (like
// rtc.h/rtc.ino) rather than folded into the main sketch file, since
// the plan is to grow this into several distinct beep patterns later
// (see docs/DEVELOPMENT.md) - an active buzzer only has one fixed
// pitch, so "distinct" here means timing (short/long, single/double
// beep, ...), not tone pitch - beeperBeep() below is the one
// low-level primitive everything else, then and now, builds on.

#ifndef BEEPER_H
#define BEEPER_H

#include <Arduino.h>

// Call once from setup(), before any of the functions below.
void beeperBegin();

// Turns the buzzer on, blocks for durationMs, turns it back off.
// Silent, with no error, if SYSTem:BEEP is currently OFF - every
// higher-level beep (beeperPlayStartupChirp() below, and whatever
// else gets added later) is expected to go through this one function
// so that single setting gates all of them uniformly, rather than
// each caller re-checking settingsGetBeepEnabled() itself.
void beeperBeep(uint16_t durationMs);

// One short chirp, CONFIG_BEEP_STARTUP_DURATION_MS long (config.h).
// Call once from setup(), after settingsBegin() (so SYSTem:BEEP's
// persisted value is already loaded) - see ESP32S3_K197.ino.
void beeperPlayStartupChirp();

// One short click, CONFIG_BEEP_CLICK_MS long (config.h), played by the
// button task for every confirmed front-panel press. Same gating by
// SYSTem:BEEP as every other beep (it goes through beeperBeep()).
void beeperClick();

// Turns the buzzer on for durationMs WITHOUT blocking: returns at once, a
// one-shot esp_timer switches it off. For long beeps requested from tasks
// that must keep running (the render/UI task). While such a beep sounds,
// beeperBeep()/beeperClick() stay silent so their own "off" cannot cut it
// short. Same SYSTem:BEEP gating. A new call while one is running restarts
// the duration.
void beeperBeepAsync(uint16_t durationMs);

// The long confirmation beep after the log was cleared from the front
// panel, CONFIG_BEEP_LOG_CLEAR_MS long (config.h), non-blocking.
void beeperPlayLogCleared();

#endif // BEEPER_H
