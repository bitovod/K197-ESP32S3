// ESP32S3_K197.ino
//
// Ports "Kelthley_197_LED_display_03A.ino" (author: rastro,
// https://www.eevblog.com/forum/projects/keithley-197-led-display-hack/)
// from an Arduino Nano + 74LS959 LED module to an ESP32-S3 driving
// our NV3007 TFT panel.
//
// Dual-core split (new in v6, single-core in
// v3): DMM acquisition runs on core 0 (core0Task()
// below), SCPI/UART communication runs on core 1 (core1Task() below)
// - so a long SCPI reply (a multi-thousand-record LOG:DATA? dump,
// say) can never stall frame capture on the other core.
//
// New in v7: drawing no longer happens inside the capture
// task. The decoded DMM frame only updates a small shared display
// state (display.h's setters, a few microseconds); a render task on
// core 1 composes a complete frame into a 6bpp indexed framebuffer
// and a low-priority blit task on core 0 streams it to the TFT - see
// display.ino's header comment and framebuffer.h. Both are created by
// displayStartTasks() below. Front-panel buttons (buttons.h/
// buttons.ino): the GPIO interrupts catch every edge (and start the
// REL/dB pulse at once); since v9 a small dedicated high-priority task,
// woken by those interrupts, does the debounce confirmation and records
// the press as an event for the UI - how quickly a press is noticed no
// longer depends on what any loop is busy with.
//
// New in v8: a first user interface screen (ui.h/ui.ino):
// the STO button toggles a quick statistics view (live value plus
// AVG/MED/MAX/MIN of the recent log), drawn as text by the same render
// task (framebuffer.h, font_data.h). Settings for it are in config.h.
// New in v9: its layout (interval selector, date and time and the live
// value on the top edge), intervals of 1 minute up to 1 hour (REL =
// shorter, dB = longer), light themes turned around so that the text is
// always light on dark, a plain zero in the font, and the faster,
// sliced statistics computation.
// New in v10: the debug output of the button task, the UI task and the
// DMM capture task no longer goes to Serial directly - it is queued
// (debuglog.h) and written by core1Task(), the only task that may wait
// for the USB link; the "BTN ..." lines are only printed with
// SYSTem:DEBUG ON now; every confirmed button press clicks the beeper for
// CONFIG_BEEP_CLICK_MS; statistics intervals 5 SEC ... 6 HRS; "--.----"
// where a number is not available.
// setup() still runs everything single-threaded first, exactly as
// before, and only spawns the tasks at the very end - see
// docs/DEVELOPMENT.md for the full reasoning, and datalog.ino's
// datalogMutex for the one piece of state genuinely shared between
// the two cores' own work (the LOG:* ring buffer).
//
// This file only holds setup()/loop()/the two per-core task functions
// (including disabling WiFi/Bluetooth right at the top of setup() -
// this project never uses either, see the comment there) - all the
// SPI3 capture and protocol decode logic lives in dmm_bridge.ino, all
// the display logic (state, render task, blit task) lives in
// display.ino, the pixel math (6bpp framebuffer, palette, artwork
// composition) in framebuffer.h, the second-UART SCPI
// command interface lives in serial_out.ino, its persistent settings
// (emulated EEPROM) live in settings.ino, the reading-to-number/
// formatting logic they both share lives in reading_math.ino, the
// PSRAM reading log (LOG:* commands) lives in datalog.ino, the DS3231
// RTC wrapper (SYSTem:TIME/SYSTem:TIME?, and centisecond-resolution
// timestamps for the reading log) lives in rtc.ino - built on the
// third-party DS3231Centisecond library, not a hand-rolled I2C driver
// the way earlier snapshots of this project had it - the acoustic
// signalling (SYSTem:BEEP, the startup chirp) lives in
// beeper.ino, and front-panel button reading + REL/dB pulse
// generation lives in buttons.ino (copy display.h, display.ino,
// framebuffer.h, keithley_shapes.h, serial_out.h, serial_out.ino, settings.h,
// settings.ino, reading_math.h, reading_math.ino, datalog.h,
// datalog.ino, rtc.h, rtc.ino, beeper.h, beeper.ino, buttons.h,
// buttons.ino and config.h into this same sketch folder before
// building - and install the third-party "Vrekrer SCPI parser"
// library via the Arduino Library Manager, since serial_out.ino
// depends on it, AND the third-party "DS3231Centisecond" library
// (copy its own folder into your Arduino libraries directory - it is
// not distributed through the Library Manager) since rtc.ino depends
// on that one).
//
// Every pin assignment, the USB-vs-UART SCPI switch, the SCPI *IDN?
// fields, first-boot defaults and the color themes all live in
// config.h - that is the file to edit to adapt this firmware to a
// different physical build. Nothing in this file or any other should
// need to change for that.
//
// IMPORTANT: this file must be named exactly like the sketch folder,
// so Arduino compiles it first - display.ino and serial_out.ino rely
// on config.h's defines already being visible when they are
// compiled, and config.h is included from this file (transitively,
// via display.h/serial_out.h/settings.h too, but every file that
// needs it includes it directly, so include order between .ino files
// does not actually matter here).

