#pragma once
// One-shot esp_timer stand-in: the callback runs on its own thread after the delay.
#include <stdint.h>
#include <thread>
#include <chrono>
typedef void *esp_timer_handle_t;
#define ESP_TIMER_TASK 0
struct esp_timer_create_args_t
{
    void (*callback)(void *);
    void *arg;
    int dispatch_method;
    const char *name;
};
struct EspTimerObj { void (*cb)(void *); void *arg; };
inline int esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h)
{
    *h = new EspTimerObj{a->callback, a->arg};
    return 0;
}
inline int esp_timer_stop(esp_timer_handle_t) { return 0; } // stand-in: the pending callback still runs
inline int esp_timer_start_once(esp_timer_handle_t h, uint64_t us)
{
    EspTimerObj *t = (EspTimerObj *)h;
    std::thread([t, us]() { std::this_thread::sleep_for(std::chrono::microseconds(us)); t->cb(t->arg); }).detach();
    return 0;
}
// v17: periodic timers (alarm.ino) - not run on the host; the tests call the callback body by hand.
inline int esp_timer_start_periodic(esp_timer_handle_t, uint64_t) { return 0; }
