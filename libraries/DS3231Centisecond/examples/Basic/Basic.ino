// Basic.ino
//
// Minimal DS3231Centisecond example: prints the current date/time
// every time the centisecond digits change, so you see a clean count
// 00,01,02...99,00,01... with no gaps or repeats.
//
// Wire the DS3231's SDA/SCL to your board's I2C pins (add them to
// Wire.begin() below if your board needs explicit pins, e.g. most
// ESP32 boards: Wire.begin(SDA_PIN, SCL_PIN)).

#include <Wire.h>
#include <DS3231Centisecond.h>

DS3231Centisecond rtc;

uint8_t lastPrintedCenti = 0xFF;

void setup()
{
    Serial.begin(115200);
    unsigned long start = millis();
    while (!Serial && (millis() - start) < 3000)
    {
        delay(10);
    }

    Wire.begin(); // e.g. Wire.begin(21, 22) on many ESP32 boards

    if (rtc.begin())
    {
        Serial.println("DS3231 OK.");
    }
    else
    {
        Serial.println("DS3231 NOT responding - check wiring/address.");
    }
}

void loop()
{
    rtc.poll(); // must run every loop() iteration - see the library's header comment

    uint8_t centi = rtc.centisecond();
    if (centi != 0xFF && centi != lastPrintedCenti)
    {
        lastPrintedCenti = centi;

        char buf[24];
        rtc.formatDateTime(buf, sizeof(buf));
        Serial.println(buf);
    }
}
