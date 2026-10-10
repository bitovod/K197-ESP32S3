// trigger_io.ino
// See trigger_io.h.

#include "trigger_io.h"
#include "config.h"

static volatile uint32_t triggerIoExtCount = 0;

// GPIO interrupt: only counts. Kept to a single store so it is safe in
// interrupt context; the SCPI side compares the count with what it has
// already handled (serialOutUpdateReading(), serial_out.ino).
static volatile uint32_t triggerIoExtLastMs = 0; // v18: millis() of the newest edge

static void triggerIoExtIsr()
{
    triggerIoExtLastMs = millis();
    triggerIoExtCount = triggerIoExtCount + 1; // plain read + write: ++ on a volatile is deprecated in C++20; this ISR is the only writer
}

void triggerIoBegin()
{
    pinMode(MC_OUT_PIN, OUTPUT);
    digitalWrite(MC_OUT_PIN, CONFIG_MC_OUT_ACTIVE_LEVEL == HIGH ? LOW : HIGH); // idle
    pinMode(EXT_TRIG_IN_PIN, CONFIG_EXT_TRIG_PULLUP ? INPUT_PULLUP : INPUT);
    attachInterrupt(digitalPinToInterrupt(EXT_TRIG_IN_PIN), &triggerIoExtIsr, CONFIG_EXT_TRIG_EDGE);
}

void triggerIoPulseMeteringComplete()
{
    digitalWrite(MC_OUT_PIN, CONFIG_MC_OUT_ACTIVE_LEVEL);
    delayMicroseconds(CONFIG_MC_PULSE_US);
    digitalWrite(MC_OUT_PIN, CONFIG_MC_OUT_ACTIVE_LEVEL == HIGH ? LOW : HIGH);
}

uint32_t triggerIoExternalCount()
{
    return triggerIoExtCount;
}

uint32_t triggerIoExternalLastMs()
{
    return triggerIoExtLastMs;
}

// ---- v18: the input as a marker key (MEASure:TRIGger MARK) ----
// A contact (foot switch) bounces on press AND release, and the release
// bounce also has edges of the triggering direction. So a burst of edges is
// only taken once the line has been quiet for CONFIG_EXT_TRIG_SETTLE_MS, and
// only if it then rests at the ACTIVE level (pressed); the press must also be
// CONFIG_MARKER_EXT_HOLDOFF_MS after the last accepted one. Comm task only.
static uint32_t triggerIoKeySeen = 0;
static bool triggerIoKeyBurst = false;
static uint32_t triggerIoKeyBurstMs = 0;
static uint32_t triggerIoKeyAcceptedMs = 0;
static bool triggerIoKeyAcceptedAny = false;

void triggerIoKeySync()
{
    triggerIoKeySeen = triggerIoExtCount;
    triggerIoKeyBurst = false;
}

bool triggerIoTakeKeyPress(uint32_t *firstEdgeMs)
{
    uint32_t count = triggerIoExtCount;
    if (count != triggerIoKeySeen)
    {
        if (!triggerIoKeyBurst)
        {
            triggerIoKeyBurst = true;
            triggerIoKeyBurstMs = triggerIoExtLastMs; // (close to) the first edge of the burst
        }
        triggerIoKeySeen = count;
    }
    if (!triggerIoKeyBurst || (uint32_t)((uint32_t)millis() - triggerIoExtLastMs) < (uint32_t)CONFIG_EXT_TRIG_SETTLE_MS)
    {
        return false;
    }
    triggerIoKeyBurst = false;
    const int activeLevel = (CONFIG_EXT_TRIG_EDGE == FALLING) ? LOW : HIGH;
    if (digitalRead(EXT_TRIG_IN_PIN) != activeLevel)
    {
        return false; // a release (or a spike): the line is back at rest
    }
    if (triggerIoKeyAcceptedAny &&
        (uint32_t)(triggerIoKeyBurstMs - triggerIoKeyAcceptedMs) < (uint32_t)CONFIG_MARKER_EXT_HOLDOFF_MS)
    {
        return false;
    }
    triggerIoKeyAcceptedAny = true;
    triggerIoKeyAcceptedMs = triggerIoKeyBurstMs;
    *firstEdgeMs = triggerIoKeyBurstMs;
    return true;
}
