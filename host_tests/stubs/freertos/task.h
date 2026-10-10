#pragma once
#include "FreeRTOS.h"
struct TaskObj { std::mutex m; std::condition_variable cv; uint32_t count = 0; uint32_t bits = 0; };
typedef int BaseType_t;
#define pdFALSE 0
typedef enum { eNoAction = 0, eSetBits = 1 } eNotifyAction;
typedef TaskObj *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
inline thread_local TaskObj *g_self = nullptr;
inline int xTaskCreatePinnedToCore(TaskFunction_t fn, const char *, uint32_t, void *arg, int, TaskHandle_t *h, int)
{
    TaskObj *o = new TaskObj();
    if (h) *h = o;
    std::thread([fn, arg, o]() { g_self = o; fn(arg); }).detach();
    return 1;
}
inline void xTaskNotifyGive(TaskHandle_t h)
{
    std::lock_guard<std::mutex> l(h->m); h->count++; h->cv.notify_one();
}
inline uint32_t ulTaskNotifyTake(int clear, TickType_t ticks)
{
    TaskObj *o = g_self;
    std::unique_lock<std::mutex> l(o->m);
    if (ticks == portMAX_DELAY) o->cv.wait(l, [&] { return o->count > 0; });
    else o->cv.wait_for(l, std::chrono::milliseconds(ticks), [&] { return o->count > 0; });
    uint32_t prev = o->count;
    if (clear) o->count = 0; else if (o->count) o->count--;
    return prev;
}
inline void vTaskDelay(TickType_t t) { std::this_thread::sleep_for(std::chrono::milliseconds(t)); }
inline uint32_t uxTaskGetStackHighWaterMark(void *) { return 1234; }

inline void vTaskNotifyGiveFromISR(TaskHandle_t h, BaseType_t *woken)
{
    xTaskNotifyGive(h);
    if (woken) *woken = pdFALSE;
}
inline int xTaskNotify(TaskHandle_t h, uint32_t value, eNotifyAction)
{
    std::lock_guard<std::mutex> l(h->m);
    h->bits |= value;
    h->cv.notify_one();
    return 1;
}
// Waits for any notification bit (or until ticks have passed); returns 1 and the bits if there were any.
inline int xTaskNotifyWait(uint32_t clearOnEntry, uint32_t clearOnExit, uint32_t *value, TickType_t ticks)
{
    TaskObj *o = g_self;
    std::unique_lock<std::mutex> l(o->m);
    o->bits &= ~clearOnEntry;
    bool got = true;
    if (ticks == portMAX_DELAY) o->cv.wait(l, [&] { return o->bits != 0; });
    else got = o->cv.wait_for(l, std::chrono::milliseconds(ticks), [&] { return o->bits != 0; });
    if (value) *value = o->bits;
    if (got) o->bits &= ~clearOnExit;
    return got ? 1 : 0;
}
