#pragma once
#include <stdint.h>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
typedef uint32_t TickType_t;
#define portMAX_DELAY 0xFFFFFFFFu
#define pdTRUE 1
#define pdMS_TO_TICKS(x) ((TickType_t)(x))
struct portMUX_TYPE_s { std::mutex m; };
typedef portMUX_TYPE_s portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(p) ((p)->m.lock())
#define portEXIT_CRITICAL(p) ((p)->m.unlock())
