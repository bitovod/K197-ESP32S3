// display.ino
// All display handling for the Keithley 197 replacement screen. As of
// v7 nothing draws straight onto the TFT any more:
//
//   core 0, capture task (dmm_bridge.ino)
//        |  setReadingSegments()/setIndicators()/setUnit()   (state only,
//        v   microseconds, never blocks)
//   shared display state  --notify-->  core 1, RENDER task
//                                          |  composes a full 428x94 frame
//                                          |  into the back buffer (6bpp,
//                                          |  framebuffer.h) + its palette
//                                          v
//                                      front/back swap  --notify-->
//                                          core 0, BLIT task (low priority)
//                                              streams the front buffer to
//                                              the panel
//
// Why it is built this way:
//   - The DMM capture task on core 0 must stay predictable. The old
//     code drew digits and indicators pixel by pixel from inside that
//     very task (and from the SCPI task on core 1 for theme/offset
//     changes, which was a latent race, see docs/DEVELOPMENT.md). Now
//     the capture task only stores a few bytes of state.
//   - All drawing math runs on core 1, in a render task whose priority
//     is higher than the SCPI task's, so a long SCPI reply (a
//     LOG:DATA? dump) cannot freeze the picture - the render task
//     simply preempts it for the couple of milliseconds a frame takes.
//   - Only ONE task ever touches the TFT: the blit task. Its priority
//     is LOWER than the capture task's, so when a DMM frame arrives the
//     capture task preempts a blit in progress immediately (the TFT
//     transfer just pauses between two SPI bursts). Capture is never
//     delayed by the display.
//   - (v8) The render task is also the user interface's task: it
//     polls the buttons and lets ui.ino switch between the LCD and the
//     text screens; a text screen is just another way to compose the
//     same kind of frame.
//   - Two complete frames (slots) exist. The render task always writes
//     the one the blit task is NOT reading, then swaps. A slot carries
//     its own palette and Y offset, so a theme or offset change can
//     never be shown half-applied.
//
// IMPORTANT: this file relies on TFT_CS/TFT_DC/TFT_RST/TFT_SCK/
// TFT_MOSI being #defined already by the time it is compiled - they
// come from config.h, pulled in transitively through display.h.
//
// NOT VERIFIED on real hardware: the pure framebuffer math
// (framebuffer.h) and the frame-exchange logic were tested on a PC,
// but writePixels()/setAddrWindow() against the real Adafruit_NV3007
// library, the task timing and the stack sizes have not been run on
// the ESP32-S3 yet - see CONFIG_DISPLAY_BLIT_USE_DRAWRGBBITMAP in
// config.h for the fallback if writePixels() misbehaves.

#include <Adafruit_GFX.h>
#include <Adafruit_NV3007.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "keithley_shapes.h"
#include "framebuffer.h"
#include "display.h"
#include "ui.h"

// framebuffer.h cannot see display.h (it must stay free of it), so
// the two definitions of the minus bit are checked against each
// other here.
static_assert(FB_BIT_MINUS == KEITHLEY_BIT_MINUS, "minus bit mismatch between framebuffer.h and display.h");

Adafruit_NV3007 tft(&SPI, TFT_CS, TFT_DC, TFT_RST);

// ---- panel geometry (filled in by displayBegin()) ----

// Defaults are the NV3007 panel in rotation 1 (landscape); the real
// values are read back from the library once it is up.
static int16_t dispPanelWidth = KEITHLEY_SCREEN_WIDTH;
static int16_t dispPanelHeight = 142;

// ---- requested state: written by any task, read by the render task ----
//
// Guarded by dispStateMux (a spinlock critical section). Every
// section is a handful of plain stores or a ~20 byte copy, so
// holding it is cheap and bounded - nothing in here can stall the
// capture task for a measurable time.