#include "config.h"
#include "display.h"
#include "serial_out.h"
#include "settings.h"
#include "reading_math.h"
#include "datalog.h"
#include "rtc.h"
#include "beeper.h"
#include "buttons.h"
#include "trigger_io.h"
#include "ui.h"
#include "alarm.h" // v17: limit alarm
#include "marker.h" // v18: markers (TAG)
#include "graph.h" // v14: GraphLive/GraphColumn must be known before Arduino's generated prototypes (graphSetLive(), ...)
#include "debuglog.h"
#include "power.h" // v22: PowerStats must be known before the generated prototypes
#include "build_info.h"
#include <esp_system.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <Wire.h>
#include <WiFi.h>
#include "esp_bt.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
// Also included here (not just inside serial_out.ino, which already
// needs it) because of an Arduino IDE quirk: its auto-generated
// function-prototype pass scans the whole concatenated sketch and
// inserts every prototype together near the top of the merged file,
// ahead of a #include that only appears further down inside a
// secondary tab like serial_out.ino - so by the time it tries to
// generate a prototype for a handler like HandleIdentify() there,
// Vrekrer_scpi_parser.h's SCPI_C/SCPI_P types aren't visible yet, and
// the build fails with "'SCPI_C' was not declared in this scope"
// even though serial_out.ino's own #include is perfectly correct on
// its own. Including it here too, in the file Arduino always
// compiles first, guarantees the types exist before that prototype
// pass needs them, regardless of tab order.
#include <Vrekrer_scpi_parser.h>

// v16: the two tasks created in setup() - for SYSTem:DIAGnostic? (serial_out.ino).
static volatile TaskHandle_t mainCaptureTaskHandle = NULL;
static volatile TaskHandle_t mainCommTaskHandle = NULL;

// v13 boot report: esp_reset_reason() as text.
static const char *bootResetReasonName()
{
    switch (esp_reset_reason())
    {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXTERNAL PIN";
        case ESP_RST_SW:        return "SOFTWARE";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INTERRUPT WDT";
        case ESP_RST_TASK_WDT:  return "TASK WDT";
        case ESP_RST_WDT:       return "OTHER WDT";
        case ESP_RST_DEEPSLEEP: return "DEEP SLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        // v16: reasons of newer ESP-IDF versions (5.x enum esp_reset_reason_t), by number so that older cores,
        // which do not define the names, still compile
        case 11:                return "USB (11)";
        case 12:                return "JTAG (12)";
        case 13:                return "EFUSE (13)";
        case 14:                return "POWER GLITCH (14)";
        case 15:                return "CPU LOCKUP (15)";
        default:
        {
            // v16: newer ESP-IDF versions have more reasons (e.g. the reset by the USB port after flashing) that
            // older cores do not define, so they are not named here - the number at least tells them apart
            // (look it up in esp_system.h, enum esp_reset_reason_t).
            static char text[20];
            snprintf(text, sizeof(text), "UNKNOWN (%d)", (int)esp_reset_reason());
            return text;
        }
    }
}

