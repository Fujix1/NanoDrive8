#ifndef NDSIF_H
#define NDSIF_H

#include <stddef.h>
#include <stdint.h>

namespace ndsif {
// Immediate controls plus experimental continuous-ADPCM callbacks (0x58..0x5b).
class Protocol {
 public:
  static constexpr size_t MAX_PAYLOAD = 256;
  static constexpr size_t MAX_RAW = 8 + MAX_PAYLOAD + 2;
  static constexpr size_t MAX_ENCODED = MAX_RAW + MAX_RAW / 254 + 1;
  static constexpr uint32_t FRAME_TIMEOUT_MS = 500;
  using Reset = void (*)();
  using WriteYM = void (*)(uint8_t, uint8_t);
  using Audio = size_t (*)(uint8_t, const uint8_t*, size_t, uint8_t*);
  using SetClock = void (*)(uint8_t, uint32_t);
  using SetVolume = void (*)(uint8_t);

  void configure(Reset reset, WriteYM write, const char* firmware, SetClock clock = nullptr,
                 Audio audio = nullptr, SetVolume volume = nullptr);
  // After transport data loss, ignore bytes until a delimiter arrives.
  void clear(bool resync = false);
  void expire(uint32_t now);
  // Caller must drain the pending response before feeding another byte.
  void feed(uint8_t byte, uint32_t now);
  const uint8_t* output() const { return tx_ + txOffset_; }
  size_t outputSize() const { return txSize_ - txOffset_; }
  void consumeOutput(size_t count);

 private:
  void dispatch(size_t size);
  void reply(uint8_t opcode, uint16_t request, size_t payloadSize);
  static uint16_t crc(const uint8_t* data, size_t size);
  Reset reset_ = nullptr;
  WriteYM write_ = nullptr;
  SetClock clock_ = nullptr;
  Audio audio_ = nullptr;
  SetVolume volume_ = nullptr;
  const char* firmware_ = "";
  uint8_t encoded_[MAX_ENCODED];
  uint8_t raw_[MAX_RAW];
  uint8_t tx_[MAX_ENCODED + 2];  // Leading and trailing delimiters.
  size_t rxSize_ = 0;
  size_t txSize_ = 0;
  size_t txOffset_ = 0;
  uint32_t lastByteMs_ = 0;
  bool discard_ = false;
};
}  // namespace ndsif
#endif