static portMUX_TYPE dispStateMux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t dispSegBits[KEITHLEY_DIGIT_COUNT] = {0, 0, 0, 0, 0, 0};
static int8_t dispDpPos = -1;
static uint16_t dispIndicatorBits = 0;
static uint8_t dispUnit = KEITHLEY_UNIT_NONE;
static uint8_t dispAlarm = FB_ALARM_NONE; // v17
static bool dispTagLit = false;           // v18
static bool dispSdLit = false;            // v23
static uint8_t dispHoldInd = 0;           // v24: FB_HOLD_* bits ("HOLD" / "AUTO" lit)
// v24 DATA HOLD: the LCD replica shows dispHeld instead of the live reading while dispHoldActive. The
// last DISP_HIST_COUNT complete frames (stored by setUnit(), the last setter of a frame) with their millis()
// let displayHoldAt() freeze what was SHOWN at a given moment (the first edge of the STO press).
#define DISP_HIST_COUNT 8
static DisplayLcdState dispHist[DISP_HIST_COUNT];
static uint8_t dispHistNext = 0;
static uint8_t dispHistCount = 0;
static bool dispHoldActive = false;
static DisplayLcdState dispHeld;
static uint8_t dispTheme = CONFIG_DEFAULT_THEME;
static int16_t dispYOffset = CONFIG_DEFAULT_DISPLAY_Y_OFFSET;

// ---- frame exchange between render task and blit task ----

// Two complete frames, ~30 kB each, in internal SRAM (static, so
// the linker reports the cost up front and there is no allocation to
// fail at run time). See framebuffer.h for the pixel format.
static FbSlot dispSlots[2];

// dispFrontIdx: the slot holding the newest finished frame - written
// only by the render task (and displayBegin() before any task
// exists), read by the blit task. dispReadingIdx: the slot the blit
// task is streaming to the panel right now, or -1. Both live under
// dispFrameMux.
//
// The protocol: the blit task, in one critical section, reads
// dispFrontIdx and publishes it as dispReadingIdx. The render task
// only ever writes into slot (1 - dispFrontIdx). After a swap that
// slot is the one the blit task may STILL be reading, so before
// rendering into it the render task waits until dispReadingIdx no
// longer names it. A claim can only happen on the front slot, so once
// a slot has stopped being the front slot nobody can newly claim it -
// checking dispReadingIdx once it is clear is therefore enough.
static portMUX_TYPE dispFrameMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t dispFrontIdx = 0;
static volatile int8_t dispReadingIdx = -1;

// Panel Y offset the strips above/below the content area were last
// painted for; -1 = never painted. Touched by the blit task only (and
// displayBegin() before any task exists).
static int16_t dispLastBlitY = -1;

// Scratch for converting packed rows to RGB565 before sending. Blit
// task only. CONFIG_DISPLAY_BLIT_CHUNK_ROWS rows per SPI call.
static uint16_t dispLineBuf[FB_WIDTH * CONFIG_DISPLAY_BLIT_CHUNK_ROWS];

static volatile TaskHandle_t dispRenderTaskHandle = NULL;
static volatile TaskHandle_t dispBlitTaskHandle = NULL;

static DisplayStats dispStats = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

// ---- helpers ----

// Notification bits for the render/UI task. DISP_NOTIFY_RENDER: the
// requested display state changed (a frame must be rendered, after the
// short settle pause). DISP_NOTIFY_UI: the user interface has something
// to look at (a button event) - no frame is rendered unless uiPoll() says
// the picture changed.
#define DISP_NOTIFY_RENDER 0x01
#define DISP_NOTIFY_UI     0x02

static void displayNotify(uint32_t bits)
{
    TaskHandle_t h = dispRenderTaskHandle;
    if (h != NULL)
    {
        xTaskNotify(h, bits, eSetBits);
    }
}

// Wakes the render task. Safe from any task, and a no-op before
// displayStartTasks() has created it - the render task's first pass
// picks up whatever state was recorded by then.
static void displayWakeRender()
{
    displayNotify(DISP_NOTIFY_RENDER);
}

// Wakes the render/UI task for the user interface only (button events).
// Called by the button task (buttons.ino).
void displayWakeUi()
{
    displayNotify(DISP_NOTIFY_UI);
}

// v22 (power.ino, light sleep): rendering paused, a render or a blit under way / about to start.
static volatile bool dispPaused = false;
static volatile bool dispRenderBusy = false;
static volatile bool dispBlitPending = false;

static void displayWakeBlit()
{
    TaskHandle_t h = dispBlitTaskHandle;
    if (h != NULL)
    {
        dispBlitPending = true; // cleared when the blit task claims the frame
        xTaskNotifyGive(h);
    }
}

// True while the blit task is streaming slot `idx` to the panel.
static bool displaySlotInUse(uint8_t idx)
{
    bool busy;
    portENTER_CRITICAL(&dispFrameMux);
    busy = (dispReadingIdx == (int8_t)idx);
    portEXIT_CRITICAL(&dispFrameMux);
    return busy;
}

