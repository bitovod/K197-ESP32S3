// Part of K197-ESP32S3 - MIT license, (c) 2026 Pavel (see LICENSE in the repository root).
// Not an official Adafruit product: the name only says that the class is built on
// Adafruit_SPITFT and works with the Adafruit GFX API.
//
// Adafruit_NV3007.h
//
// Driver for TFT IPS displays with the NV3007 controller (NewVision),
// compatible with the Adafruit GFX / Adafruit_SPITFT library.
//
// The NV3007 controller supports resolutions up to 168(RGB)x428,
// most commonly sold on 142x428 panels (1.65"/2.79"). Communication
// via 3-/4-wire SPI.
//
// The init sequence was verified against a working ESP-IDF driver
// for a 142x428 panel (see comments in the .cpp file), it is not
// a guessed sequence. If you have a different panel (e.g. 168x428),
// you may need to tune NV3007_COL_OFFSET / NV3007_ROW_OFFSET and
// possibly the gamma/power registers according to your specific
// panel datasheet.

#ifndef _ADAFRUIT_NV3007_H_
#define _ADAFRUIT_NV3007_H_

#include <Adafruit_GFX.h>
#include <Adafruit_SPITFT.h>

// Default resolution of the most common panel (1.65"/2.79" 142x428).
// If you have a different panel, override before #include or when
// creating the instance.
#ifndef NV3007_TFTWIDTH
#define NV3007_TFTWIDTH 142
#endif
#ifndef NV3007_TFTHEIGHT
#define NV3007_TFTHEIGHT 428
#endif

// GRAM window offsets - the controller has internal GRAM wider/taller
// than the visible panel area (up to 168 source channels vs. a
// 142px-wide panel). If you see a stripe of garbled/random pixels
// along one edge, the write window does not start at the correct
// physical column/row.
//
// NV3007_ROW_OFFSET = 14 was empirically confirmed to work for a
// 142x428 panel used with setRotation(1) (landscape). Note that
// MADCTL (0x36) swaps X/Y for rotated modes, but this offset is
// applied BEFORE that swap, so it may need to be re-tuned if you
// use a different rotation value.
#ifndef NV3007_COL_OFFSET
#define NV3007_COL_OFFSET 0
#endif
#ifndef NV3007_ROW_OFFSET
#define NV3007_ROW_OFFSET 14
#endif

// Standard MIPI DCS commands (valid on the "standard page",
// i.e. after 0xFF/0x00)
#define NV3007_NOP 0x00
#define NV3007_SWRESET 0x01
#define NV3007_SLPIN 0x10
#define NV3007_SLPOUT 0x11
#define NV3007_INVOFF 0x20
#define NV3007_INVON 0x21
#define NV3007_DISPOFF 0x28
#define NV3007_DISPON 0x29
#define NV3007_CASET 0x2A
#define NV3007_RASET 0x2B
#define NV3007_RAMWR 0x2C
#define NV3007_TEON 0x35
#define NV3007_TEOFF 0x34
#define NV3007_MADCTL 0x36
#define NV3007_COLMOD 0x3A

// MADCTL bits (standard MIPI, assuming 0x36 is supported by this
// controller)
#define NV3007_MADCTL_MY 0x80
#define NV3007_MADCTL_MX 0x40
#define NV3007_MADCTL_MV 0x20
#define NV3007_MADCTL_ML 0x10
#define NV3007_MADCTL_RGB 0x00
#define NV3007_MADCTL_BGR 0x08
#define NV3007_MADCTL_MH 0x04

// Class for controlling a TFT display with the NV3007 controller
class Adafruit_NV3007 : public Adafruit_SPITFT
{
public:
    // Software SPI
    Adafruit_NV3007(int8_t cs, int8_t dc, int8_t mosi, int8_t sclk,
                     int8_t rst = -1, int8_t miso = -1);

    // Hardware SPI (default SPI bus)
    Adafruit_NV3007(int8_t cs, int8_t dc, int8_t rst = -1);

    // Hardware SPI with a specific SPI bus selected (e.g. for ESP32-S3
    // FSPI/HSPI)
    Adafruit_NV3007(SPIClass *spiClass, int8_t cs, int8_t dc, int8_t rst = -1);

    void begin(uint32_t freq = 0);
    void setRotation(uint8_t r) override;
    void setAddrWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h) override;
    void invertDisplay(bool invert);
    void enableTearingEffect(bool enable);

private:
    void initNV3007(void);
    void writeReg1(uint8_t cmd, uint8_t data);
};

#endif // _ADAFRUIT_NV3007_H_
