#pragma once

#include <stddef.h>
#include <stdint.h>

// Set to 0 at build time for the meter OFF comparison firmware.
#ifndef ND_SERIAL_ADPCM_METER
#define ND_SERIAL_ADPCM_METER 1
#endif

namespace SerialAudio {
uint8_t displayPan();     // Applied OKI PAN (0=center, 1=left, 2=right, 3=mute).
uint8_t takeMeterLevel();  // Mixed ADPCM only, 0..15; display task only.
bool meterSuspended();    // Queue loss disables the meter until RESET.

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
// After physical RESET/STOP, drive encoded silence without advancing the
// host's byte position. The RESET caller still holds the main output mute.
void prepareOutput();
void service();
void transportLost(uint32_t reason = BusReset);
size_t command(uint8_t opcode, const uint8_t* data, size_t size, uint8_t* response);
}  // namespace SerialAudio
