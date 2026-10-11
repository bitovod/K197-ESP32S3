// display.h
// Public interface to the display handling in display.ino. Include
// this from any .ino file that needs to draw on the screen or read
// its current state - the pixel-level details stay in display.ino
// (and framebuffer.h).
//
// v7 changed HOW the screen gets drawn, not this API:
// every setter below now only records the requested state and wakes
// the render task (core 1) - it no longer touches the TFT. The render
// task composes a complete 428x94 frame into a 6bpp indexed
// framebuffer, and a low-priority blit task on core 0 streams the
// finished frame to the panel. Consequences worth knowing:
//   - the setters are cheap and never block, so the DMM capture task
//     (core 0, dmm_bridge.ino) calls them without any cost to its
//     timing, and they are safe to call from any task;
//   - what appears on the panel lags the call by the render settle
//     time plus one blit (a few ms to a few tens of ms), it is not
//     there yet when the call returns;
//   - the getters return the REQUESTED state, which is what the
//     panel will show a moment later.

#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>
#include "keithley_shapes.h"
#include "config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h> // TaskHandle_t (v16)

// RGB565(), struct KeithleyTheme and themes[]/themeCount live in
// config.h - that is the file to edit to add, remove or recolor a
// theme.

// One bit per indicator (same order as the KeithleyIndicator enum in
// keithley_shapes.h), plus one extra bit for the minus sign. dB used
// to be here too, but it now drives setUnit() instead (KEITHLEY_UNIT_DB)
// since the original LCD shows it as a unit, blanking the normal one.
#define KEITHLEY_BIT_AUT   (1u << KEITHLEY_IND_AUT)
#define KEITHLEY_BIT_REL   (1u << KEITHLEY_IND_REL)
#define KEITHLEY_BIT_STO   (1u << KEITHLEY_IND_STO)
#define KEITHLEY_BIT_RCL   (1u << KEITHLEY_IND_RCL)
#define KEITHLEY_BIT_AC    (1u << KEITHLEY_IND_AC)
#define KEITHLEY_BIT_REM   (1u << KEITHLEY_IND_REM)
#define KEITHLEY_BIT_BAT   (1u << KEITHLEY_IND_BAT)
#define KEITHLEY_BIT_MINUS (1u << KEITHLEY_INDICATOR_COUNT)

// Timing counters for hardware bring-up (see displayGetStats()). All
// times in microseconds. "render" is composing one frame into the
// back buffer on core 1, "blit" is streaming one frame to the panel
// on core 0 (the number that matters for how long the TFT SPI bus is
// busy per update).
struct DisplayStats
{
    uint32_t renderCount;
    uint32_t renderLastUs;
    uint32_t renderMaxUs;
    uint32_t blitCount;
    uint32_t blitLastUs;
    uint32_t blitMaxUs;
    uint32_t renderWaitedForBlit; // renders that had to wait for the blit task to release the back buffer
    uint32_t renderStackMinFreeBytes; // uxTaskGetStackHighWaterMark() of the render task, lowest free stack seen
    uint32_t blitStackMinFreeBytes;   // same for the blit task
    uint32_t uiLoopMaxUs;             // longest time one pass of the render/UI loop kept the task busy (uiPoll() + render)
};

// Call once from setup(), after Serial.begin() if you want to see
// its debug output. Owns and initializes the SPI bus and the TFT,
// using the TFT_CS/TFT_DC/TFT_RST/TFT_SCK/TFT_MOSI pins #defined in
// config.h. Also renders and shows a first frame synchronously (no
// tasks needed yet), so the panel is alive from this point on, like
// before.
void displayBegin();

// Creates the render task (core 1) and the blit task (core 0). Call
// once from setup(), after displayBegin() and after everything that
// might call the setters below has been initialized (the setters are
// harmless before this - they just record state - and the render
// task's very first pass picks up whatever was recorded by then).
void displayStartTasks();

// Wakes the render/UI task so that it looks at the user interface at once
// (called by the button task when a press or release has been
// confirmed). Cheap, safe from any task (not from an interrupt handler).
void displayWakeUi();

// v22 (power.ino, light sleep): while paused the render task renders nothing
// (it still runs the user interface); un-pausing renders the current state.
// displayIsIdle(): no render and no blit running or about to start - the
// panel is not being written to and keeps its picture on its own (GRAM).
void displaySetPaused(bool paused);
bool displayIsIdle();

// Snapshot of the timing counters above.
DisplayStats displayGetStats();

// v16 (SYSTem:DIAGnostic:CLEar): zeroes the maxima above (render, blit, UI
// loop) and the count of renders that waited for the blit. The stack figures
// are FreeRTOS high-water marks and cannot be reset.
void displayResetStatsMax();

// v16 (SYSTem:DIAGnostic?): the render/UI and blit task handles (NULL before
// displayStartTasks()), for their stack high-water marks.
TaskHandle_t displayGetRenderTask();
TaskHandle_t displayGetBlitTask();

