// hold.h
// v24: DATA HOLD - the LCD replica keeps showing one reading until it is
// released. Only the LCD replica: the log, the statistics, the graph, the
// alarm, the SD card and SCPI READ? keep working with the live readings.
//
// Front panel (ui.ino): STO held CONFIG_UI_HOLD_HOLD_MS on the LCD screen
// starts HOLD of the kind chosen on the DMM settings screen (settings.h
// SETTINGS_HOLD_*); a short STO (at the release) then ends it - only while
// HOLD is on; otherwise a short STO opens the statistics as before. STO held
// again while HOLD is on starts it over. SCPI: MEASure:HOLD OFF|ON|AUTO / ?.
//
//   MANUAL  at the press of STO the reading SHOWN at its first edge is frozen
//           and "HOLD" (in place of STO) lights at once - a preview
//           (holdPreview(), undone with holdCancelPreview() if it turns out to
//           be a click). After CONFIG_UI_HOLD_HOLD_MS HOLD is confirmed: a short
//           beep, the record marked and "TAG" lit.
//   AUTO    AUTO HOLD: "HOLD" blinks while it waits; the first STABLE reading
//           (CONFIG_HOLD_AUTO_READINGS in a row, same function, unit and
//           decimal point, within +-CONFIG_HOLD_AUTO_TOLERANCE_COUNTS, not
//           settling) that is not OL and at least CONFIG_HOLD_AUTO_MIN_COUNTS
//           away from zero is captured: frozen, "HOLD" + "AUTO" lit, a
//           CONFIG_HOLD_CAPTURE_BEEP_MS beep (an ordinary sound - SOUND ALL)
//           and a marker. It then keeps watching: another stable reading that
//           differs from the held one by more than the tolerance (or has
//           another unit / decimal point) is captured the same way; one run of
//           stable readings captures at most once. A change of the FUNCTION
//           drops the held reading and waits again (live display, blinking).
//
// The K197 switched off (oFF197) ends HOLD (dmm_bridge.ino), and so does a
// restart - nothing of it is persisted except the kind (MANUAL / AUTO).

#ifndef HOLD_H
#define HOLD_H

#include <Arduino.h>
#include "keithley_shapes.h"

#define HOLD_STATE_OFF       0
#define HOLD_STATE_MANUAL    1 // manual HOLD: a reading frozen
#define HOLD_STATE_AUTO_WAIT 2 // AUTO HOLD: no reading captured (live display, HOLD blinks)
#define HOLD_STATE_AUTO_HELD 3 // AUTO HOLD: a captured reading frozen (HOLD + AUTO lit)

// Call once from setup() (after markerBegin(), beeperBegin()): the indicator tick.
void holdBegin();

// Starts HOLD of the given kind (SETTINGS_HOLD_*): manual freezes the reading
// shown at millis() atMs and marks it in the log (silently - showTag lights
// TAG at once; the front panel lights it at the release of STO with
// markerShowTag()); AUTO starts waiting. A HOLD already on starts over. Any
// task except the capture task (the marker).
void holdStart(uint8_t mode, uint32_t atMs, bool showTag);

// Ends HOLD (live display again). Any task.
void holdStop();

// v24: the manual HOLD preview while STO is still held (ui.ino): the reading
// shown at millis() atMs frozen and "HOLD" lit, no marker yet - holdStart()
// takes it over, holdCancelPreview() undoes it (a click). Only while HOLD is off.
void holdPreview(uint32_t atMs);
void holdCancelPreview();

uint8_t holdState();

// Every decoded DMM frame (dmm_bridge.ino, capture task, after
// datalogUpdateReading() and the display setters) - the AUTO HOLD logic. Cheap.
void holdUpdateReading(const char digits[KEITHLEY_DIGIT_COUNT], int8_t dpPos, uint16_t indicatorBits, uint8_t unit);

// The indicator tick (esp_timer every CONFIG_HOLD_TICK_MS; host tests call it):
// blinking, and the marker + beep of an AUTO HOLD capture (the capture task
// may not mark the log itself).
void holdTick();

// AUTO HOLD captures since boot (diagnostics, tests).
uint32_t holdCaptureCount();

#endif // HOLD_H