// Converts a millisecond setting into FreeRTOS ticks, never 0 (a
// 0-tick delay would not yield at all).
static TickType_t displayMsToTicks(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return (ticks == 0) ? 1 : ticks;
}

// ---- render (core 1) ----

// Takes a consistent snapshot of the requested state and composes it
// into the back slot, then publishes that slot as the new front and
// wakes the blit task. waitForBlitter is false only for the call from
// displayBegin(), where no blit task exists yet and waiting could
// never end.
static void displayRenderOnce(bool waitForBlitter)
{
    uint32_t startUs = micros();

    uint8_t seg[KEITHLEY_DIGIT_COUNT];
    int8_t dp;
    uint16_t ind;
    uint8_t unit;
    uint8_t alarm;
    bool tagLit;
    bool sdLit;
    uint8_t holdInd;
    uint8_t theme;
    int16_t yOffset;

    portENTER_CRITICAL(&dispStateMux);
    if (dispHoldActive)
    {
        // v24: DATA HOLD - the held reading instead of the live one
        memcpy(seg, dispHeld.segBits, sizeof(seg));
        dp = dispHeld.dpPos;
        ind = dispHeld.indicatorBits;
        unit = dispHeld.unit;
    }
    else
    {
        memcpy(seg, dispSegBits, sizeof(seg));
        dp = dispDpPos;
        ind = dispIndicatorBits;
        unit = dispUnit;
    }
    holdInd = dispHoldInd;
    alarm = dispAlarm;
    tagLit = dispTagLit;
    sdLit = dispSdLit;
    theme = dispTheme;
    yOffset = dispYOffset;
    portEXIT_CRITICAL(&dispStateMux);

    uint8_t back = (uint8_t)(1 - dispFrontIdx);

    if (waitForBlitter)
    {
        bool waited = false;
        while (displaySlotInUse(back))
        {
            waited = true;
            vTaskDelay(1);
        }
        if (waited)
        {
            dispStats.renderWaitedForBlit++;
        }
    }

    FbSlot &slot = dispSlots[back];
    uint8_t screen = uiGetScreen();
    bool uiScreenShown = (screen != UI_SCREEN_LCD);
    // The emulated LCD keeps the theme exactly as configured; the user interface screens
    // (statistics, help) always use a black background (fbStatsTheme()).
    fbBuildPalette(uiScreenShown ? fbStatsTheme(themes[theme]) : themes[theme], slot.palette);

    // Which screen: the emulated LCD, or one of the UI screens (ui.ino).
    // The UI screens are plain text on the same palette, so a theme
    // change recolors them exactly like the LCD.
    if (screen == UI_SCREEN_QUICKSTATS)
    {
        FbStatsScene statsScene;
        uiFillStatsScene(&statsScene);
        fbComposeStats(slot.px, statsScene);
    }
    else if (screen == UI_SCREEN_HELP)
    {
        FbHelpScene helpScene;
        uiFillHelpScene(&helpScene);
        fbComposeHelp(slot.px, helpScene);
    }
    else if (screen == UI_SCREEN_GRAPH)
    {
        // v14: the scene (~3 kB) is read in place - it belongs to this task (ui/graph run here)
        fbComposeGraph(slot.px, uiGetGraphScene());
    }
    else if (screen == UI_SCREEN_GRAPH_SETUP)
    {
        // v14: the graph settings use the help screen's 6-line layout; v15: triangles (fbComposeSetup())
        FbSetupScene setupScene;
        uiFillGraphSetupScene(&setupScene);
        fbComposeSetup(slot.px, setupScene);
    }
    else if (screen == UI_SCREEN_DMM_SETUP)
    {
        // v15: language, theme, sound - same layout as the graph settings
        FbSetupScene setupScene;
        uiFillDmmSetupScene(&setupScene);
        fbComposeSetup(slot.px, setupScene);
    }
    else if (screen == UI_SCREEN_ALARM_SETUP)
    {
        // v17: mode, limits, signal of the limit alarm - same layout as the other settings
        FbSetupScene setupScene;
        uiFillAlarmSetupScene(&setupScene);
        fbComposeSetup(slot.px, setupScene);
    }
    else
    {
        FbLcdScene scene;
        memcpy(scene.segBits, seg, sizeof(seg));
        scene.dpPos = dp;
        scene.indicatorBits = ind;
        scene.unit = unit;
        scene.alarm = alarm;
        scene.tagLit = tagLit;
        scene.sdLit = sdLit;
        scene.hold = holdInd;
        fbComposeLcd(slot.px, scene);
    }
    slot.yOffset = yOffset;

    portENTER_CRITICAL(&dispFrameMux);
    dispFrontIdx = back;
    portEXIT_CRITICAL(&dispFrameMux);
    displayWakeBlit();

    uint32_t elapsedUs = micros() - startUs;
    dispStats.renderCount++;
    dispStats.renderLastUs = elapsedUs;
    if (elapsedUs > dispStats.renderMaxUs)
    {
        dispStats.renderMaxUs = elapsedUs;
    }
}

