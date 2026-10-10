// trigger_io.h
// Rear-panel trigger signals (v13), each through an optocoupler to a
// connector on the rear panel:
//
//   METERING COMPLETE OUTPUT (MC_OUT_PIN): a pulse after every complete
//   DMM frame (one per measurement, about 3.5 per second). At the
//   connector it is a pulse to zero, at least CONFIG_MC_PULSE_US long;
//   the receiving system should react to its falling edge.
//
//   EXTERNAL TRIGGER INPUT (EXT_TRIG_IN_PIN): a pulse to zero at the
//   connector. Only emulated - the K197 itself measures continuously and
//   cannot be triggered: with MEASure:TRIGger EXT (serial_out.ino) the
//   first complete frame decoded after the edge is sent once over SCPI;
//   v18: with MEASure:TRIGger MARK an edge puts a marker (marker.h) instead.
//   The edge is caught by a GPIO interrupt, so a pulse of a few
//   microseconds is enough as far as the ESP32 is concerned; in practice
//   the optocoupler sets the limit (see config.h).
//
// The polarity on the GPIO side depends on how the optocouplers are wired
// and is set in config.h (CONFIG_MC_OUT_ACTIVE_LEVEL, CONFIG_EXT_TRIG_EDGE).

#ifndef TRIGGER_IO_H
#define TRIGGER_IO_H

#include <Arduino.h>

// Call once from setup(): configures both pins (output idle, input with
// pull-up) and attaches the trigger interrupt.
void triggerIoBegin();

// Generates one METERING COMPLETE pulse. Called by the DMM capture task
// (dmm_bridge.ino) after each complete frame; busy-waits
// CONFIG_MC_PULSE_US (10 us), which that task can afford.
void triggerIoPulseMeteringComplete();

// Number of EXTERNAL TRIGGER edges seen since boot (wraps around). A
// consumer remembers the value it has handled and compares - nothing is
// reset or consumed here, so any number of tasks may read it.
uint32_t triggerIoExternalCount();

// v18: millis() of the newest EXTERNAL TRIGGER edge (0 before the first one).
// Read it AFTER the count: the ISR writes the time first.
uint32_t triggerIoExternalLastMs();

// v18: the EXTERNAL TRIGGER input as a key (a foot switch for markers): true
// once per press, debounced (see trigger_io.ino), *firstEdgeMs = when it was
// pressed. triggerIoKeySync() forgets edges seen so far (call when switching
// to MEASure:TRIGger MARK). Both from one task (the comm task).
void triggerIoKeySync();
bool triggerIoTakeKeyPress(uint32_t *firstEdgeMs);

#endif // TRIGGER_IO_H
