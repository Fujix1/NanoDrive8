#pragma once
#include <atomic>
#include <stdint.h>
#include <stddef.h>

// Display-only SPSC queue. No producer waits; only already-played bytes are decoded.
class SerialMeter {
 public:
  static constexpr unsigned QueueSize = 128;
  void reset() {
    unsigned next = (generation.load() + 1) & 0xffff;
    generation.store(next ? next : 1);
  }
  unsigned generationToken() const { return generation.load(); }
  bool suspended() const { return disabledGeneration.load() == generation.load(); }
  void enqueue(uint32_t position, const uint8_t* data, size_t size) {
    const unsigned gen = generation.load();
    if (disabledGeneration.load() == gen) return;
    const unsigned h = head.load(std::memory_order_relaxed);
    if (!size || size > 252 || h - tail.load(std::memory_order_acquire) == QueueSize) {
      disabledGeneration.store(gen); return;
    }
    Packet& p = packets[h % QueueSize];
    p.generation = gen; p.position = position; p.size = size;
    for (size_t i = 0; i < size; ++i) p.data[i] = data[i];
    head.store(h + 1, std::memory_order_release);
  }
  // Bounded slice, called by the low-priority task. Never accesses the audio ring.
  void process(uint32_t consumed, bool active, unsigned gen) {
    if (gen != generation.load()) return;
    if (decoderGeneration != gen) {
      decoderGeneration = gen; signal = -2; stepIndex = 0; offset = 0; decoded = 0;
    }
    unsigned peak = 0, work = 0, skipped = 0;
    while (work < 252 && skipped < QueueSize) {
      const unsigned t = tail.load(std::memory_order_relaxed);
      if (t == head.load(std::memory_order_acquire)) break;
      const Packet& p = packets[t % QueueSize];
      if (p.generation != gen || suspended()) {
        offset = 0; ++skipped;
        tail.store(t + 1, std::memory_order_release); continue;
      }
      if (!active || p.position + offset >= consumed) break;
      if (p.position + offset != decoded) { disabledGeneration.store(gen); break; }
      const uint8_t b = p.data[offset++];
      const int a = decode(b & 15), c = decode(b >> 4);
      const unsigned aa = a < 0 ? -a : a, ac = c < 0 ? -c : c;
      if (aa > peak) peak = aa;
      if (ac > peak) peak = ac;
      ++decoded; ++work;
      if (offset == p.size) {
        offset = 0; tail.store(t + 1, std::memory_order_release);
      }
    }
    if (!work || generation.load() != gen || suspended()) return;
    unsigned old = published.load(), value;
    do {
      const unsigned previous = (old >> 16) == gen ? old & 0xffff : 0;
      value = (gen << 16) | (previous > peak ? previous : peak);
    } while (!published.compare_exchange_weak(old, value));
  }
  unsigned takePeak(bool active) {
    const unsigned value = published.exchange(0);
    return active && !suspended() && (value >> 16) == generation.load() ? value & 0xffff : 0;
  }
 private:
  struct Packet { uint32_t position; unsigned generation; size_t size; uint8_t data[252]; };
  Packet packets[QueueSize]{};
  std::atomic<unsigned> head{0}, tail{0}, generation{1}, disabledGeneration{0}, published{0};
  unsigned decoderGeneration = 0, offset = 0;
  uint32_t decoded = 0;
  int signal = -2, stepIndex = 0;
  int decode(unsigned nibble) {
    // OKI 12-bit predictor from the existing MAME-derived codec (BSD-3-Clause,
    // copyright Barry Rodewald); see src/okim6258.cpp and license.txt.
    static const int steps[49] = {
      16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,
      157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,
      1060,1166,1282,1411,1552};
    static const int shifts[8] = {-1,-1,-1,-1,2,4,6,8};
    const int step = steps[stepIndex];
    int diff = step >> 3;
    if (nibble & 1) diff += step >> 2;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 4) diff += step;
    signal += (nibble & 8) ? -diff : diff;
    if (signal > 2047) signal = 2047;
    if (signal < -2048) signal = -2048;
    stepIndex += shifts[nibble & 7];
    if (stepIndex < 0) stepIndex = 0;
    if (stepIndex > 48) stepIndex = 48;
    return signal;
  }
};
static_assert(ATOMIC_INT_LOCK_FREE == 2, "Meter queue requires lock-free atomics");