// Render/UI task body. Besides rendering it runs the user interface:
// uiPoll() (screen switching, text refresh, the statistics job) after
// every wake-up and, when nothing wakes it, every CONFIG_UI_POLL_MS - or
// every tick while a statistics job is running, so the job gets its next
// slice at once. A render happens when either a display setter woke the
// task, or the UI says something changed.
//
// Button presses are NOT detected here: the button task (buttons.ino)
// confirms them, records the event and wakes this task with
// displayWakeUi() - so how quickly a press is noticed never depends on
// what this loop is busy with (v8 polled the buttons from this loop).
//
// The pause after a display wake-up exists because the DMM capture task
// updates reading, indicators and unit with three separate calls
// microseconds apart; without it a render could land in between them and
// show a frame with the new digits but the old indicators. Notifications
// that arrive during the pause are merged (the snapshot taken right after
// covers them); ones that arrive later trigger the next pass.
static void displayRenderTaskEntry(void *pvParameters)
{
    // First pass without waiting for a notification: setTheme()/
    // setDisplayYOffset() and the first reading may all have been
    // recorded before this task existed.
    displayRenderOnce(true);

    for (;;)
    {
        uint32_t bits = 0;
        TickType_t timeout = uiBusy() ? (TickType_t)1 : displayMsToTicks(CONFIG_UI_POLL_MS);
        xTaskNotifyWait(0, 0xFFFFFFFFUL, &bits, timeout);

        uint32_t workStartUs = micros();
        if (bits & DISP_NOTIFY_RENDER)
        {
            vTaskDelay(displayMsToTicks(CONFIG_DISPLAY_RENDER_SETTLE_MS));
            uint32_t more = 0;
            xTaskNotifyWait(0, 0xFFFFFFFFUL, &more, 0);
            bits |= more;
        }

        bool uiChanged = uiPoll();
        if ((bits & DISP_NOTIFY_RENDER) || uiChanged)
        {
            // v22: busy BEFORE the pause check, so displayIsIdle() can never miss a render that is starting
            dispRenderBusy = true;
            if (!dispPaused)
            {
                displayRenderOnce(true);
            }
            dispRenderBusy = false;
        }

        // How long one pass of this loop kept the task busy - the longest one is what could delay the UI.
        uint32_t busyUs = micros() - workStartUs;
        if (busyUs > dispStats.uiLoopMaxUs)
        {
            dispStats.uiLoopMaxUs = busyUs;
        }
        dispStats.renderStackMinFreeBytes = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    }
}

// ---- blit (core 0) ----

// Paints the two strips of the panel outside the 428x94 content area
// black. The panel is taller than what the meter's plastic bezel
// exposes, and whatever the content area leaves uncovered must be
// black regardless of theme - a light theme's background would
// otherwise show under the bezel edge. Only needed when the offset
// changes; the content blit itself covers everything else.
static void displayPaintStrips(int16_t y0)
{
    const uint16_t black = RGB565(0, 0, 0);
    if (y0 > 0)
    {
        tft.fillRect(0, 0, dispPanelWidth, y0, black);
    }
    int16_t below = y0 + FB_HEIGHT;
    if (below < dispPanelHeight)
    {
        tft.fillRect(0, below, dispPanelWidth, dispPanelHeight - below, black);
    }
}

