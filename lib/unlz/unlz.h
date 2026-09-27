#ifndef UNLZ_H
#define UNLZ_H

#include <stddef.h>
#include <stdint.h>

namespace unlz {

enum class Result {
  Ok,
  InvalidArgument,
  MarkerNotFound,
  InputOverrun,
  OutputOverrun,
  InvalidBackReference,
};

struct DecodeResult {
  Result result;
  size_t bytesWritten;
  size_t bytesRead;
};

constexpr size_t kStreamMarkerSize = 4;

const char* resultText(Result result);
bool looksLikeContainer(const uint8_t* src, size_t srcSize);
bool findStreamOffset(const uint8_t* src, size_t srcSize, size_t searchStart, size_t* streamOffset);
DecodeResult decode(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstCapacity);

}  // namespace unlz

#endif  // UNLZ_H