// Places the reading into the 6 digit positions, left-to-right
// exactly as given in `value` - NOT auto-padded or right-aligned, so
// leading zeros/spaces are entirely up to the caller (pass " 42" for
// a right-shifted blank-padded reading, "004200" for zero-padded,
// etc). Any character that isn't '0'-'9' (or 'L') is shown as blank.
// dpPos selects which digit (0-5) shows the decimal point, or -1 for
// none. This does NOT touch the minus sign - use setIndicators().
void setReading(String value, int8_t dpPos);

// Same as setReading(), but takes each digit's segments directly as
// raw bits instead of an ASCII character - bit0=a, bit1=g, bit2=d,
// bit3=f, bit4=b, bit5=e, bit6=c (the same scheme keithley_char_segbits
// already uses internally, so segbits[i] is exactly what a lookup in
// that table would produce for a character digit). dpPos works the
// same as in setReading(). Prefer this whenever the segment pattern
// is already known - e.g. straight from the DMM's own frame bytes,
// after remapping to this bit order (see dmm_bridge.ino) - instead
// of round-tripping it through an ASCII digit first.
void setReadingSegments(const uint8_t segbits[KEITHLEY_DIGIT_COUNT], int8_t dpPos);

// Sets all indicators + the minus sign in one call, e.g.:
//   setIndicators(KEITHLEY_BIT_AUT | KEITHLEY_BIT_AC | KEITHLEY_BIT_MINUS);
void setIndicators(uint16_t bits);
uint16_t getIndicators();  // current bitfield, for read-modify-write toggling

// unit: one of the KEITHLEY_UNIT_* constants from keithley_shapes.h
// (e.g. setUnit(KEITHLEY_UNIT_MOHM)). Pass KEITHLEY_UNIT_NONE to hide
// the unit display entirely. Out-of-range values are ignored.
void setUnit(uint8_t unit);

// v17: the limit alarm's triangle above the minus sign (FB_ALARM_* =
// ALARM_STATE_*, see alarm.h). Called by alarm.ino whenever the state
// changes; cheap, any task.
void setAlarmIndicator(uint8_t state);

// v18: "TAG" in place of REM lit (a marker was just set, marker.ino) or ghost.
void setTagIndicator(bool lit);

// v23: "SD!" in place of BAT lit (an SD card problem, blinking - sdlog.ino) or
// ghost. Wakes the render task only while the LCD replica is shown.
void setSdIndicator(bool lit);

// ---- v24: DATA HOLD (hold.h) ----
// One complete frame of the LCD replica (what setReadingSegments(),
// setIndicators() and setUnit() recorded) and when it was completed.
// In this header for the Arduino prototype rule (displayHeldState()).
struct DisplayLcdState
{
    uint8_t segBits[KEITHLEY_DIGIT_COUNT];
    int8_t dpPos;
    uint16_t indicatorBits;
    uint8_t unit;
    uint32_t ms; // millis() when the frame was completed
};
// Freeze the LCD replica on the frame that was SHOWN at millis() atMs (one of
// the last 8 frames; older: the oldest of them) / on the current frame;
// release = live again. The live state keeps being recorded meanwhile, and
// the other screens are not affected. Any task.
void displayHoldAt(uint32_t atMs);
void displayHoldCurrent();
void displayHoldRelease();
bool displayHoldIsActive();
// The held frame (false if nothing is held) - host tests, SCPI.
bool displayHeldState(DisplayLcdState *out);
// "HOLD" / "AUTO" in place of STO / RCL lit (FB_HOLD_* bits, framebuffer.h)
// or ghost. Wakes the render task only while the LCD replica is shown.
void setHoldIndicator(uint8_t bits);

const char *currentThemeName();

// Switches the active color theme (see themes[] in config.h). `theme`
// is clamped into [0, themeCount) if out of range, rather than
// treated as an error - callers driving this from an SCPI parameter
// (serial_out.ino) can pass whatever the person typed without
// validating it first.
void setTheme(uint8_t theme);

// Vertical offset (pixels, from the top of the panel) where the
// 428x94 content area starts - see SYSTem:YOFFset (serial_out.ino)
// and CONFIG_DEFAULT_DISPLAY_Y_OFFSET (config.h). Needed because the
// exact correct offset depends on how this particular panel sits
// behind the meter's plastic bezel once mounted in real hardware,
// not a fixed property of the panel or the artwork - the visible
// content's HEIGHT does not change, only where it starts. The strips
// above and below the content area are always painted black.
// Clamped into [0, getDisplayMaxYOffset()] if out of range, same
// "callers can pass unvalidated input" reasoning as setTheme() above.
void setDisplayYOffset(int16_t offsetY);
int16_t getDisplayYOffset();
int16_t getDisplayMaxYOffset();

#endif // DISPLAY_H
