#include "ndsif.h"

namespace ndsif {
void Protocol::configure(Reset reset, WriteYM write, const char* firmware, SetClock clock) {
  reset_ = reset;
  write_ = write;
  clock_ = clock;
  firmware_ = firmware;
  clear();
}

void Protocol::clear(bool resync) {
  rxSize_ = txSize_ = txOffset_ = 0;
  lastByteMs_ = 0;
  discard_ = resync;
}

void Protocol::expire(uint32_t now) {
  if (rxSize_ != 0 && static_cast<uint32_t>(now - lastByteMs_) >= FRAME_TIMEOUT_MS) {
    rxSize_ = 0;
    discard_ = true;
  }
}

uint16_t Protocol::crc(const uint8_t* data, size_t size) {
  uint16_t value = 0xffff;
  for (size_t i = 0; i < size; ++i) {
    value ^= static_cast<uint16_t>(data[i]) << 8;
    for (unsigned bit = 0; bit < 8; ++bit)
      value = (value & 0x8000) ? (value << 1) ^ 0x1021 : value << 1;
  }
  return value;
}

void Protocol::feed(uint8_t byte, uint32_t now) {
  if (outputSize() != 0) return;
  expire(now);
  lastByteMs_ = now;
  if (byte != 0) {
    if (discard_) return;
    if (rxSize_ == MAX_ENCODED) {
      rxSize_ = 0;
      discard_ = true;
    } else {
      encoded_[rxSize_++] = byte;
    }
    return;
  }
  if (discard_) {
    discard_ = false;
    rxSize_ = 0;
    return;
  }
  const size_t size = rxSize_;
  rxSize_ = 0;
  if (size == 0) return;
  size_t src = 0, dst = 0;
  while (src < size) {
    const uint8_t code = encoded_[src++];
    const size_t count = code - 1;
    if (count > size - src || count > MAX_RAW - dst) return;
    for (size_t i = 0; i < count; ++i) raw_[dst++] = encoded_[src++];
    if (code != 0xff && src < size) {
      if (dst == MAX_RAW) return;
      raw_[dst++] = 0;
    }
  }
  dispatch(dst);
}

void Protocol::dispatch(size_t size) {
  if (size < 10 || raw_[0] != 'N' || raw_[1] != 'D' || raw_[2] != 1) return;
  const size_t length = raw_[6] | (static_cast<size_t>(raw_[7]) << 8);
  if (length > MAX_PAYLOAD || size != 10 + length) return;
  const uint16_t checksum = raw_[size - 2] | (static_cast<uint16_t>(raw_[size - 1]) << 8);
  if (crc(raw_, size - 2) != checksum) return;
  const uint8_t opcode = raw_[3];
  if (opcode & 0x80) return;  // Never reply to a reply.
  const uint16_t request = raw_[4] | (static_cast<uint16_t>(raw_[5]) << 8);
  size_t responseLength = 1;
  uint8_t status = 1;  // Rejected command/arguments; no error detail.
  switch (opcode) {
    case 0x00:  // RESET: completion only, no hardware failure detection.
      if (length == 0) {
        reset_();
        status = 0;
      }
      break;
    case 0x01:  // PING, echo up to 32 bytes.
      if (length <= 32) {
        for (size_t i = length; i != 0; --i) raw_[8 + i] = raw_[7 + i];
        responseLength += length;
        status = 0;
      }
      break;
    case 0x02:  // INFO: status, model length/string, firmware length/string.
      if (length == 0) {
        const char model[] = "NanoDrive 8";
        size_t pos = 9;
        raw_[pos++] = sizeof(model) - 1;
        for (size_t i = 0; i < sizeof(model) - 1; ++i) raw_[pos++] = model[i];
        const size_t lengthPos = pos++;
        size_t count = 0;
        while (firmware_[count] != 0 && count < 63) raw_[pos++] = firmware_[count++];
        raw_[lengthPos] = static_cast<uint8_t>(count);
        responseLength = pos - 8;
        status = 0;
      }
      break;
    case 0x54:  // YM2151 address/value pairs, with completion reply.
    case 0x56:  // One-way YM2151 playback burst, no reply (even on rejection).
      if (length >= 2 && (length & 1) == 0) {
        for (size_t i = 0; i < length; i += 2) write_(raw_[8 + i], raw_[9 + i]);
        status = 0;
      }
      break;
    case 0x57:  // SET_CHIP_CLOCK: chip ID + Hz (u32 LE), always one-way.
      if (length == 5 && clock_ != nullptr) {
        const uint32_t hz = static_cast<uint32_t>(raw_[9]) |
            (static_cast<uint32_t>(raw_[10]) << 8) |
            (static_cast<uint32_t>(raw_[11]) << 16) |
            (static_cast<uint32_t>(raw_[12]) << 24);
        if (hz != 0) clock_(raw_[8], hz);
      }
      return;
    default:
      break;
  }
  if (opcode == 0x56) return;
  raw_[8] = status;
  reply(opcode | 0x80, request, responseLength);
}

void Protocol::reply(uint8_t opcode, uint16_t request, size_t payloadSize) {
  raw_[0] = 'N'; raw_[1] = 'D'; raw_[2] = 1; raw_[3] = opcode;
  raw_[4] = static_cast<uint8_t>(request); raw_[5] = request >> 8;
  raw_[6] = static_cast<uint8_t>(payloadSize); raw_[7] = payloadSize >> 8;
  const size_t size = 8 + payloadSize;
  const uint16_t checksum = crc(raw_, size);
  raw_[size] = static_cast<uint8_t>(checksum); raw_[size + 1] = checksum >> 8;
  txOffset_ = 0;
  tx_[0] = 0;  // Separate boot logs / stale partial replies from this frame.
  size_t dst = 2, codePos = 1;
  uint8_t code = 1;
  for (size_t i = 0; i < size + 2; ++i) {
    if (raw_[i] == 0) {
      tx_[codePos] = code;
      codePos = dst++;
      code = 1;
    } else {
      tx_[dst++] = raw_[i];
      if (++code == 0xff) {
        tx_[codePos] = code;
        codePos = dst++;
        code = 1;
      }
    }
  }
  tx_[codePos] = code;
  tx_[dst++] = 0;
  txSize_ = dst;
}

void Protocol::consumeOutput(size_t count) {
  if (count > outputSize()) count = outputSize();
  txOffset_ += count;
  if (txOffset_ == txSize_) txOffset_ = txSize_ = 0;
}
}  // namespace ndsif
