#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <vector>
#include <mutex>
#include <atomic>
// Fake panel: 428x142 image, window-based sequential writes, records finished "blits".
struct FakeTft
{
    static const int W = 428, H = 142;
    uint16_t img[H][W];
    int wx, wy, ww, wh, pos;
    bool inWrite;
    std::atomic<int> concurrentUsers{0};
    int overlapErrors = 0;
    int blitsDone = 0;
    int slowUs = 0;
    void (*onFrame)(FakeTft &) = nullptr;
};
class Adafruit_NV3007
{
public:
    Adafruit_NV3007(SPIClass *, int, int, int) { memset(&t.img, 0, sizeof(t.img)); }
    void begin() {}
    void setRotation(int) {}
    int width() { return FakeTft::W; }
    int height() { return FakeTft::H; }
    void startWrite()
    {
        if (t.concurrentUsers.fetch_add(1) != 0) t.overlapErrors++;
        t.inWrite = true;
    }
    void endWrite()
    {
        t.inWrite = false;
        t.concurrentUsers.fetch_sub(1);
        t.blitsDone++;
        if (t.onFrame) t.onFrame(t);
    }
    void setAddrWindow(int x, int y, int w, int h) { t.wx = x; t.wy = y; t.ww = w; t.wh = h; t.pos = 0; }
    void writePixels(uint16_t *c, uint32_t len, bool = true, bool = false)
    {
        for (uint32_t i = 0; i < len; i++)
        {
            int p = t.pos++;
            int x = t.wx + p % t.ww, y = t.wy + p / t.ww;
            if (y < FakeTft::H && x < FakeTft::W) t.img[y][x] = c[i];
        }
        if (t.slowUs) { auto e = std::chrono::steady_clock::now() + std::chrono::microseconds(t.slowUs); while (std::chrono::steady_clock::now() < e) {} }
    }
    void drawRGBBitmap(int x, int y, uint16_t *c, int w, int h)
    {
        for (int r = 0; r < h; r++) for (int i = 0; i < w; i++) t.img[y + r][x + i] = c[r * w + i];
    }
    void fillRect(int x, int y, int w, int h, uint16_t col)
    {
        for (int r = 0; r < h; r++) for (int i = 0; i < w; i++) t.img[y + r][x + i] = col;
    }
    FakeTft t;
};
