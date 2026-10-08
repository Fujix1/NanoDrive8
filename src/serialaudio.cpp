#include "serialaudio.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>

#include "NJU72342.h"
#include "fm.h"

namespace SerialAudio {
namespace {
constexpr uint32_t Capacity = 4096;
constexpr unsigned EventCapacity = 1024;
struct Event {
  uint32_t position;
  uint32_t value;
  uint16_t extra;
  uint8_t kind;
  uint8_t address;
};
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
uint8_t* bytes = nullptr;
Event* events = nullptr;
uint32_t written = 0, consumed = 0, endPosition = 0;
uint32_t underflows = 0, overflows = 0, rejected = 0, late = 0, maxLate = 0;
uint32_t highWater = 0;
uint32_t faultReasons = 0;
unsigned eventRead = 0, eventWrite = 0, eventCount = 0;
uint32_t lastEventPosition = 0;
bool haveEvent = false, haveEnd = false, running = false, ended = false;
bool fault = false, faultMuted = false;
uint8_t silence = 0x80;
ClockChanged changeClock = nullptr;

uint32_t get32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
      (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
void put32(uint8_t* p, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) p[i] = v >> (i * 8);
}
bool validRate(uint32_t hz, uint16_t divider) {
  return (hz == 4000000 || hz == 8000000) &&
      (divider == 512 || divider == 768 || divider == 1024);
}
void reject() {
  portENTER_CRITICAL(&mux);
  ++rejected;
  fault = true;
  faultReasons |= InvalidCommand;
  portEXIT_CRITICAL(&mux);
}

void IRAM_ATTR requestByte(void*) {
  // Internal RAM only. No parsing, allocation, I2C, USB or task notification.
  // Holding this lock through the GPIO write fences reset against an in-flight ISR.
  portENTER_CRITICAL_ISR(&mux);
  if (running) {
    uint8_t value;
    if (fault) {
      value = 0x80;  // Fault is latched and the task mutes OKI; never resume this stream.
    } else if (haveEnd && consumed == endPosition) {
      ended = true;
      value = silence;  // Host-encoded, state-matched zero pair after the zero tail.
    } else if (consumed != written) {
      value = bytes[consumed & (Capacity - 1)];
      ++consumed;
    } else {
      ++underflows;
      fault = true;
      faultReasons |= Underflow;
      value = 0x80;
    }
    FM.setOKIM6258dataISR(value, 1);
  }
  portEXIT_CRITICAL_ISR(&mux);
}

void apply(const Event& e) {
  switch (e.kind) {
    case 0: FM.setRegisterOPM(e.address, uint8_t(e.value), 0); break;
    case 1:
      changeClock(e.value);
      FM.setOKIM6258divider(static_cast<tOKIM6258Divider>(e.extra));
      break;
    case 2: nju72342.panSetPan(static_cast<tPan>(e.value)); break;
  }
}
}  // namespace

void init(ClockChanged clock) {
  changeClock = clock;
  bytes = static_cast<uint8_t*>(heap_caps_malloc(Capacity, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  events = static_cast<Event*>(heap_caps_malloc(sizeof(Event) * EventCapacity,
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  configASSERT(bytes && events && changeClock);
  pinMode(MCK, INPUT);
  ESP_ERROR_CHECK(gpio_set_intr_type(static_cast<gpio_num_t>(MCK), GPIO_INTR_POSEDGE));
  ESP_ERROR_CHECK(gpio_isr_handler_add(static_cast<gpio_num_t>(MCK), requestByte, nullptr));
}

void reset() {
  portENTER_CRITICAL(&mux);
  running = ended = fault = haveEnd = false;
  written = consumed = endPosition = 0;
  underflows = overflows = rejected = highWater = 0;
  faultReasons = 0;
  portEXIT_CRITICAL(&mux);
  eventRead = eventWrite = eventCount = 0;
  lastEventPosition = late = maxLate = 0;
  haveEvent = faultMuted = false;
  nju72342.panMute();
}

void transportLost(uint32_t reason) {
  portENTER_CRITICAL(&mux);
  if (running || written != 0) { fault = true; faultReasons |= reason; }
  portEXIT_CRITICAL(&mux);
}

void service() {
  portENTER_CRITICAL(&mux);
  const bool failed = fault;
  const bool active = running;
  portEXIT_CRITICAL(&mux);
  if (failed) {
    if (!faultMuted) { nju72342.panMute(); faultMuted = true; }
    return;
  }
  if (!active) return;
  // Event timing is byte-boundary based; I2C/FM run in the task, not the supply ISR.
  // Bound FM work so USB parsing is serviced between dense groups of events.
  for (unsigned work = 0; eventCount && work < 32; ++work) {
    portENTER_CRITICAL(&mux);
    const uint32_t position = consumed;
    const bool failedNow = fault;
    portEXIT_CRITICAL(&mux);
    if (failedNow) {
      if (!faultMuted) { nju72342.panMute(); faultMuted = true; }
      return;
    }
    const Event& e = events[eventRead];
    if (e.position > position) break;
    if (position > e.position) {
      ++late;
      if (position - e.position > maxLate) maxLate = position - e.position;
    }
    apply(e);
    eventRead = (eventRead + 1) % EventCapacity;
    --eventCount;
  }
}

size_t command(uint8_t opcode, const uint8_t* data, size_t size, uint8_t* response) {
  if (opcode == 0x5b) {
    portENTER_CRITICAL(&mux);
    const uint32_t values[] = {written, consumed, written - consumed, underflows,
        overflows, rejected, highWater,
        uint32_t(running) | (uint32_t(ended) << 1) | (uint32_t(fault) << 2) | faultReasons};
    portEXIT_CRITICAL(&mux);
    for (unsigned i = 0; i < 8; ++i) put32(response + 4 * i, values[i]);
    put32(response + 32, late);
    put32(response + 36, maxLate);
    return 40;
  }
  portENTER_CRITICAL(&mux);
  const bool failed = fault;
  portEXIT_CRITICAL(&mux);
  if (failed) return 0;

  if (opcode == 0x58) {
    if (size < 5 || size > 256) { reject(); return 0; }
    const uint32_t position = get32(data);
    const uint32_t count = size - 4;
    portENTER_CRITICAL(&mux);
    const uint32_t start = written;
    const bool full = count > Capacity - (written - consumed);
    const bool invalid = position != written || haveEnd || written > UINT32_MAX - count;
    if (full) { ++overflows; fault = true; faultReasons |= DataOverflow; }
    portEXIT_CRITICAL(&mux);
    if (full) return 0;
    if (invalid) { reject(); return 0; }
    // Single producer. Publish only after the whole chunk is copied; ISR can
    // consume older bytes during the copy, shortening the interrupt-off window.
    for (uint32_t i = 0; i < count; ++i) bytes[(start + i) & (Capacity - 1)] = data[4 + i];
    portENTER_CRITICAL(&mux);
    written += count;
    if (written - consumed > highWater) highWater = written - consumed;
    portEXIT_CRITICAL(&mux);
  } else if (opcode == 0x59) {
    if (size < 6) { reject(); return 0; }
    const uint32_t position = get32(data);
    const uint8_t kind = data[4];
    if (haveEvent && position < lastEventPosition) { reject(); return 0; }
    if (kind == 3) {
      // End is installed on receipt, not after a task observes the final byte.
      portENTER_CRITICAL(&mux);
      const bool valid = size == 6 && !haveEnd && position == written &&
          (data[5] == 0x80 || data[5] == 0x08);
      if (valid) { haveEnd = true; endPosition = position; silence = data[5]; }
      portEXIT_CRITICAL(&mux);
      if (!valid) reject();
      return 0;
    }
    const unsigned count = kind == 0 ? (size - 5) / 2 : 1;
    const uint16_t div = size == 11 ? uint16_t(data[9]) | (uint16_t(data[10]) << 8) : 0;
    const bool valid = (kind == 0 && size >= 7 && ((size - 5) & 1) == 0) ||
        (kind == 1 && size == 11 && validRate(get32(data + 5), div)) ||
        (kind == 2 && size == 6 && data[5] <= 3);
    if (!valid || haveEnd) { reject(); return 0; }
    if (count > EventCapacity - eventCount) {
      portENTER_CRITICAL(&mux);
      ++overflows; fault = true; faultReasons |= EventOverflow;
      portEXIT_CRITICAL(&mux);
      return 0;
    }
    for (unsigned i = 0; i < count; ++i) {
      Event e = {position, 0, div, kind, 0};
      if (kind == 0) { e.address = data[5 + 2 * i]; e.value = data[6 + 2 * i]; }
      else e.value = kind == 1 ? get32(data + 5) : data[5];
      events[eventWrite] = e;
      eventWrite = (eventWrite + 1) % EventCapacity;
      ++eventCount;
    }
    haveEvent = true;
    lastEventPosition = position;
  } else if (opcode == 0x5a) {
    if (size != 6 || !validRate(get32(data), uint16_t(data[4]) | (uint16_t(data[5]) << 8))) {
      reject(); return 0;
    }
    portENTER_CRITICAL(&mux);
    const bool valid = !running && written > 0 && consumed == 0;
    portEXIT_CRITICAL(&mux);
    if (!valid) { reject(); return 0; }
    nju72342.panMute();
    changeClock(get32(data));
    FM.setOKIM6258divider(static_cast<tOKIM6258Divider>(uint16_t(data[4]) | (uint16_t(data[5]) << 8)));
    portENTER_CRITICAL(&mux); running = true; portEXIT_CRITICAL(&mux);
    FM.setOKIM6258command(0x02, 1);
    ets_delay_us(260);
    nju72342.panSetPan(PAN_CENTER);
    nju72342.panUnmute();
    // Host includes an encoded-zero preroll so this settling interval loses no note.
  }
  return 0;
}
}  // namespace SerialAudio
