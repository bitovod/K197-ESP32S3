#pragma once
#include "FreeRTOS.h"
#include <mutex>
typedef std::mutex *SemaphoreHandle_t;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new std::mutex(); }
inline int xSemaphoreTake(SemaphoreHandle_t m, uint32_t) { m->lock(); return 1; }
inline int xSemaphoreGive(SemaphoreHandle_t m) { m->unlock(); return 1; }
