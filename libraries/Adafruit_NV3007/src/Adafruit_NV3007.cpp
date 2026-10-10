// Part of K197-ESP32S3 - MIT license, (c) 2026 Pavel (see LICENSE in the repository root).
// Not an official Adafruit product: the name only says that the class is built on
// Adafruit_SPITFT and works with the Adafruit GFX API.
//
// Adafruit_NV3007.cpp
//
// Driver for TFT IPS displays with the NV3007 controller, built on
// top of Adafruit_SPITFT (same base as e.g. Adafruit_ST7789), so it
// is fully compatible with the Adafruit GFX API.
//
// The initialization sequence (extended page registers 0x50-0xFA,
// gamma, GOA mapping, power) is based on a verified, working ESP-IDF
// driver for the NV3007 on a 142x428 panel (IVO 2.66"/2.79"). This is
// not a guessed sequence - the comments on each block describe the
// purpose of the registers according to the panel manufacturer's
// reference source.

#include "Adafruit_NV3007.h"

#ifndef ARDUINO_STM32_FEATHER
#include "pins_arduino.h"
#ifndef RASPI
#include "wiring_private.h"
#endif
#endif
#include <limits.h>

// --------------------------------------------------------------
// Constructors - just forward parameters to Adafruit_SPITFT
// --------------------------------------------------------------

Adafruit_NV3007::Adafruit_NV3007(int8_t cs, int8_t dc, int8_t mosi,
                                  int8_t sclk, int8_t rst, int8_t miso)
    : Adafruit_SPITFT(NV3007_TFTWIDTH, NV3007_TFTHEIGHT, cs, dc, mosi, sclk,
                       rst, miso)
{
}

Adafruit_NV3007::Adafruit_NV3007(int8_t cs, int8_t dc, int8_t rst)
    : Adafruit_SPITFT(NV3007_TFTWIDTH, NV3007_TFTHEIGHT, cs, dc, rst)
{
}

Adafruit_NV3007::Adafruit_NV3007(SPIClass *spiClass, int8_t cs, int8_t dc,
                                  int8_t rst)
    : Adafruit_SPITFT(NV3007_TFTWIDTH, NV3007_TFTHEIGHT, spiClass, cs, dc,
                       rst)
{
}

// --------------------------------------------------------------
// Helper function - write a command with a single data byte
// --------------------------------------------------------------
void Adafruit_NV3007::writeReg1(uint8_t cmd, uint8_t data)
{
    sendCommand(cmd, &data, 1);
}

// --------------------------------------------------------------
// begin() - initializes the SPI bus and the controller itself
// --------------------------------------------------------------
void Adafruit_NV3007::begin(uint32_t freq)
{
    if (!freq)
    {
        // Default 80 MHz: this is what the K197-ESP32S3 firmware runs
        // with (tft.begin() without an argument) on the 2.79" 142x428
        // panel, with short wiring. The controller datasheet is more
        // conservative (roughly 40-50 MHz); if you see artefacts, call
        // begin(40000000UL).
        freq = 80000000UL;
    }

    // NV3007 requires SPI mode 3 (CPOL=1, CPHA=1)
    initSPI(freq, SPI_MODE3);

    initNV3007();
}

