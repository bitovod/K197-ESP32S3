// debuglog.h
// Debug output that can never make the caller wait. New in v10.
//
// Why it exists: a Serial.print() on the ESP32-S3's USB port can block for
// a long time when the PC on the other end is slow to take the data
// (or reads it only in bursts) - every print of a message of N pieces
// is N such waits. A task that prints is then as slow as the USB link,
// and if that task is the button task, the DMM capture task or the UI
// task, the whole instrument feels laggy exactly while a USB cable is
// connected (v9 printed "BTN ..." lines from the button task for every
// press, whether SYSTem:DEBUG was on or not).
//
// So the tasks that must not wait do not touch Serial any more. They
// format a message (at most DEBUGLOG_MESSAGE_CHARS characters) and put it
// into a small queue - a few microseconds, never blocking; if the queue
// is full the message is dropped and counted. Only the communication task
// (core1Task() in the main sketch), which is allowed to wait, empties the
// queue and writes the messages to Serial, one write per message.
//
// Messages are plain printf-style text without a trailing newline (it is
// added when the message is printed).

#ifndef DEBUGLOG_H
#define DEBUGLOG_H

#include <Arduino.h>
#include <stdarg.h>

#define DEBUGLOG_MESSAGE_CHARS 128 // longer messages are cut
#define DEBUGLOG_QUEUE_LENGTH  24  // messages waiting for the communication task

// Call once from setup(), before any task starts to log.
void debugLogBegin();

// Queues a message, but only while SYSTem:DEBUG is on (and no SCPI line is
// half typed - see serialOutDebugTraceAllowed()). Does nothing otherwise.
void debugLogf(const char *format, ...) __attribute__((format(printf, 1, 2)));

// Queues a message whatever SYSTem:DEBUG says - for errors that are worth
// always seeing.
void debugLogAlways(const char *format, ...) __attribute__((format(printf, 1, 2)));

// Writes all queued messages to the output stream (Serial by default).
// Call from the one task that may wait for the link (core1Task()). Also
// reports how many messages were dropped since the last time, if any.
void debugLogDrain();

// v13: chooses the stream debugLogDrain() writes to (SYSTem:DEBUG:PORT,
// serial_out.ino: Serial = USB CDC, Serial1 = SCPI UART). `prefix` puts
// "# " in front of every line (used on the UART, where SCPI replies go
// too). Call from the same task as debugLogDrain().
void debugLogSetOutput(Stream *stream, bool prefix);

// Total number of messages dropped because the queue was full (for tests
// and diagnostics).
uint32_t debugLogDropped();

#endif // DEBUGLOG_H
