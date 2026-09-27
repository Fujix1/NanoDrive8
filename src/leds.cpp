#include "leds.h"

#include <Adafruit_AW9523.h>
#include <Wire.h>

#include "common.h"
#include "disp.h"

/**
 * ピン番号とピンの対応
 * P0_0..P0_7 = pin 0..7
 * P1_0..P1_7 = pin 8..15
 */
namespace {
constexpr u16_t LED_TEST_DELAY_MS = 32;
constexpr u8_t LED_TEST_HEAD_BRIGHTNESS = 5;
constexpr u8_t LED_TEST_TAIL_BRIGHTNESS = 2;
constexpr u8_t LED_TEST_PINS[] = {0, 1, 2, 3, 4, 5, 6, 7};

constexpr u8_t ledTestPinCount() {
  return sizeof(LED_TEST_PINS) / sizeof(LED_TEST_PINS[0]);
}
}  // namespace

LedClass Leds;

bool LedClass::init() {
  if (_ready) {
    return true;
  }

  if (_aw9523 == nullptr) {
    _aw9523 = new Adafruit_AW9523();
  }

  if (!Wire.begin(I2C_SDA, I2C_SCL, I2C_CLOCK)) {
    Serial.println("AW9523: Wire.begin() failed.");
    lcd.printf("[WARN] AW9523 Wire init failed.\n");
    return false;
  }

  if (!_aw9523->begin(ADDR, &Wire)) {
    Serial.println("AW9523: not found.");
    lcd.printf("[WARN] AW9523 not found.\n");
    return false;
  }

  Serial.println("AW9523: configure all pins for constant-current LED mode.");
  for (u8_t pin = 0; pin < PIN_COUNT; pin++) {
    _aw9523->pinMode(pin, AW9523_LED_MODE);
    _aw9523->analogWrite(pin, 0);
  }

  _ready = true;
  lcd.printf("AW9523 initialized.\n");
  return true;
}

void LedClass::startupTest() {
  if (!_ready) {
    return;
  }

  lcd.printf("AW9523 LED test...\n");

  for (u8_t i = 0; i < ledTestPinCount(); i++) {
    for (u8_t j = 0; j < ledTestPinCount(); j++) {
      _aw9523->analogWrite(LED_TEST_PINS[j], 0);
    }
    if (i > 0) {
      _aw9523->analogWrite(LED_TEST_PINS[i - 1], LED_TEST_TAIL_BRIGHTNESS);
    }
    _aw9523->analogWrite(LED_TEST_PINS[i], LED_TEST_HEAD_BRIGHTNESS);
    delay(LED_TEST_DELAY_MS);
  }
  for (int i = ledTestPinCount() - 2; i >= 0; i--) {
    for (u8_t j = 0; j < ledTestPinCount(); j++) {
      _aw9523->analogWrite(LED_TEST_PINS[j], 0);
    }
    _aw9523->analogWrite(LED_TEST_PINS[i + 1], LED_TEST_TAIL_BRIGHTNESS);
    _aw9523->analogWrite(LED_TEST_PINS[i], LED_TEST_HEAD_BRIGHTNESS);
    delay(LED_TEST_DELAY_MS);
  }
  setAll(LED_TEST_TAIL_BRIGHTNESS);

  Serial.println("AW9523: LED test complete.");
}

bool LedClass::set(u8_t led, u8_t brightness) {
  if (!_ready || led >= PIN_COUNT) {
    return false;
  }

  _aw9523->analogWrite(led, brightness);
  return true;
}

void LedClass::setAll(u8_t brightness) {
  if (!_ready) {
    return;
  }

  for (u8_t pin = 0; pin < PIN_COUNT; pin++) {
    _aw9523->analogWrite(pin, brightness);
  }
}

bool LedClass::ready() const {
  return _ready;
}
