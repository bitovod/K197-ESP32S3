// marker.ino
// See marker.h.

#include "marker.h"
#include "config.h"
#include "datalog.h"
#include "beeper.h"
#include "display.h"
#include "debuglog.h"
#include <esp_timer.h>

// Feedback sequence on one one-shot timer: first beep at once, the second one
// after CONFIG_MARKER_BEEP_MS + CONFIG_MARKER_BEEP_GAP_MS, "TAG" off after
// CONFIG_MARKER_SHOW_MS. A new marker restarts it.
#define MARKER_STEP_SECOND_BEEP 1
#define MARKER_STEP_TAG_OFF     2

static esp_timer_handle_t markerTimer = NULL;
static volatile uint8_t markerStep = 0;
static volatile uint32_t markerSince = 0; // millis() of the last marker (for the TAG-off time)
static volatile uint32_t markerTotal = 0;
static portMUX_TYPE markerMux = portMUX_INITIALIZER_UNLOCKED;

static void markerTimerCallback(void *arg)
{
    (void)arg;
    if (markerStep == MARKER_STEP_SECOND_BEEP)
    {
        beeperBeepAsync(CONFIG_MARKER_BEEP_MS);
        markerStep = MARKER_STEP_TAG_OFF;
        uint32_t elapsed = (uint32_t)millis() - markerSince;
        uint32_t rest = (elapsed < (uint32_t)CONFIG_MARKER_SHOW_MS) ? (uint32_t)CONFIG_MARKER_SHOW_MS - elapsed : 1;
        esp_timer_start_once(markerTimer, (uint64_t)rest * 1000ULL);
        return;
    }
    markerStep = 0;
    setTagIndicator(false);
}

void markerBegin()
{
    esp_timer_create_args_t timerArgs = {};
    timerArgs.callback = &markerTimerCallback;
    timerArgs.arg = NULL;
    timerArgs.dispatch_method = ESP_TIMER_TASK;
    timerArgs.name = "marker";
    esp_timer_create(&timerArgs, &markerTimer);
    setTagIndicator(false);
}

bool markerAdd(uint32_t atMs, uint8_t source)
{
    uint32_t serial = 0;
    if (!datalogMarkAtMs(atMs, &serial))
    {
        debugLogf("MARK: no record to mark");
        return false;
    }
    portENTER_CRITICAL(&markerMux);
    markerTotal = markerTotal + 1;
    markerSince = (uint32_t)millis();
    markerStep = MARKER_STEP_SECOND_BEEP;
    portEXIT_CRITICAL(&markerMux);

    setTagIndicator(true);
    beeperBeepAsync(CONFIG_MARKER_BEEP_MS);
    if (markerTimer != NULL)
    {
        esp_timer_stop(markerTimer); // a running sequence starts over (stop fails harmlessly if idle)
        esp_timer_start_once(markerTimer, (uint64_t)(CONFIG_MARKER_BEEP_MS + CONFIG_MARKER_BEEP_GAP_MS) * 1000ULL);
    }
    static const char *const names[] = {"keys", "EXT TRIG", "SCPI"};
    debugLogf("MARK: record serial %lu (%s)", (unsigned long)serial, (source <= MARKER_SOURCE_SCPI) ? names[source] : "?");
    return true;
}

uint32_t markerCount()
{
    return markerTotal;
}
