#ifndef UTIL_H
#define UTIL_H

#include <Arduino.h>

#include <vector>

typedef enum {
  OKIM6258_DIV_1024 = 1024,
  OKIM6258_DIV_768 = 768,
  OKIM6258_DIV_512 = 512
} tOKIM6258Divider;

//------------------------------------------------------------
// リングバッファ
class ByteQueue {
 public:
  ByteQueue(size_t size) : buffer(size), head(0), tail(0), count(0) {
    mux = portMUX_INITIALIZER_UNLOCKED;
  }

  bool push(u8_t value) {
    portENTER_CRITICAL_SAFE(&mux);
    if (count >= buffer.size()) {
      portEXIT_CRITICAL_SAFE(&mux);
      return false;
    }
    buffer[tail] = value;
    tail = (tail + 1) % buffer.size();
    count++;
    portEXIT_CRITICAL_SAFE(&mux);
    return true;
  }

  bool pop(u8_t& value) {
    portENTER_CRITICAL_SAFE(&mux);
    if (count == 0) {
      portEXIT_CRITICAL_SAFE(&mux);
      return false;
    }
    value = buffer[head];
    head = (head + 1) % buffer.size();
    count--;
    portEXIT_CRITICAL_SAFE(&mux);
    return true;
  }

  bool isEmpty() const {
    return size() == 0;
  }
  bool isFull() const {
    return size() == buffer.size();
  }
  size_t size() const {
    portENTER_CRITICAL_SAFE((portMUX_TYPE*)&mux);
    size_t c = count;
    portEXIT_CRITICAL_SAFE((portMUX_TYPE*)&mux);
    return c;
  }

  void clear() {
    portENTER_CRITICAL_SAFE(&mux);
    head = tail = count = 0;
    portEXIT_CRITICAL_SAFE(&mux);
  }

 private:
  std::vector<u8_t> buffer;
  size_t head, tail, count;

  mutable portMUX_TYPE mux;
};

//----------------------------------------------------
s64_t micros64();

#endif