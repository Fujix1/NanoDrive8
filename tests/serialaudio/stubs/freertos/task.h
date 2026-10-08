#pragma once
using TaskHandle_t = void*;
using BaseType_t = int;
constexpr int APP_CPU_NUM = 1, pdPASS = 1;
inline int xTaskCreatePinnedToCore(void(*)(void*),const char*,unsigned,void*,int,void** out,int) { *out=reinterpret_cast<void*>(1); return 1; }
inline void vTaskDelay(unsigned) {}
