#pragma once
#include <stdint.h>
#include <stddef.h>
#define IRAM_ATTR
#define INPUT 0
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)0)
#define portEXIT_CRITICAL(p) ((void)0)
#define portENTER_CRITICAL_ISR(p) ((void)0)
#define portEXIT_CRITICAL_ISR(p) ((void)0)
extern unsigned assertionFailures;
#define configASSERT(x) do { if (!(x)) ++assertionFailures; } while(0)
#define ESP_ERROR_CHECK(x) configASSERT((x)==0)
inline void pinMode(int,int) {}
inline void ets_delay_us(unsigned) {}

using esp_event_base_t = void*;
using EventHandler = void (*)(void*, esp_event_base_t, int32_t, void*);
constexpr int ARDUINO_HW_CDC_BUS_RESET_EVENT = 1;
extern uint32_t fakeMillis;
inline uint32_t millis() { return fakeMillis; }
struct SerialStub {
 bool plugged, connected;
 uint8_t rx[65536]; size_t head, tail;
 EventHandler busReset;
 bool isPlugged() { return plugged; }
 bool isConnected() { return connected; }
 void onEvent(int, EventHandler handler) { busReset = handler; }
 int available() { return int(tail-head); }
 size_t read(uint8_t* p, size_t size) {
  if(size>tail-head)size=tail-head;
  for(size_t i=0;i<size;++i)p[i]=rx[head++];
  return size;
 }
 int availableForWrite() { return 256; }
 size_t write(const uint8_t*,size_t size) { return size; }
 void append(const uint8_t* p, size_t n) { for(size_t i=0;i<n;++i)rx[tail++]=p[i]; }
};
extern SerialStub Serial;
