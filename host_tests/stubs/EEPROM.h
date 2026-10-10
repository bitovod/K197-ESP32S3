// Host stand-in for the ESP32 Arduino EEPROM library: a RAM blob that
// survives "reboots" within one test run, plus a commit counter.
#pragma once
#include <stdint.h>
#include <string.h>
#include <stddef.h>

class EEPROMClass
{
public:
    uint8_t data[512];
    size_t size = 0;
    int commits = 0;
    EEPROMClass() { memset(data, 0xFF, sizeof(data)); }
    bool begin(size_t s) { size = s; return true; }
    template <typename T> T &get(int addr, T &t) { memcpy(&t, data + addr, sizeof(T)); return t; }
    template <typename T> const T &put(int addr, const T &t) { memcpy(pending + addr, &t, sizeof(T)); dirty = true; return t; }
    bool commit() { if (dirty) { memcpy(data, pending, sizeof(data)); } commits++; dirty = false; return true; }
    void reset() { memset(data, 0xFF, sizeof(data)); memset(pending, 0xFF, sizeof(pending)); commits = 0; dirty = false; }
private:
    uint8_t pending[512];
    bool dirty = false;
};
inline EEPROMClass EEPROM;
