#pragma once
// FreeRTOS queue stand-in (fixed size items, copy in/out, timeouts only 0 are used by the code under test).
#include "FreeRTOS.h"
#include <deque>
#include <vector>
#include <mutex>
#include <string.h>
#define pdPASS 1
struct QueueObj { std::mutex m; std::deque<std::vector<uint8_t>> q; size_t length; size_t item; };
typedef QueueObj *QueueHandle_t;
inline QueueHandle_t xQueueCreate(size_t length, size_t itemSize) { auto *o = new QueueObj(); o->length = length; o->item = itemSize; return o; }
inline int xQueueSend(QueueHandle_t h, const void *item, TickType_t)
{
    std::lock_guard<std::mutex> l(h->m);
    if (h->q.size() >= h->length) return 0;
    h->q.emplace_back((const uint8_t *)item, (const uint8_t *)item + h->item);
    return 1;
}
inline int xQueueReceive(QueueHandle_t h, void *item, TickType_t)
{
    std::lock_guard<std::mutex> l(h->m);
    if (h->q.empty()) return 0;
    memcpy(item, h->q.front().data(), h->item);
    h->q.pop_front();
    return 1;
}