// --------------------------------------------------------------
// NV3007 initialization sequence
// --------------------------------------------------------------
void Adafruit_NV3007::initNV3007(void)
{
    // Note: if a valid RST pin was passed to the constructor, the
    // hardware reset (pin toggle with sufficient delays) was already
    // performed by Adafruit_SPITFT::initSPI(), which is called in
    // begin() BEFORE this function. Here we just add a software
    // reset as a safety net - it is harmless even after a hardware
    // reset, and it also covers the case where no RST pin was wired
    // (rst = -1 in the constructor).
    sendCommand(NV3007_SWRESET);
    delay(150);

    // === Enter the extended register page (0x50-0xFA) ===
    writeReg1(0xFF, 0xA5);
    delay(1);

    // Basic GIP/interface settings
    writeReg1(0x9A, 0x08);
    writeReg1(0x9B, 0x08);
    writeReg1(0x9C, 0xB0);
    writeReg1(0x9D, 0x16);
    writeReg1(0x9E, 0xC4);

    // Power settings (VGH/VGL/DDVDH/DDVDL) - critically affects image
    // quality
    {
        uint8_t d[2] = {0x55, 0x04};
        sendCommand(0x8F, d, 2);
    }
    writeReg1(0x84, 0x90);
    writeReg1(0x83, 0x7B);
    writeReg1(0x85, 0x33);

    // Gamma correction (positive/negative branch)
    writeReg1(0x60, 0x00);
    writeReg1(0x70, 0x00);
    writeReg1(0x61, 0x02);
    writeReg1(0x71, 0x02);
    writeReg1(0x62, 0x04);
    writeReg1(0x72, 0x04);
    writeReg1(0x6C, 0x29);
    writeReg1(0x7C, 0x29);
    writeReg1(0x6D, 0x31);
    writeReg1(0x7D, 0x31);
    writeReg1(0x6E, 0x0F);
    writeReg1(0x7E, 0x0F);
    writeReg1(0x66, 0x21);
    writeReg1(0x76, 0x21);
    writeReg1(0x68, 0x3A);
    writeReg1(0x78, 0x3A);
    writeReg1(0x63, 0x07);
    writeReg1(0x73, 0x07);
    writeReg1(0x64, 0x05);
    writeReg1(0x74, 0x05);
    writeReg1(0x65, 0x02);
    writeReg1(0x75, 0x02);
    writeReg1(0x67, 0x23);
    writeReg1(0x77, 0x23);
    writeReg1(0x69, 0x08);
    writeReg1(0x79, 0x08);
    writeReg1(0x6A, 0x13);
    writeReg1(0x7A, 0x13);
    writeReg1(0x6B, 0x13);
    writeReg1(0x7B, 0x13);
    writeReg1(0x6F, 0x00);
    writeReg1(0x7F, 0x00);

    // Source driver settings
    writeReg1(0x50, 0x00);
    writeReg1(0x52, 0xD6);
    writeReg1(0x53, 0x08);
    writeReg1(0x54, 0x08);
    writeReg1(0x55, 0x1E);
    writeReg1(0x56, 0x1C);

    // GOA map_sel (gate driver mapping)
    {
        uint8_t d[3] = {0x2B, 0x24, 0x00};
        sendCommand(0xA0, d, 3);
    }
    writeReg1(0xA1, 0x87);
    writeReg1(0xA2, 0x86);
    writeReg1(0xA5, 0x00);
    writeReg1(0xA6, 0x00);
    writeReg1(0xA7, 0x00);
    writeReg1(0xA8, 0x36);
    writeReg1(0xA9, 0x7E);
    writeReg1(0xAA, 0x7E);

    // GOA timing (0xB9-0xC6)
    writeReg1(0xB9, 0x85);
    writeReg1(0xBA, 0x84);
    writeReg1(0xBB, 0x83);
    writeReg1(0xBC, 0x82);
    writeReg1(0xBD, 0x81);
    writeReg1(0xBE, 0x80);
    writeReg1(0xBF, 0x01);
    writeReg1(0xC0, 0x02);
    writeReg1(0xC1, 0x00);
    writeReg1(0xC2, 0x00);
    writeReg1(0xC3, 0x00);
    writeReg1(0xC4, 0x33);
    writeReg1(0xC5, 0x7E);
    writeReg1(0xC6, 0x7E);

    // Gate control signals (0xC8-0xD1)
    {
        uint8_t d[2] = {0x33, 0x33};
        sendCommand(0xC8, d, 2);
    }
    writeReg1(0xC9, 0x68);
    writeReg1(0xCA, 0x69);
    writeReg1(0xCB, 0x6A);
    writeReg1(0xCC, 0x6B);
    {
        uint8_t d[2] = {0x33, 0x33};
        sendCommand(0xCD, d, 2);
    }
    writeReg1(0xCE, 0x6C);
    writeReg1(0xCF, 0x6D);
    writeReg1(0xD0, 0x6E);
    writeReg1(0xD1, 0x6F);

    // Multiplex outputs (0xAB-0xAE)
    {
        uint8_t d[2] = {0x03, 0x67};
        sendCommand(0xAB, d, 2);
    }
    {
        uint8_t d[2] = {0x03, 0x6B};
        sendCommand(0xAC, d, 2);
    }
    {
        uint8_t d[2] = {0x03, 0x68};
        sendCommand(0xAD, d, 2);
    }
    {
        uint8_t d[2] = {0x03, 0x6C};
        sendCommand(0xAE, d, 2);
    }

    // 0xB3-0xB8
    writeReg1(0xB3, 0x00);
    writeReg1(0xB4, 0x00);
    writeReg1(0xB5, 0x00);
    writeReg1(0xB6, 0x32);
    writeReg1(0xB7, 0x7E);
    writeReg1(0xB8, 0x7E);

    // Power management (0xE0-0xF1)
    writeReg1(0xE0, 0x00);
    {
        uint8_t d[2] = {0x03, 0x0F};
        sendCommand(0xE1, d, 2);
    }
    writeReg1(0xE2, 0x04);
    writeReg1(0xE3, 0x01);
    writeReg1(0xE4, 0x0E);
    writeReg1(0xE5, 0x01);
    writeReg1(0xE6, 0x19);
    writeReg1(0xE7, 0x10);
    writeReg1(0xE8, 0x10);
    writeReg1(0xEA, 0x12);
    writeReg1(0xEB, 0xD0);
    writeReg1(0xEC, 0x04);
    writeReg1(0xED, 0x07);
    writeReg1(0xEE, 0x07);
    writeReg1(0xEF, 0x09);
    writeReg1(0xF0, 0xD0);
    writeReg1(0xF1, 0x0E);

    writeReg1(0xF9, 0x17);

    // 0xF2 - affects refresh/redraw quality
    {
        uint8_t d[4] = {0x2C, 0x1B, 0x0B, 0x20};
        sendCommand(0xF2, d, 4);
    }

    // "1 dot" setting
    writeReg1(0xE9, 0x29);
    writeReg1(0xEC, 0x04);

    // Tearing effect (default off / can be enabled via
    // enableTearingEffect())
    writeReg1(0x35, 0x00);
    {
        uint8_t d[2] = {0x00, 0x10};
        sendCommand(0x44, d, 2);
    }
    writeReg1(0x46, 0x10);

    // === Return to the standard register page (MIPI DCS commands) ===
    writeReg1(0xFF, 0x00);

    // Pixel format: 16bit RGB565
    writeReg1(NV3007_COLMOD, 0x05);

    // Wake up from sleep mode
    sendCommand(NV3007_SLPOUT);
    delay(220);

    // Turn the display on
    sendCommand(NV3007_DISPON);
    delay(200);

    // Default orientation (rotation 0) - set via setRotation()
    setRotation(0);
}