void setup()
{
#if CONFIG_BROWNOUT_DETECTOR_DISABLE
    // EXPERIMENT, see config.h: no brownout reset from here on.
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
#endif
    // Disables WiFi and Bluetooth entirely - this project never uses
    // either (SCPI goes out over a wired UART, not the network), and
    // both radios sitting next to a bench multimeter's own sensitive
    // analog measurement front-end are exactly the kind of power
    // draw and RF interference source worth eliminating outright
    // rather than just leaving idle. Done as the very first thing in
    // setup(), before anything else could touch either radio:
    //   - esp_bt_controller_mem_release() frees the RAM the ESP32
    //     Arduino core reserves for the Bluetooth controller by
    //     default (tens of KB) even when Bluetooth is never used -
    //     but it's documented as a one-time "call before the
    //     controller is ever initialized" operation, not a "stop
    //     what's currently running" one, so it has to happen before
    //     anything else, not just anywhere convenient.
    //     ESP_BT_MODE_BLE, not ESP_BT_MODE_BTDM: the ESP32-S3 (unlike
    //     the original ESP32) only has Bluetooth Low Energy, no
    //     Bluetooth Classic - BTDM ("dual mode") doesn't apply here.
    //     Not verified against a real build in this environment (no
    //     toolchain available) - worth confirming this compiles and
    //     the reported free heap actually goes up.
    //   - WiFi.mode(WIFI_OFF) ensures the WiFi radio is off - a safe
    //     no-op given this sketch never starts WiFi any other way,
    //     kept as an explicit statement of intent (and a guarantee
    //     that stays correct even if something else changes that)
    //     rather than relying on "we just never call it".
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    WiFi.mode(WIFI_OFF);

    Serial.begin(115200);
    delay(200);

    // v13: the debug queue first, so everything below can report through
    // it; the messages wait in the queue until serialOutBegin() has chosen
    // the port (SYSTem:DEBUG:PORT, persisted - default the SCPI UART, which
    // stays connected while the instrument is off) and the comm task
    // writes them. Boot report: firmware, build, reset reason (BROWNOUT
    // means the supply - e.g. the DMM's - sagged while the ESP32 started).
    debugLogBegin();
    {
        char stamp[24];
        buildInfoFormatStamp(stamp, sizeof(stamp));
        debugLogAlways("boot: KEITHLEY 197R fw %s, build %s, reset reason %s%s", BUILD_FIRMWARE, stamp,
                       bootResetReasonName(), CONFIG_BROWNOUT_DETECTOR_DISABLE ? ", brownout detector OFF" : "");
    }

    // Loads persisted SYSTem:FORMat/AUTO/THEMe/DEBUG/BEEP/COMM
    // settings from the emulated EEPROM before anything below needs
    // them - serialOutBegin() applies the stored theme once the
    // display is up and the stored port (SYSTem:COMM) once it runs,
    // and the format/auto/debug/beep settings are just read on demand
    // from settings.h elsewhere, so nothing else here needs to wait
    // on it beyond that ordering (datalogBegin() right below is one
    // of those "reads on demand" cases - see its own comment).
    settingsBegin();

    // Allocates the PSRAM ring buffer for LOG:* - requires PSRAM
    // enabled in Tools menu for this board, see datalog.h. Can run
    // any time before the first DMM frame is decoded; placed here,
    // right after settingsBegin(), since datalogUpdateReading() reads
    // the SYSTem:AUTO interval setting on every call, and this
    // function itself checks SYSTem:DEBUG to decide whether to print
    // the resulting capacity (see datalog.ino).
    datalogBegin();

    displayBegin();
    dmmBusInit(DMM_SCK_PIN, DMM_MOSI_PIN, DMM_CS_PIN);
    serialOutBegin();

    // Prints the same identification string *IDN? would answer with,
    // instead of a plain "starting up" banner that said nothing
    // useful about what firmware/version is actually running - sent
    // to serialOutActiveStream() (serial_out.h), i.e. whichever port
    // SYSTem:COMM currently has active, rather than a port hardcoded
    // independently of that setting. Needs serialOutBegin() to have
    // already run (that's what resolves which port that is) - unlike
    // serialOutPrintIdn() itself, which doesn't care.
    serialOutPrintIdn(serialOutActiveStream());

    // Front-panel buttons + REL/dB rear-panel relay emulation
    // (buttons.h/buttons.ino) - sets up pins, one-shot pulse-end
    // timers, and attaches all four GPIO interrupts, so button
    // presses are caught from this point on regardless of what either
    // core is doing. The button task created here (buttons.ino) does
    // the follow-up work the interrupt handlers themselves can't safely
    // do and turns confirmed presses into events for ui.ino.
    buttonsBegin();
    triggerIoBegin(); // v13: METERING COMPLETE output + EXTERNAL TRIGGER input - before the capture task runs

    // Acoustic signalling (beeper.h/beeper.ino) - a startup chirp
    // plays further below, once settingsBegin() (already ran above)
    // has loaded SYSTem:BEEP's persisted value.
    beeperBegin();
    alarmBegin(); // v17: limit alarm configuration + beep timer (after settingsBegin(), beeperBegin())
    markerBegin(); // v18: marker feedback timer (double beep, TAG)

    // I2C bus for the DS3231 RTC (rtc.h/rtc.ino, wrapping the
    // third-party DS3231Centisecond library), backing
    // SYSTem:TIME/SYSTem:TIME? and the reading log's timestamps
    // (datalog.ino) in serial_out.ino. rtcPoll() (core1Task() below)
    // still needs calling every ~1ms after this for the library's
    // centisecond resolution to stay accurate.
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    rtcBegin();
    rtcApplyStoredAging(); // v22: SYSTem:TIME:AGINg kept in the settings, back into the chip if it differs
    powerBegin();          // v22: light sleep while the K197 is off (power.h)

    // One short chirp, now that every other subsystem above is
    // initialized - the audible "boot finished, about to start
    // acquiring" signal. Blocking (see beeperBeep()'s own comment),
    // but only for CONFIG_BEEP_STARTUP_DURATION_MS (config.h) and
    // only here, before either per-core task exists yet, so this
    // delay does not cost either core anything once running.
    beeperPlayStartupChirp();

    // Everything above ran single-threaded, exactly as in the
    // single-core version - only now, once all shared state
    // (datalogMutex included - see datalogBegin() above) already
    // exists, do the two per-core tasks start. NULL task handles:
    // neither is ever deleted/queried elsewhere, so nothing needs to
    // keep them.
    // The display render task (core 1) and blit task (core 0) -
    // everything that could call display.h's setters has been
    // initialized above, and the first render picks up whatever
    // state those calls recorded (stored theme, Y offset).
    uiBegin();
    displayStartTasks();

    // v16: the handles are kept for SYSTem:DIAGnostic? (stack high-water marks)
    TaskHandle_t core0Handle = NULL;
    TaskHandle_t core1Handle = NULL;
    xTaskCreatePinnedToCore(core0Task, "core0_acq",
                             CONFIG_CORE0_TASK_STACK_BYTES, NULL,
                             CONFIG_CORE0_TASK_PRIORITY, &core0Handle, 0);
    xTaskCreatePinnedToCore(core1Task, "core1_comm_ui",
                             CONFIG_CORE1_TASK_STACK_BYTES, NULL,
                             CONFIG_CORE1_TASK_PRIORITY, &core1Handle, 1);
    mainCaptureTaskHandle = core0Handle;
    mainCommTaskHandle = core1Handle;

    // The Arduino core's own default loop() below now runs on
    // whichever core it happened to be pinned to (its own separate
    // task, "loopTask" - not either of the two above) doing nothing
    // useful - it already did its one job (calling this setup()
    // function). Deleting it here, once, frees that task's stack
    // rather than leaving it idling forever.
    vTaskDelete(NULL);
}

