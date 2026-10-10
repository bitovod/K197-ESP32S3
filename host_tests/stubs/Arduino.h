#pragma once
// Minimal stand-in for Arduino.h so the sketch's pure logic and the whole of
// datalog.ino / reading_math.ino / ui.ino / display.ino can be compiled and tested
// on a PC. Only what those files actually use.
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <string>
#include <algorithm>
#include <chrono>
#define PROGMEM
#define pgm_read_byte(a) (*(const uint8_t *)(a))
#define pgm_read_word(a) (*(const uint16_t *)(a))
#define F(x) (x)

// Time: real microseconds, millis() can be pushed forward by tests.
inline uint32_t &fakeMillisOffset()
{
    static uint32_t offset = 0;
    return offset;
}
inline unsigned long micros()
{
    return (unsigned long)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline unsigned long millis()
{
    return (unsigned long)(micros() / 1000UL) + fakeMillisOffset();
}

class String
{
public:
    String(const char *c = "") : s(c) {}
    size_t length() const { return s.size(); }
    char charAt(size_t i) const { return s[i]; }
    void trim()
    {
        size_t a = s.find_first_not_of(" \t\r\n");
        size_t b = s.find_last_not_of(" \t\r\n");
        s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
    }
    void toUpperCase()
    {
        for (auto &c : s) { if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A'); }
    }
    bool equals(const char *o) const { return s == o; }
    std::string s;
};

// Output sink that swallows everything (the LOG:* printers write to a Stream).
inline void (*serialWriteHook)(const uint8_t *, size_t) = nullptr;
class Stream
{
public:
    template <typename T> size_t print(T) { return 0; }
    template <typename T> size_t print(T, int) { return 0; }
    virtual size_t write(const uint8_t *b, size_t n) { if (serialWriteHook) serialWriteHook(b, n); return n; }
    size_t println() { return 0; }
    template <typename T> size_t println(T) { return 0; }
    template <typename T> size_t println(T, int) { return 0; }
};
struct SerialStub : public Stream {};
inline SerialStub Serial;

struct EspStub
{
    size_t freePsram = 0;
    size_t getFreePsram() { return freePsram; }
};
inline EspStub ESP;
// v16: tests can make large PSRAM allocations fail (fakePsMallocLimit) and see what was asked for.
inline size_t &fakePsMallocLimit() { static size_t l = (size_t)-1; return l; }
inline size_t &fakePsMallocLastOk() { static size_t n = 0; return n; }
inline void *ps_malloc(size_t n)
{
    if (n > fakePsMallocLimit()) return nullptr;
    fakePsMallocLastOk() = n;
    return malloc(n);
}

// AVR/ESP helper used by keithleyPrintScientific().
inline char *dtostrf(double v, signed char width, unsigned char prec, char *out)
{
    snprintf(out, 32, "%*.*f", (int)width, (int)prec, v);
    return out;
}

// ---- GPIO stand-ins for the button test: pin levels the test sets, outputs it can read back, and the
// ---- interrupt handlers the code attaches (the test calls them like the hardware would)
#define INPUT_PULLUP 2
#ifndef INPUT
#define INPUT 0
#endif
#define OUTPUT 1
#define LOW 0
#define HIGH 1
#define CHANGE 3
inline int gpioLevel[64] = {};
inline int gpioOut[64] = {};
inline int gpioOutHighCount[64] = {};
inline void (*gpioIsr[64])(void *) = {};
inline void *gpioIsrArg[64] = {};
inline void pinMode(int pin, int mode)
{
    if (mode == INPUT_PULLUP) gpioLevel[pin] = HIGH;
}
inline int digitalRead(int pin) { return gpioLevel[pin]; }
inline void digitalWrite(int pin, int value)
{
    gpioOut[pin] = value;
    if (value == HIGH) gpioOutHighCount[pin]++;
}
inline int digitalPinToInterrupt(int pin) { return pin; }
inline void attachInterruptArg(int irq, void (*fn)(void *), void *arg, int)
{
    gpioIsr[irq] = fn;
    gpioIsrArg[irq] = arg;
}
