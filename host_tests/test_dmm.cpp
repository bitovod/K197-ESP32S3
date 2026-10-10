// Host test for dmm_bridge.ino (v13): SPI slave capture against a simulated K197 that starts sending only
// some time after the ESP32 - the power-up case where the display could stay dark - and the no-data
// watchdog ("Err197", SPI slave restart). The ESP-IDF SPI slave driver is modelled in stubs/driver/spi_slave.h.
#include <Arduino.h>
#include <stdio.h>
#include <vector>
#include <string>
#include <stdarg.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#ifndef bitRead
#define bitRead(v, b) (((v) >> (b)) & 1)
#endif
#include "config.h"
#include "display.h"
#include "keithley_shapes.h"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

// ---- stand-ins for what dmmApplyFrame()/dmmCheckNoData() feed ----
static uint8_t gSeg[6];
static int8_t gDp = -2;
static uint16_t gInd = 0xFFFF;
static uint8_t gUnit = 0xFF;
static int gGoodFrames = 0;  // frames that decoded to the reading the fake DMM sends
static int gBlankFrames = 0; // frames of valid length but all-zero content (the v12 symptom)
static int8_t gLiveDp = -2;
void setReadingSegments(const uint8_t segbits[KEITHLEY_DIGIT_COUNT], int8_t dpPos) { memcpy(gSeg, segbits, 6); gDp = dpPos; }
void setIndicators(uint16_t bits) { gInd = bits; }
void setUnit(uint8_t unit) { gUnit = unit; }
bool serialOutDebugTraceAllowed() { return false; }
void serialOutUpdateReading(const char digits[KEITHLEY_DIGIT_COUNT], int8_t dpPos, uint16_t, uint8_t unit)
{
    if (memcmp(digits, "012345", 6) == 0 && dpPos == 1 && unit == KEITHLEY_UNIT_V) gGoodFrames++;
    else if (memcmp(digits, "      ", 6) == 0) gBlankFrames++;
}
void datalogUpdateReading(const char *, int8_t, uint16_t, uint8_t) {}
void uiUpdateLiveReading(const char *, int8_t dpPos, uint16_t, uint8_t) { gLiveDp = dpPos; }
void alarmUpdateReading(const char *, int8_t, uint16_t, uint8_t) {} // v17
static std::vector<std::string> gLog;
void debugLogAlways(const char *format, ...) { char b[200]; va_list a; va_start(a, format); vsnprintf(b, sizeof(b), format, a); va_end(a); gLog.push_back(b); }
void debugLogf(const char *, ...) {}
void triggerIoPulseMeteringComplete() {}

#include "dmm_bridge.ino"

// The v12 capture loop, kept here to show the failure it had.
static int oldCaptureFrame()
{
    memset(&dmmTransaction, 0, sizeof(dmmTransaction));
    memset(dmmRxBuf, 0, sizeof(dmmRxBuf));
    dmmTransaction.length = DMM_FRAME_MAX_BYTES * 8;
    dmmTransaction.rx_buffer = dmmRxBuf;
    if (spi_slave_transmit(SPI3_HOST, &dmmTransaction, pdMS_TO_TICKS(DMM_CAPTURE_TIMEOUT_MS)) != ESP_OK) return 0;
    int n = (int)(dmmTransaction.trans_len / 8);
    return n > DMM_FRAME_MAX_BYTES ? 0 : n;
}

// A normal 17-byte frame: 8 setup bytes, then modes, digits 0 1 2 3 4 5, V, unit2. The dp bit of a digit byte
// lights the dot to the LEFT of that digit, so it sits in digit 2 for "01.2345".
static void makeFrame(uint8_t *f)
{
    memset(f, 0, 17);
    for (int i = 0; i < 8; i++) f[i] = 0x20;
    const uint8_t d[6] = {0xEB, 0xC0, 0x7A | 0x04, 0xF8, 0xD1, 0xB9};
    f[8] = 0x01; // AUTO
    memcpy(f + 9, d, 6);
    f[15] = 0x08; // V
    f[16] = 0x00;
}

