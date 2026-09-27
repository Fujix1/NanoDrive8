#include "unlz.h"

#include <string.h>

// Decompression core based on the token model of Fabrice Bellard's
// MIT-licensed original LZEXE source code, with X68000 stream-layout
// differences verified from compressed/decoded MDX and PDX samples.

namespace unlz {
namespace {

constexpr uint8_t STREAM_MARKER[kStreamMarkerSize] = {0x7f, 0xff, 0xff, 0x4c};
constexpr uint8_t LZX_HEADER_PREFIX[] = {'L', 'Z', 'X', ' ', '0', '.'};
constexpr size_t STREAM_SCAN_START = 0x24;

struct BitReader {
  const uint8_t* src;
  size_t srcSize;
  size_t pos;
  uint8_t bitsLeft;
  uint8_t current;

  Result readByte(uint8_t* value) {
    if (pos >= srcSize) {
      return Result::InputOverrun;
    }
    *value = src[pos++];
    return Result::Ok;
  }

  Result readBit(uint8_t* bit) {
    if (bitsLeft == 0) {
      Result result = readByte(&current);
      if (result != Result::Ok) {
        return result;
      }
      bitsLeft = 8;
    }

    *bit = (current & 0x80) ? 1 : 0;
    current = (uint8_t)(current << 1);
    bitsLeft--;
    return Result::Ok;
  }
};

int32_t arithmeticShiftRight3(uint32_t value) {
  if ((value & 0x80000000u) == 0) {
    return (int32_t)(value >> 3);
  }

  const uint32_t magnitude = (~value) + 1;
  return -(int32_t)((magnitude + 7) >> 3);
}

Result copyFromHistory(uint8_t* dst, size_t dstCapacity, size_t* outPos, int32_t offset, size_t length) {
  if (offset >= 0) {
    return Result::InvalidBackReference;
  }

  const size_t distance = (size_t)(-offset);
  if (distance == 0 || distance > *outPos) {
    return Result::InvalidBackReference;
  }
  if (length > dstCapacity - *outPos) {
    return Result::OutputOverrun;
  }

  size_t srcPos = *outPos - distance;
  for (size_t i = 0; i < length; i++) {
    dst[*outPos] = dst[srcPos++];
    (*outPos)++;
  }

  return Result::Ok;
}

bool contains(const uint8_t* src, size_t srcSize, const uint8_t* needle, size_t needleSize) {
  if (!src || !needle || needleSize == 0 || srcSize < needleSize) {
    return false;
  }

  for (size_t i = 0; i + needleSize <= srcSize; i++) {
    if (memcmp(src + i, needle, needleSize) == 0) {
      return true;
    }
  }

  return false;
}

DecodeResult makeResult(Result result, size_t bytesWritten, const BitReader& reader) {
  return {result, bytesWritten, reader.pos};
}

}  // namespace

const char* resultText(Result result) {
  switch (result) {
    case Result::Ok:
      return "ok";
    case Result::InvalidArgument:
      return "invalid argument";
    case Result::MarkerNotFound:
      return "compressed stream marker not found";
    case Result::InputOverrun:
      return "input overrun";
    case Result::OutputOverrun:
      return "output overrun";
    case Result::InvalidBackReference:
      return "invalid back reference";
  }
  return "unknown";
}

bool looksLikeContainer(const uint8_t* src, size_t srcSize) {
  return contains(src, srcSize, STREAM_MARKER, sizeof(STREAM_MARKER)) ||
         contains(src, srcSize, LZX_HEADER_PREFIX, sizeof(LZX_HEADER_PREFIX));
}

bool findStreamOffset(const uint8_t* src, size_t srcSize, size_t searchStart, size_t* streamOffset) {
  if (streamOffset) {
    *streamOffset = 0;
  }
  if (!src || !streamOffset || srcSize < sizeof(STREAM_MARKER)) {
    return false;
  }

  size_t pos = searchStart + STREAM_SCAN_START;
  while (pos + 2 + sizeof(STREAM_MARKER) <= srcSize) {
    pos += 2;
    if (memcmp(src + pos, STREAM_MARKER, sizeof(STREAM_MARKER)) == 0) {
      *streamOffset = pos + sizeof(STREAM_MARKER);
      return true;
    }
  }

  for (size_t i = searchStart; i + sizeof(STREAM_MARKER) <= srcSize; i++) {
    if (memcmp(src + i, STREAM_MARKER, sizeof(STREAM_MARKER)) == 0) {
      *streamOffset = i + sizeof(STREAM_MARKER);
      return true;
    }
  }

  return false;
}

DecodeResult decode(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstCapacity) {
  BitReader reader = {src, srcSize, 0, 0, 0};
  size_t outPos = 0;

  if (!src || !dst || srcSize == 0 || dstCapacity == 0) {
    return makeResult(Result::InvalidArgument, outPos, reader);
  }

  while (true) {
    uint8_t bit = 0;
    Result result = reader.readBit(&bit);
    if (result != Result::Ok) {
      return makeResult(result, outPos, reader);
    }

    if (bit) {
      if (outPos >= dstCapacity) {
        return makeResult(Result::OutputOverrun, outPos, reader);
      }

      result = reader.readByte(&dst[outPos]);
      if (result != Result::Ok) {
        return makeResult(result, outPos, reader);
      }
      outPos++;
      continue;
    }

    result = reader.readBit(&bit);
    if (result != Result::Ok) {
      return makeResult(result, outPos, reader);
    }

    if (!bit) {
      uint8_t b0 = 0;
      uint8_t b1 = 0;
      uint8_t low = 0;

      result = reader.readBit(&b0);
      if (result != Result::Ok) {
        return makeResult(result, outPos, reader);
      }
      result = reader.readBit(&b1);
      if (result != Result::Ok) {
        return makeResult(result, outPos, reader);
      }
      result = reader.readByte(&low);
      if (result != Result::Ok) {
        return makeResult(result, outPos, reader);
      }

      const size_t length = (size_t)((b0 << 1) | b1) + 2;
      result = copyFromHistory(dst, dstCapacity, &outPos, -256 + (int32_t)low, length);
      if (result != Result::Ok) {
        return makeResult(result, outPos, reader);
      }
      continue;
    }

    uint8_t high = 0;
    uint8_t low = 0;
    result = reader.readByte(&high);
    if (result != Result::Ok) {
      return makeResult(result, outPos, reader);
    }
    result = reader.readByte(&low);
    if (result != Result::Ok) {
      return makeResult(result, outPos, reader);
    }

    const int32_t offset = arithmeticShiftRight3(0xffff0000u | ((uint32_t)high << 8) | low);
    const uint8_t code = low & 0x07;
    if (code) {
      result = copyFromHistory(dst, dstCapacity, &outPos, offset, (size_t)code + 2);
      if (result != Result::Ok) {
        return makeResult(result, outPos, reader);
      }
      continue;
    }

    uint8_t extra = 0;
    result = reader.readByte(&extra);
    if (result != Result::Ok) {
      return makeResult(result, outPos, reader);
    }
    if (extra == 0) {
      return makeResult(Result::Ok, outPos, reader);
    }

    result = copyFromHistory(dst, dstCapacity, &outPos, offset, (size_t)extra + 1);
    if (result != Result::Ok) {
      return makeResult(result, outPos, reader);
    }
  }
}

}  // namespace unlz