// Streams the newest finished frame to the panel. Idempotent: a
// second call without a new frame in between just repaints the same
// picture, so several wake-ups arriving close together can be served
// by one call.
static void displayBlitFront()
{
    uint32_t startUs = micros();

    uint8_t idx;
    portENTER_CRITICAL(&dispFrameMux);
    idx = dispFrontIdx;
    dispReadingIdx = (int8_t)idx;
    dispBlitPending = false;
    portEXIT_CRITICAL(&dispFrameMux);

    const FbSlot &slot = dispSlots[idx];
    int16_t y0 = slot.yOffset;

    if (y0 != dispLastBlitY)
    {
        displayPaintStrips(y0);
        dispLastBlitY = y0;
    }

#if CONFIG_DISPLAY_BLIT_USE_DRAWRGBBITMAP
    // Fallback path, see config.h: one drawRGBBitmap() per chunk.
    // Slower if the library draws pixel by pixel, but uses nothing
    // beyond what the previous versions of this project already
    // relied on.
    for (uint16_t row = 0; row < FB_HEIGHT; row += CONFIG_DISPLAY_BLIT_CHUNK_ROWS)
    {
        uint16_t rows = FB_HEIGHT - row;
        if (rows > CONFIG_DISPLAY_BLIT_CHUNK_ROWS)
        {
            rows = CONFIG_DISPLAY_BLIT_CHUNK_ROWS;
        }
        fbUnpackRows(slot.px, row, rows, slot.palette, dispLineBuf);
        tft.drawRGBBitmap(0, y0 + row, dispLineBuf, FB_WIDTH, rows);
    }
#else
    // One address window for the whole content area, then the pixels
    // stream into it row chunk by row chunk - the panel's write
    // pointer just keeps advancing.
    tft.startWrite();
    tft.setAddrWindow(0, y0, FB_WIDTH, FB_HEIGHT);
    for (uint16_t row = 0; row < FB_HEIGHT; row += CONFIG_DISPLAY_BLIT_CHUNK_ROWS)
    {
        uint16_t rows = FB_HEIGHT - row;
        if (rows > CONFIG_DISPLAY_BLIT_CHUNK_ROWS)
        {
            rows = CONFIG_DISPLAY_BLIT_CHUNK_ROWS;
        }
        fbUnpackRows(slot.px, row, rows, slot.palette, dispLineBuf);
        tft.writePixels(dispLineBuf, (uint32_t)FB_WIDTH * rows);
    }
    tft.endWrite();
#endif

    portENTER_CRITICAL(&dispFrameMux);
    dispReadingIdx = -1;
    portEXIT_CRITICAL(&dispFrameMux);

    uint32_t elapsedUs = micros() - startUs;
    dispStats.blitCount++;
    dispStats.blitLastUs = elapsedUs;
    if (elapsedUs > dispStats.blitMaxUs)
    {
        dispStats.blitMaxUs = elapsedUs;
    }
}

static void displayBlitTaskEntry(void *pvParameters)
{
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        displayBlitFront();
        dispStats.blitStackMinFreeBytes = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    }
}

// ---- public API (declared in display.h) ----

void displaySetPaused(bool paused)
{
    dispPaused = paused;
    if (!paused)
    {
        displayNotify(DISP_NOTIFY_RENDER); // whatever changed meanwhile
    }
}

bool displayIsIdle()
{
    bool reading;
    portENTER_CRITICAL(&dispFrameMux);
    reading = dispReadingIdx >= 0;
    portEXIT_CRITICAL(&dispFrameMux);
    return !dispRenderBusy && !dispBlitPending && !reading;
}

void displayBegin()
{
    SPI.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS);
    tft.begin();
    tft.setRotation(1);
    dispPanelWidth = tft.width();
    dispPanelHeight = tft.height();

    // Keep the stored/default offset valid for THIS panel (a stale
    // value from a different panel size could otherwise leave a
    // negative bottom strip).
    int16_t maxOffset = dispPanelHeight - FB_HEIGHT;
    if (dispYOffset > maxOffset)
    {
        dispYOffset = maxOffset;
    }

    // No tasks exist yet: render and show the first frame right here.
    displayRenderOnce(false);
    displayBlitFront();
}

void displayStartTasks()
{
    // The blit task first, so its handle is valid before the render
    // task can publish its first frame.
    TaskHandle_t blitHandle = NULL;
    xTaskCreatePinnedToCore(displayBlitTaskEntry, "disp_blit",
                             CONFIG_DISPLAY_BLIT_TASK_STACK_BYTES, NULL,
                             CONFIG_DISPLAY_BLIT_TASK_PRIORITY, &blitHandle, 0);
    dispBlitTaskHandle = blitHandle;

    TaskHandle_t renderHandle = NULL;
    xTaskCreatePinnedToCore(displayRenderTaskEntry, "disp_render",
                             CONFIG_DISPLAY_RENDER_TASK_STACK_BYTES, NULL,
                             CONFIG_DISPLAY_RENDER_TASK_PRIORITY, &renderHandle, 1);
    dispRenderTaskHandle = renderHandle;
}

