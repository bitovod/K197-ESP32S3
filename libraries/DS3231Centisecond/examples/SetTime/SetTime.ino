// SetTime.ino
//
// Shows how to write a new date/time to the DS3231 over Serial, and
// doubles as a battery-retention test: set the clock, power-cycle the
// board with only the RTC's own backup battery/supercap connected,
// then power the board back up and check the printed time picked up
// where it left off instead of resetting.
//
// Open the Serial Monitor at 115200 baud and send a line like:
//
//   SET 2026-09-24 14:30:00
//
// Wire the DS3231's SDA/SCL to your board's I2C pins (add them to
// Wire.begin() below if your board needs explicit pins, e.g. most
// ESP32 boards: Wire.begin(SDA_PIN, SCL_PIN)).

#include <Wire.h>
#include <DS3231Centisecond.h>

DS3231Centisecond rtc;

uint8_t lastPrintedCenti = 0xFF;

// Reads one line from Serial (if a full line is available) and, if it
// matches "SET YYYY-MM-DD HH:MM:SS", writes that date/time to the RTC.
void handleSerialCommand()
{
    if (!Serial.available())
    {
        return;
    }

    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() == 0)
    {
        return;
    }

    uint16_t year;
    uint8_t month, day, hour, minute, second;

    if (sscanf(line.c_str(), "SET %hu-%hhu-%hhu %hhu:%hhu:%hhu",
               &year, &month, &day, &hour, &minute, &second) == 6)
    {
        rtc.setDateTime(year, month, day, hour, minute, second);
        Serial.println("RTC set. Verifying:");

        uint8_t vSec, vMin, vHour, vDay, vMonth;
        uint16_t vYear;
        if (rtc.readNow(vSec, vMin, vHour, vDay, vMonth, vYear))
        {
            Serial.printf("RTC date/time: %04u-%02u-%02u %02u:%02u:%02u\n",
                          vYear, vMonth, vDay, vHour, vMin, vSec);
        }
    }
    else
    {
        Serial.println("Unrecognized command. Use: SET YYYY-MM-DD HH:MM:SS");
    }
}

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

    Serial.println("Send: SET YYYY-MM-DD HH:MM:SS");
    Serial.println("Then power off/on the board (RTC on its own battery)");
    Serial.println("and check the printed time picked up where it left off.");
}

void loop()
{
    handleSerialCommand();
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