// Core 0: DMM acquisition. Must never be blocked by anything slow -
// see the file header comment above. Highest priority on this core
// (CONFIG_CORE0_TASK_PRIORITY, config.h), above the display blit
// task that shares it, so a blit in progress is preempted the moment
// a DMM frame completes.
void core0Task(void *pvParameters)
{
    for (;;)
    {
        // Waits up to DMM_CAPTURE_TIMEOUT_MS (dmm_bridge.ino) for the
        // DMM CPU to finish one CS-framed update; decodes and
        // forwards it to the display state (setReadingSegments()/
        // setIndicators()/setUnit(), called synchronously from inside
        // dmmApplyFrame() itself - they only record the values and
        // wake the render task, nothing is drawn here) and to
        // datalog.ino's ring buffer (also from inside
        // dmmApplyFrame()). dmmCaptureFrame()'s own
        // bounded wait (rather than blocking forever) is what
        // guarantees this loop yields back to the FreeRTOS
        // scheduler - and this core's idle task, which the watchdog
        // needs to see run - regularly even if the DMM bus goes
        // quiet, so no extra vTaskDelay() is needed here.
        int byteCount = dmmCaptureFrame();
        if (byteCount > 0)
        {
            dmmApplyFrame(byteCount);
        }
        dmmCheckNoData(); // v13: "Err197" + SPI slave restart after CONFIG_DMM_NO_DATA_MS without a valid frame
    }
}

