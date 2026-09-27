#ifndef LEDS_H
#define LEDS_H
#include <Arduino.h>

class Adafruit_AW9523;

class LedClass {
 public:
  bool init();
  void startupTest();
  bool set(u8_t led, u8_t brightness);
  void setAll(u8_t brightness);
  bool ready() const;

 private:
  static constexpr u8_t ADDR = 0x5B;
  static constexpr u8_t PIN_COUNT = 16;
  Adafruit_AW9523* _aw9523 = nullptr;
  bool _ready = false;
};

extern LedClass Leds;

#endif