// --------------------------------------------------------------
// setRotation() - MADCTL (0x36) was not explicitly used/verified by
// the manufacturer in the reference init file, but it follows
// standard MIPI behavior; if rotation/mirroring does not match,
// adjust the table below.
// --------------------------------------------------------------
void Adafruit_NV3007::setRotation(uint8_t r)
{
    rotation = r & 3;

    uint8_t madctl = 0;

    switch (rotation)
    {
    case 0:
        madctl = NV3007_MADCTL_RGB;
        _width = NV3007_TFTWIDTH;
        _height = NV3007_TFTHEIGHT;
        break;
    case 1:
        madctl = NV3007_MADCTL_MX | NV3007_MADCTL_MV | NV3007_MADCTL_RGB;
        _width = NV3007_TFTHEIGHT;
        _height = NV3007_TFTWIDTH;
        break;
    case 2:
        madctl = NV3007_MADCTL_MX | NV3007_MADCTL_MY | NV3007_MADCTL_RGB;
        _width = NV3007_TFTWIDTH;
        _height = NV3007_TFTHEIGHT;
        break;
    case 3:
        madctl = NV3007_MADCTL_MY | NV3007_MADCTL_MV | NV3007_MADCTL_RGB;
        _width = NV3007_TFTHEIGHT;
        _height = NV3007_TFTWIDTH;
        break;
    }

    writeReg1(NV3007_MADCTL, madctl);
}

// --------------------------------------------------------------
// setAddrWindow() - sets the GRAM write window (CASET/RASET/RAMWR)
// --------------------------------------------------------------
void Adafruit_NV3007::setAddrWindow(uint16_t x, uint16_t y, uint16_t w,
                                     uint16_t h)
{
    uint16_t x1 = x + NV3007_COL_OFFSET;
    uint16_t x2 = x1 + w - 1;
    uint16_t y1 = y + NV3007_ROW_OFFSET;
    uint16_t y2 = y1 + h - 1;

    writeCommand(NV3007_CASET);
    SPI_WRITE16(x1);
    SPI_WRITE16(x2);

    writeCommand(NV3007_RASET);
    SPI_WRITE16(y1);
    SPI_WRITE16(y2);

    writeCommand(NV3007_RAMWR);
}

// --------------------------------------------------------------
// Additional helper functions
// --------------------------------------------------------------
void Adafruit_NV3007::invertDisplay(bool invert)
{
    sendCommand(invert ? NV3007_INVON : NV3007_INVOFF);
}

void Adafruit_NV3007::enableTearingEffect(bool enable)
{
    if (enable)
    {
        // 0x00 = TE only on V-blank
        writeReg1(NV3007_TEON, 0x00);
    }
    else
    {
        sendCommand(NV3007_TEOFF);
    }
}
