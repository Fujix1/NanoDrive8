#pragma once

#include <stddef.h>
#include <stdint.h>

namespace SerialAudio {
// AUDIO_STATUS flags retain bits 0..2; upper bits identify latched causes.
enum FaultReason : uint32_t {
  BusReset = 1u << 3,
  ReceiveOverflow = 1u << 4,
  ExplicitDiscard = 1u << 5,
  Underflow = 1u << 6,
  DataOverflow = 1u << 7,
  EventOverflow = 1u << 8,
  InvalidCommand = 1u << 9,
};
using ClockChanged = void (*)(uint32_t);
void init(ClockChanged clock);
// Called under the main output mute, before resetting the physical chips.
void reset();
void service();
void transportLost(uint32_t reason = BusReset);
size_t command(uint8_t opcode, const uint8_t* data, size_t size, uint8_t* response);
}  // namespace SerialAudio