// Core 1, communication task: SCPI/UART (serialOutProcess()) and keeping
// the DS3231Centisecond library's centisecond resolution accurate
// (rtcPoll(), rtc.h/rtc.ino - needs calling often, see its own comment
// for why this loop rather than core0Task()'s). The display render/UI
// task shares this core at a higher priority (display.ino) and runs the
// user interface (ui.ino), and the button task (buttons.ino) is above
// both - so a long SCPI reply that keeps THIS task busy can no longer
// delay the buttons or the picture. BTN_*_PIN/REL_OUT_PIN/DB_OUT_PIN are
// touched only from the button interrupt handlers and the button task,
// never from core0Task, so no mutex is needed here either.
void core1Task(void *pvParameters)
{
    for (;;)
    {
        // serialOutProcess() itself is non-blocking - it polls
        // Serial/Serial1 for a pending command and returns promptly
        // if there's nothing to do - so the 1-tick delay below is a
        // plain cooperative yield to the scheduler/idle task, not a
        // pacing delay: it keeps this core responsive to the next
        // incoming SCPI byte without spinning it at 100% doing
        // nothing in between. rtcPoll() is equally cheap (a
        // rate-limited I2C touch, see its own comment) and shares the
        // same loop for the same reason.
        serialOutProcess();
        rtcPoll();
        settingsPoll();  // deferred EEPROM write of front-panel settings (flash write stays out of the UI task)
        debugLogDrain(); // the one place that writes debug messages to Serial and may wait for USB
        powerPoll();     // v22: light sleep while the K197 is switched off (power.h)
        vTaskDelay(1);
    }
}

// Required by the Arduino core's main.cpp (which calls setup() once,
// then loop() forever) purely for linkage - never actually reached.
// setup() above ends by deleting its own task (vTaskDelete(NULL)),
// which never returns, so control never comes back here; core0Task()/
// core1Task() are what run forever instead, each on its own core.
void loop()
{
}
