// Host test for debuglog.ino (v10): debug messages are queued without ever waiting, gated by SYSTem:DEBUG,
// written by the drain, dropped (and counted) when the queue is full, however slow "USB" is.
#include <Arduino.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <atomic>
#include <string>
#include <vector>
#include <chrono>
#include "debuglog.h"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static bool gDebug = false;
bool serialOutDebugTraceAllowed() { return gDebug; }

// "USB": collects everything written, optionally takes its time per write
static std::mutex gOutMutex;
static std::string gOut;
static std::atomic<int> gWriteDelayMs{0};
static void usbWrite(const uint8_t *b, size_t n)
{
    int d = gWriteDelayMs;
    if (d) std::this_thread::sleep_for(std::chrono::milliseconds(d));
    std::lock_guard<std::mutex> l(gOutMutex);
    gOut.append((const char *)b, n);
}

#include "debuglog.ino"

static double nowUs() { return (double)micros(); }

int main()
{
    serialWriteHook = usbWrite;

    // before debugLogBegin(): harmless
    debugLogAlways("too early");
    debugLogDrain();
    CHECK(gOut.empty(), "nothing happens before debugLogBegin()");
    debugLogBegin();

    // 1. gating: debugLogf() only with SYSTem:DEBUG on, debugLogAlways() always
    gDebug = false;
    debugLogf("hidden %d", 1);
    debugLogAlways("errors are always seen: %s", "yes");
    debugLogDrain();
    CHECK(gOut == "errors are always seen: yes\r\n", "debug off: only the always-message gets through");
    gOut.clear();
    gDebug = true;
    debugLogf("BTN %s pressed", "dB");
    debugLogf("BTN %s released (%s, %lums)", "dB", "short", 60UL);
    debugLogDrain();
    CHECK(gOut == "BTN dB pressed\r\nBTN dB released (short, 60ms)\r\n", "debug on: messages come out in order, one per line");
    gOut.clear();

    // 2. a long message is cut, not overrun
    {
        std::string longText(400, 'x');
        debugLogf("%s", longText.c_str());
        debugLogDrain();
        CHECK(gOut.size() == DEBUGLOG_MESSAGE_CHARS + 2, "long message cut to the limit (+ CR LF)");
        gOut.clear();
    }

    // 3. never waits - not even when the queue is full and "USB" takes 200 ms per message
    {
        gWriteDelayMs = 200;
        uint32_t droppedBefore = debugLogDropped();
        double worst = 0;
        for (int i = 0; i < 2000; i++)
        {
            double t0 = nowUs();
            debugLogf("flood %d", i);
            double dt = nowUs() - t0;
            if (dt > worst) worst = dt;
        }
        uint32_t dropped = debugLogDropped() - droppedBefore;
        printf("3. 2000 messages into a queue of %d, nobody draining: slowest call %.1f us, %u dropped\n", DEBUGLOG_QUEUE_LENGTH, worst, dropped);
        CHECK(worst < 1000, "a call never waits (slowest under 1 ms)");
        CHECK(dropped == 2000 - DEBUGLOG_QUEUE_LENGTH, "everything beyond the queue length is dropped");

        // the drain (which may be slow, like a slow USB link) writes what is there and reports the loss
        gWriteDelayMs = 1;
        gOut.clear();
        debugLogDrain();
        CHECK(gOut.find("flood 0\r\n") == 0, "the oldest messages are the ones kept");
        CHECK(gOut.find("(1976 debug messages dropped)") != std::string::npos, "the loss is reported once");
        gOut.clear();
        debugLogDrain();
        CHECK(gOut.empty(), "...and not again");
    }

    // 4. producers on other threads while the drain is slow: the producers keep their speed
    {
        gWriteDelayMs = 20; // slow link
        gOut.clear();
        std::atomic<bool> stop{false};
        std::atomic<long> maxUs{0};
        std::vector<std::thread> producers;
        for (int t = 0; t < 3; t++)
            producers.emplace_back([&]() {
                while (!stop)
                {
                    double t0 = nowUs();
                    debugLogf("producer message");
                    long dt = (long)(nowUs() - t0);
                    long m = maxUs;
                    while (dt > m && !maxUs.compare_exchange_weak(m, dt)) {}
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
            });
        std::thread drainer([&]() { while (!stop) { debugLogDrain(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); } });
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        stop = true;
        for (auto &t : producers) t.join();
        drainer.join();
        printf("4. 3 producers, drain at 20 ms per message: slowest call %ld us, %u dropped in total\n", maxUs.load(), debugLogDropped());
        CHECK(maxUs < 3000, "producers are never held up by the slow drain");
    }

    // 5. v13 SYSTem:DEBUG:PORT: another stream, with the "# " prefix (used on the SCPI UART)
    {
        struct OtherPort : public Stream
        {
            std::string got;
            size_t write(const uint8_t *b, size_t n) override { got.append((const char *)b, n); return n; }
        } uart;
        gWriteDelayMs = 0;
        debugLogDrain();
        { std::lock_guard<std::mutex> l(gOutMutex); gOut.clear(); }
        debugLogSetOutput(&uart, true);
        debugLogAlways("boot: hello %d", 42);
        debugLogDrain();
        CHECK(uart.got == "# boot: hello 42\r\n", "message on the selected port, with prefix");
        { std::lock_guard<std::mutex> l(gOutMutex); CHECK(gOut.empty(), "nothing on USB meanwhile"); }
        debugLogSetOutput(&Serial, false);
        debugLogAlways("back");
        debugLogDrain();
        { std::lock_guard<std::mutex> l(gOutMutex); CHECK(gOut == "back\r\n", "back on USB, no prefix"); }
        printf("5. output switched to another port with the \"# \" prefix and back\n");
    }

    printf(failures ? "FAILURES: %d\n" : "ALL OK\n", failures);
    return failures != 0;
}