static void resetAll(uint32_t dmmStartDelayMs)
{
    FakeSpiSlave &s = fakeSpi();
    if (s.initialized) spi_slave_free(SPI3_HOST);
    s = FakeSpiSlave();
    makeFrame(s.frame);
    gGoodFrames = gBlankFrames = 0;
    gLog.clear();
    dmmNoDataShown = false;
    dmmRestartCount = 0;
    dmmBusInit(DMM_SCK_PIN, DMM_MOSI_PIN, DMM_CS_PIN);
    s.dmmOn = true;
    s.nextFrameMs = fakeNow() + dmmStartDelayMs;
}

int main()
{
    // 1. the v12 loop, for the record (no check): it left up to 4 copies of the same transaction in the driver
    //    ("pending" below) and rewrote that struct and buffer while the driver still owned them. This model
    //    delivers a frame atomically, so it cannot reproduce what a DMA transfer racing with the CPU's memset()
    //    does on the real chip - it only shows the pile-up itself.
    {
        resetAll(2000);
        size_t maxPending = 0;
        for (int i = 0; i < 200; i++)
        {
            int n = oldCaptureFrame();
            if (n > 0) dmmApplyFrame(n);
            if (fakeSpi().pending.size() > maxPending) maxPending = fakeSpi().pending.size();
        }
        printf("1. v12 capture, DMM silent for 2 s after boot: up to %zu copies of the one transaction queued at once\n", maxPending);
    }
    // 2. the v13 loop: whatever the start delay, every frame after the DMM starts is captured and correct
    const uint32_t delays[5] = {0, 150, 700, 2000, 9000};
    for (int k = 0; k < 5; k++)
    {
        resetAll(delays[k]);
        for (int i = 0; i < 200; i++) { int n = dmmCaptureFrame(); if (n > 0) dmmApplyFrame(n); dmmCheckNoData(); }
        printf("2. v13 capture, DMM silent for %u ms after boot: %d good frames, %d blank, %d lost of %d sent, %d SPI restarts\n",
               delays[k], gGoodFrames, gBlankFrames, fakeSpi().lost, fakeSpi().sent, (int)dmmRestartCount);
        CHECK(gBlankFrames == 0, "never a blank frame");
        CHECK(gGoodFrames >= fakeSpi().sent - 1 && fakeSpi().lost <= 1, "every frame captured (one may fall into the instant before the first transaction is queued)");
        CHECK(!dmmNoDataShown, "no Err197 while data flows");
        CHECK(fakeSpi().pending.size() <= 1, "never more than one transaction in the driver");
    }
    // 3. watchdog: no DMM at all -> Err197 after CONFIG_DMM_NO_DATA_MS, SPI slave restarted every period; data -> normal again
    {
        resetAll(0);
        fakeSpi().dmmOn = false;
        uint32_t t0 = fakeNow();
        while (!dmmNoDataShown && fakeNow() - t0 < 60000) { int n = dmmCaptureFrame(); if (n > 0) dmmApplyFrame(n); dmmCheckNoData(); }
        uint32_t after = fakeNow() - t0;
        printf("3. no DMM: Err197 after %u ms\n", after);
        CHECK(dmmNoDataShown && after >= CONFIG_DMM_NO_DATA_MS && after <= CONFIG_DMM_NO_DATA_MS + 500, "Err197 after the no-data time");
        const uint8_t err[6] = {0x2F, 0x22, 0x22, 0x50, 0x5B, 0x51};
        CHECK(memcmp(gSeg, err, 6) == 0 && gDp == -1 && gInd == 0 && gUnit == KEITHLEY_UNIT_NONE, "display shows Err197, nothing else");
        CHECK(gLiveDp == -1, "statistics live value set to 'not a reading'");
        CHECK(dmmRestartCount == 1 && fakeSpi().inits == 2, "SPI slave restarted once");
        bool reported = false;
        for (auto &m : gLog) if (m.find("CS falling edges 0, SCK edges 0") != std::string::npos) reported = true;
        CHECK(reported, "edge report: nothing on CS/SCK while the DMM is silent");
        gLog.clear();
        fakeGpioEdges(DMM_CS_PIN, 7);    // a K197 that sends something the capture does not turn into frames
        fakeGpioEdges(DMM_SCK_PIN, 500);
        for (int i = 0; i < 120; i++) { int n = dmmCaptureFrame(); if (n > 0) dmmApplyFrame(n); dmmCheckNoData(); } // 24 s more
        bool edgesSeen = false;
        for (auto &m : gLog) if (m.find("CS falling edges 7, SCK edges 500,") != std::string::npos) edgesSeen = true;
        CHECK(edgesSeen, "edge report counts CS and SCK edges of the silent period");
        fakeGpioEdges(DMM_SCK_PIN, DMM_SCK_COUNT_CAP + 50);
        CHECK(!fakeGpioIntr(DMM_SCK_PIN).enabled && dmmSckCapped, "SCK counter switches itself off at the cap");
        printf("   restarts after %u ms of silence: %u\n", fakeNow() - t0, (unsigned)dmmRestartCount);
        CHECK(dmmRestartCount >= 3, "restarted again every period");
        FakeSpiSlave &s = fakeSpi();
        s.dmmOn = true;
        s.nextFrameMs = fakeNow() + 100;
        for (int i = 0; i < 20; i++) { int n = dmmCaptureFrame(); if (n > 0) dmmApplyFrame(n); dmmCheckNoData(); }
        CHECK(!dmmNoDataShown && gGoodFrames > 0 && gDp == 1, "data again: the reading replaces Err197");
        CHECK(!fakeGpioIntr(DMM_SCK_PIN).enabled && fakeGpioIntr(DMM_CS_PIN).enabled, "frames flow: SCK counting off, CS counting on");
        unsigned restarts = (unsigned)dmmRestartCount;
        for (int i = 0; i < 200; i++) { int n = dmmCaptureFrame(); if (n > 0) dmmApplyFrame(n); dmmCheckNoData(); }
        CHECK(dmmRestartCount == restarts, "no restarts while data flows");
    }
    // 4. frames of valid length but all-zero payload (a capture gone wrong) do not count as data: Err197 + restarts
    {
        resetAll(0);
        memset(fakeSpi().frame, 0, sizeof(fakeSpi().frame));
        uint32_t t0 = fakeNow();
        while (!dmmNoDataShown && fakeNow() - t0 < 60000) { int n = dmmCaptureFrame(); if (n > 0) dmmApplyFrame(n); dmmCheckNoData(); }
        printf("4. all-zero frames: Err197 after %u ms, %d frames received\n", fakeNow() - t0, fakeSpi().sent);
        CHECK(dmmNoDataShown && dmmRestartCount == 1, "all-zero frames: Err197 and an SPI restart");
        for (int i = 0; i < 20; i++) { int n = dmmCaptureFrame(); if (n > 0) dmmApplyFrame(n); dmmCheckNoData(); }
        const uint8_t err[6] = {0x2F, 0x22, 0x22, 0x50, 0x5B, 0x51};
        CHECK(memcmp(gSeg, err, 6) == 0, "Err197 stays (not overwritten by the blank frames)");
        makeFrame(fakeSpi().frame);
        for (int i = 0; i < 20; i++) { int n = dmmCaptureFrame(); if (n > 0) dmmApplyFrame(n); dmmCheckNoData(); }
        CHECK(!dmmNoDataShown && gDp == 1, "a real frame clears it");
    }
    printf(failures ? "FAILURES: %d\n" : "ALL OK\n", failures);
    return failures != 0;
}
