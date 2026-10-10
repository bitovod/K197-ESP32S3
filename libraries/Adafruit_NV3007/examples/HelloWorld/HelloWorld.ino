// HelloWorld - minimal test of the Adafruit_NV3007 driver.
// Pins are the ones of the K197-ESP32S3 board (ESP32-S3); change them for yours.

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_NV3007.h>

#define TFT_CS   2
#define TFT_DC   10
#define TFT_RST  9
#define TFT_SCK  12
#define TFT_MOSI 11

Adafruit_NV3007 tft(&SPI, TFT_CS, TFT_DC, TFT_RST);

void setup() {
  SPI.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS);
  tft.begin();            // 80 MHz; use tft.begin(40000000UL) if you see artefacts
  tft.setRotation(1);     // landscape, 428 x 142
  tft.fillScreen(0x0000);
  tft.drawRect(0, 0, tft.width(), tft.height(), 0xFFFF);
  tft.setTextColor(0xFFE0);
  tft.setTextSize(3);
  tft.setCursor(20, 50);
  tft.print("NV3007 OK");
}

void loop() {
}