DisplayStats displayGetStats()
{
    return dispStats;
}

void displayResetStatsMax()
{
    dispStats.renderMaxUs = 0;
    dispStats.blitMaxUs = 0;
    dispStats.uiLoopMaxUs = 0;
    dispStats.renderWaitedForBlit = 0;
}

TaskHandle_t displayGetRenderTask()
{
    return dispRenderTaskHandle;
}

TaskHandle_t displayGetBlitTask()
{
    return dispBlitTaskHandle;
}

void setReadingSegments(const uint8_t segbits[KEITHLEY_DIGIT_COUNT], int8_t dpPos)
{
    portENTER_CRITICAL(&dispStateMux);
    for (uint8_t i = 0; i < KEITHLEY_DIGIT_COUNT; i++)
    {
        dispSegBits[i] = segbits[i];
    }
    dispDpPos = dpPos;
    portEXIT_CRITICAL(&dispStateMux);
    displayWakeRender();
}

// Character path: looks up the segment pattern for each ASCII digit
// (or blank/'L') and hands off to setReadingSegments().
void setReading(String value, int8_t dpPos)
{
    uint8_t seg[KEITHLEY_DIGIT_COUNT];
    for (uint8_t i = 0; i < KEITHLEY_DIGIT_COUNT; i++)
    {
        char c = ' ';
        if (i < value.length())
        {
            c = value.charAt(i);
        }
        uint8_t charIdx;
        if (c >= '0' && c <= '9')
        {
            charIdx = c - '0';
        }
        else if (c == 'L')
        {
            charIdx = 11; // OL overload indication, see keithley_shapes.h
        }
        else
        {
            charIdx = 10; // blank/space
        }
        seg[i] = pgm_read_byte(&keithley_char_segbits[charIdx]);
    }
    setReadingSegments(seg, dpPos);
}

void setIndicators(uint16_t bits)
{
    portENTER_CRITICAL(&dispStateMux);
    dispIndicatorBits = bits;
    portEXIT_CRITICAL(&dispStateMux);
    displayWakeRender();
}

uint16_t getIndicators()
{
    return dispIndicatorBits;
}

void setUnit(uint8_t unit)
{
    if (unit >= KEITHLEY_UNIT_COUNT)
    {
        return; // out of range, ignore
    }
    portENTER_CRITICAL(&dispStateMux);
    dispUnit = unit;
    // v24: the frame is complete (setReadingSegments(), setIndicators(), setUnit()) - into the history
    DisplayLcdState &h = dispHist[dispHistNext];
    memcpy(h.segBits, dispSegBits, sizeof(h.segBits));
    h.dpPos = dispDpPos;
    h.indicatorBits = dispIndicatorBits;
    h.unit = dispUnit;
    h.ms = (uint32_t)millis();
    dispHistNext = (uint8_t)((dispHistNext + 1) % DISP_HIST_COUNT);
    if (dispHistCount < DISP_HIST_COUNT)
    {
        dispHistCount++;
    }
    portEXIT_CRITICAL(&dispStateMux);
    displayWakeRender();
}

// ---- v24: DATA HOLD ----

// Caller holds dispStateMux: the current live state into dispHeld.
static void displayHoldTakeCurrentLocked()
{
    memcpy(dispHeld.segBits, dispSegBits, sizeof(dispHeld.segBits));
    dispHeld.dpPos = dispDpPos;
    dispHeld.indicatorBits = dispIndicatorBits;
    dispHeld.unit = dispUnit;
    dispHeld.ms = (uint32_t)millis();
}

void displayHoldAt(uint32_t atMs)
{
    portENTER_CRITICAL(&dispStateMux);
    bool found = false;
    for (uint8_t k = 0; k < dispHistCount; k++)
    {
        // newest first: the last frame completed at or before atMs
        const DisplayLcdState &h = dispHist[(dispHistNext + DISP_HIST_COUNT - 1 - k) % DISP_HIST_COUNT];
        if ((int32_t)(atMs - h.ms) >= 0)
        {
            dispHeld = h;
            found = true;
            break;
        }
    }
    if (!found && dispHistCount > 0)
    {
        // older than the whole history: the oldest frame known
        dispHeld = dispHist[(dispHistNext + DISP_HIST_COUNT - dispHistCount) % DISP_HIST_COUNT];
        found = true;
    }
    if (!found)
    {
        displayHoldTakeCurrentLocked();
    }
    dispHoldActive = true;
    portEXIT_CRITICAL(&dispStateMux);
    displayWakeRender();
}

