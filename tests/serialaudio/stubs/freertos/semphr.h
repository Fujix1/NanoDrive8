#pragma once
#include "task.h"
using SemaphoreHandle_t = void*;
constexpr unsigned portMAX_DELAY = 0xffffffffu;
inline void* xSemaphoreCreateMutex() { return reinterpret_cast<void*>(1); }
inline void xSemaphoreTake(void*,unsigned) {}
inline void xSemaphoreGive(void*) {}
