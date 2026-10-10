// marker.h
// v18: markers - "this reading, now" put into the log, to be found again when
// the log is evaluated. A marker is one bit in the record that was SHOWN at
// the moment of the request (datalog.h DATALOG_BIT_MARKER), so it costs no
// log space; LOG:DATA? and LOG:MARKers? list it as "TAG" among the
// indicators, the graph draws a small triangle at the top edge of its column.
//
// Sources:
//   KEYS  REL and dB held together on the LCD or the graph screen
//         (buttonsTakeChord(), ui.ino) - timed at the first edge of the press
//         that came first, so the time it takes to press the second key does
//         not matter
//   EXT   an edge on the rear EXTERNAL TRIGGER input with MEASure:TRIGger MARK
//         (serial_out.ino) - edges within CONFIG_MARKER_EXT_HOLDOFF_MS of the
//         last accepted one are ignored (a foot switch bounces)
//   SCPI  LOG:MARK - the newest record
//
// Feedback: a double beep (CONFIG_MARKER_BEEP_MS, SYSTem:BEEP permitting) and
// "TAG" - drawn in place of REM on the LCD replica (this K197 never gets the
// GPIB board) - lit for CONFIG_MARKER_SHOW_MS; REM itself is still drawn when
// the K197 lights it (its self-test).
//
// Any task except the capture task (it reads/writes the log under its mutex).

#ifndef MARKER_H
#define MARKER_H

#include <Arduino.h>

#define MARKER_SOURCE_KEYS 0
#define MARKER_SOURCE_EXT  1
#define MARKER_SOURCE_SCPI 2

// Call once from setup() (after beeperBegin()): creates the feedback timer.
void markerBegin();

// Marks the record shown at millis() `atMs` and gives the feedback. Returns
// false (and stays silent) if the log has no record yet.
bool markerAdd(uint32_t atMs, uint8_t source);

// Markers set since boot (all sources).
uint32_t markerCount();

#endif // MARKER_H