void displayHoldCurrent()
{
    portENTER_CRITICAL(&dispStateMux);
    displayHoldTakeCurrentLocked();
    dispHoldActive = true;
    portEXIT_CRITICAL(&dispStateMux);
    displayWakeRender();
}

void displayHoldRelease()
{
    portENTER_CRITICAL(&dispStateMux);
    bool changed = dispHoldActive;
    dispHoldActive = false;
    portEXIT_CRITICAL(&dispStateMux);
    if (changed)
    {
        displayWakeRender();
    }
}

bool displayHoldIsActive()
{
    return dispHoldActive;
}

bool displayHeldState(DisplayLcdState *out)
{
    portENTER_CRITICAL(&dispStateMux);
    bool active = dispHoldActive;
    if (active)
    {
        *out = dispHeld;
    }
    portEXIT_CRITICAL(&dispStateMux);
    return active;
}

void setHoldIndicator(uint8_t bits)
{
    portENTER_CRITICAL(&dispStateMux);
    bool changed = (dispHoldInd != bits);
    dispHoldInd = bits;
    portEXIT_CRITICAL(&dispStateMux);
    // like SD!: only the LCD replica shows it - blinking must not re-render the other screens
    if (changed && uiGetScreen() == UI_SCREEN_LCD)
    {
        displayWakeRender();
    }
}

void setAlarmIndicator(uint8_t state)
{
    portENTER_CRITICAL(&dispStateMux);
    bool changed = (dispAlarm != state);
    dispAlarm = state;
    portEXIT_CRITICAL(&dispStateMux);
    if (changed)
    {
        displayWakeRender();
    }
}

void setTagIndicator(bool lit)
{
    portENTER_CRITICAL(&dispStateMux);
    bool changed = (dispTagLit != lit);
    dispTagLit = lit;
    portEXIT_CRITICAL(&dispStateMux);
    if (changed)
    {
        displayWakeRender();
    }
}

void setSdIndicator(bool lit)
{
    portENTER_CRITICAL(&dispStateMux);
    bool changed = (dispSdLit != lit);
    dispSdLit = lit;
    portEXIT_CRITICAL(&dispStateMux);
    // Only the LCD replica shows it: blinking must not re-render the other screens several times a second.
    // The next render of the LCD takes the current value anyway.
    if (changed && uiGetScreen() == UI_SCREEN_LCD)
    {
        displayWakeRender();
    }
}

const char *currentThemeName()
{
    return themes[dispTheme].name;
}

void setTheme(uint8_t theme)
{
    if (theme >= themeCount)
    {
        theme = themeCount - 1;
    }
    portENTER_CRITICAL(&dispStateMux);
    dispTheme = theme;
    portEXIT_CRITICAL(&dispStateMux);
    displayWakeRender();
}

// Highest valid setDisplayYOffset() value - beyond this, the bottom
// strip's height (panel height - offset - content height) would go
// negative. Exposed as a function rather than a #define since it
// depends on the real panel height, only known once displayBegin()
// has run - serial_out.ino's SYSTem:YOFFset handler calls this to
// validate a person's input rather than duplicating the panel
// geometry itself.
int16_t getDisplayMaxYOffset()
{
    return dispPanelHeight - FB_HEIGHT;
}

// offsetY is clamped into [0, getDisplayMaxYOffset()] here too (not
// just by the SCPI handler that's the normal way in) - defensive
// against a stale/out-of-range value ever loaded from EEPROM (e.g.
// after a panel swap to a different physical size), same reasoning as
// setTheme()'s own clamp above.
void setDisplayYOffset(int16_t offsetY)
{
    int16_t maxOffset = getDisplayMaxYOffset();
    if (offsetY < 0)
    {
        offsetY = 0;
    }
    else if (offsetY > maxOffset)
    {
        offsetY = maxOffset;
    }
    portENTER_CRITICAL(&dispStateMux);
    dispYOffset = offsetY;
    portEXIT_CRITICAL(&dispStateMux);
    displayWakeRender();
}

int16_t getDisplayYOffset()
{
    return dispYOffset;
}
