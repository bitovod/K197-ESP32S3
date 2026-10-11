// sdlog.h
// v23: SD card logging - the PSRAM reading log (datalog.h) copied to CSV
// files on an SD card (SDMMC, 1-bit mode, pins SD_CLK_PIN/SD_CMD_PIN/SD_D0_PIN
// in config.h).
//
// How it works: a task of its own (core 1, CONFIG_SD_TASK_PRIORITY) follows
// the log by SERIAL number, like the graph does (datalogReadSerial()), so the
// capture task never waits for the card. Every record becomes one CSV line,
// collected in a RAM buffer (CONFIG_SD_BUFFER_BYTES) and written + synced to
// the card every CONFIG_SD_FLUSH_MS - a power loss costs at most that much of
// the file. Written + synced at once:
//   - a marker (TAG, REL + dB, LOG:MARK, EXTERNAL TRIGGER with MEAS:TRIG MARK),
//   - a change of the function (the old file is closed, a new one opened),
//   - the K197 switched off (oFF197 - the file is closed; the next reading
//     opens a new file, so "after the K197 was switched on" is a new file),
//   - SD logging switched off (then the card is also unmounted and may be
//     taken out).
// A new file also after the ESP32 starts and after a card was inserted - NOT
// at midnight: an evening-to-morning measurement stays in one file.
//
// Records are only written once CONFIG_SD_LAG_RECORDS newer ones exist (or the
// log was quiet for CONFIG_SD_QUIET_MS), because a marker goes on the record
// SHOWN at the moment of the request, which may be one of the last few. A
// marker always writes up to and including its record. A marker on a record
// already written (rare: two markers within a second or so, or a marker long
// after the last reading) adds a comment line "# TAG: record N".
//
// Nothing gets lost while it is still in PSRAM: without a card, with a full
// card or after a write error the task simply stops reading the log, and
// after a (new) card is mounted it continues with the first record that was
// not yet SYNCED to the old card - into a new file. Only records the ring
// buffer has overwritten meanwhile are lost (counted, SYSTem:DIAGnostic?).
//
// File: /K197/YYYY-MM/K197_YYYY-MM-DD_hh-mm-ss_FUN.csv with the RTC time of
// its first record (FUN = DCV, ACV, DCI, ACI, OHM, dB); _2, _3, ... appended if
// the name exists. Records without an RTC time: /K197/NO-RTC/K197_nnnn_FUN.csv.
// Content: four comment lines starting with "# " (instrument + firmware, the
// function and its unit, the start time, the CSV format), the column header
// "record,timestamp,status,function,value,indicators" and one line per record
// - the same columns and texts as LOG:DATA? (value in base units V, A, Ohm,
// dB; status N/Z/O; indicators AUTO STO RCL REM BAT TAG SETTLE), the record
// number counts from 1 in every file. CSV format (DMM settings screen,
// MMEMory:FORMat): COMMA = ',' + decimal point, SEMICOLON = ';' + decimal
// comma (also in the timestamp's centiseconds). Lines end with CR LF.
//
// SD problem (enabled, no card / card full / write error, the K197 on):
// "SD!" in place of BAT blinks on the LCD replica, and an alarm beep sounds
// every CONFIG_SD_ALARM_PERIOD_MS (an ALARM sound: silent only with the sound
// setting OFF). The first key press while it sounds only silences it (until
// the next problem, buttonsArmSwallow()).
//
// Card: FAT32 (or FAT16) - exFAT only if the ESP32 core was built with it.
// Every file gets its date from the RTC (the system time is set from it).

#ifndef SDLOG_H
#define SDLOG_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "datalog.h" // LogRecord (sdlogFormatLine())

#define SDLOG_STATE_OFF     0 // logging switched off (card unmounted)
#define SDLOG_STATE_NO_CARD 1 // on, no card / the mount failed (a new attempt every CONFIG_SD_RETRY_MS)
#define SDLOG_STATE_READY   2 // card mounted, no file open (no reading yet, or the K197 is off)
#define SDLOG_STATE_LOGGING 3 // a file is open
#define SDLOG_STATE_FULL    4 // less than CONFIG_SD_MIN_FREE_KB free - nothing more is written to this card
#define SDLOG_STATE_ERROR   5 // a file could not be created or written - the card is mounted again
#define SDLOG_STATE_COUNT   6

// For SYSTem:DIAGnostic? and MMEMory:STATe? (a consistent copy).
struct SdlogStats
{
    uint8_t state;           // SDLOG_STATE_*
    bool problem;            // the SD alarm is on (SD! blinking) right now
    bool silenced;           // ... and its beep was silenced by a key
    char file[64];           // the open file, or the last one ("" = none yet)
    uint32_t fileRecords;    // records in that file
    uint32_t records;        // records written since boot
    uint32_t files;          // files opened since boot
    uint32_t backlog;        // records in the log not yet written to the card
    uint32_t lostRecords;    // overwritten in PSRAM before they reached a card
    uint32_t writeErrors;    // failed creates / writes
    uint32_t mounts;         // successful mounts since boot
    uint32_t lastWriteMs;    // the last write (+ sync) to the card
    uint32_t maxWriteMs;     // the longest one
    uint64_t freeBytes;      // free space on the mounted card (counted down by the writes)
};

// Call once from setup(), after settingsBegin(), datalogBegin(), rtcBegin()
// and beeperBegin(): creates the SD task and the blink/beep timer.
void sdlogBegin();

// One pass of the SD task (it calls this every CONFIG_SD_POLL_MS; the host
// tests call it directly).
void sdlogStep();

// One tick of the blink / alarm beep (an esp_timer every CONFIG_SD_BLINK_MS;
// the host tests call it directly).
void sdlogTick();

uint8_t sdlogState();
const char *sdlogStateName(uint8_t state); // "OFF", "NOCARD", "READY", "LOGGING", "FULL", "ERROR"

// True while the SD alarm is on (SD! blinking).
bool sdlogProblem();

// True when a light sleep cannot lose anything: no file open, nothing
// buffered, no card operation running (power.ino).
bool sdlogReadyForSleep();

void sdlogGetStats(SdlogStats *out);

// The SD task, for its stack high-water mark (SYSTem:DIAGnostic?).
TaskHandle_t sdlogGetTask();

// ---- pure helpers (also used by the host tests) ----

// One CSV line (with CR LF) for `rec` as record number `recordNo` of its file.
// Returns the length (the line is cut at size - 1; 128 bytes are always enough).
size_t sdlogFormatLine(char *buf, size_t size, uint32_t recordNo, const LogRecord &rec, uint8_t csvFormat);

// The file name (full path) for a file whose first record has the RTC time
// seconds2000 (0 = none) and `function` ("DCV", ...). suffix 0 = none, n >= 2
// = "_n" before ".csv"; without a time the suffix is the running number.
void sdlogFileName(char *buf, size_t size, uint32_t seconds2000, const char *function, uint16_t suffix);

// "YYYY-MM-DD hh:mm:ss.cc" (decimalComma: ",cc") or "NO-RTC-TIME" for 0.
void sdlogFormatTimestamp(char *buf, size_t size, uint32_t seconds2000, uint8_t centisecond, bool decimalComma);

#endif // SDLOG_H
