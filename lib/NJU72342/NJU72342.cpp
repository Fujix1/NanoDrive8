#include "NJU72342.h"

#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "../../include/config.h"

#define FADEOUT_STEPS 62

/**
 * 設定 db: 0 -95
 * 96: ミュート
 * NJU72342: ch1/ch2 は OKIM6258 PAN、ch3/ch4 はメイン出力
 *
 *
 */

// Aカーブの音量マップ
static const u8_t NJU72342_db[FADEOUT_STEPS] = {0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,  1,  2,
                                                2,  2,  2,  3,  3,  4,  5,  5,  5,  6,  7,  8,  8,  10, 11, 12,
                                                13, 14, 16, 17, 18, 19, 21, 23, 24, 26, 27, 29, 31, 32, 35, 37,
                                                40, 42, 45, 48, 51, 55, 59, 63, 68, 73, 78, 83, 90, 96};

// MDX TL fade equivalent: 1 TL step is 0.75 dB, max 62 steps.
static const u8_t NJU72342_mdx_db[FADEOUT_STEPS] = {1,  2,  2,  3,  4,  5,  5,  6,  7,  8,  8,  9,  10, 11, 11, 12,
                                                    13, 14, 14, 15, 16, 17, 17, 18, 19, 20, 20, 21, 22, 23, 23, 24,
                                                    25, 26, 26, 27, 28, 29, 29, 30, 31, 32, 32, 33, 34, 35, 35, 36,
                                                    37, 38, 38, 39, 40, 41, 41, 42, 43, 44, 44, 45, 46, 47};

static TimerHandle_t hFadeOutTimer;  // フェードアウト用タイマー
static u32_t _fadeOutStartMS;
static byte _fadeOutStep;
static const u8_t* _activeFadeTable = NJU72342_db;
static SemaphoreHandle_t nju72342_i2c_mutex = nullptr;
static constexpr u32_t NJU72342_I2C_MUTEX_TIMEOUT_MS = 2;

static void initI2cMutex() {
  if (nju72342_i2c_mutex == nullptr) {
    nju72342_i2c_mutex = xSemaphoreCreateMutex();
  }
}

static void fadeOutTimerHandler(void* param) {
  if (nju72342.fadeOutStatus != FADEOUT_PROCESSING) {
    return;
  }

  nju72342.setAVolume(_fadeOutStep++);

  if (_fadeOutStep == FADEOUT_STEPS) {
    xTimerStop(hFadeOutTimer, 0);
    nju72342.fadeOutStatus = FADEOUT_COMPLETED;
    _fadeOutStep = 0;
    _activeFadeTable = NJU72342_db;
  }
}

void NJU72342::init(int SDA, int SCL, int CLOCK, u16_t fadeOutDuration, bool NJU72342) {
  Wire.begin(SDA, SCL, CLOCK);

  initI2cMutex();

  if (NJU72342) {
    _slaveAddress = NJU72342_ADDR;
    _isNJU72342 = true;
  } else {
    _slaveAddress = NJU72342_ADDR;
    _isNJU72342 = false;
  }
  _attenuation = 0;  // 音量調整値

  fadeOutStatus = FADEOUT_BEFORE;  // フェードアウトしてない
  _isFadeoutEnabled = (fadeOutDuration != FO_0);

  mute();                  // メイン(3ch+4ch)ミュート
  panMute();               // OKIM6258 PAN(ch1+ch2)ミュート
  setInputGain(1, GAIN0);  // 入力ゲイン設定
  setInputGain(2, GAIN0);
  setInputGain(3, GAIN9);
  setInputGain(4, GAIN9);

  // タイマー生成
  if (fadeOutDuration == FO_0) {
    _fadeOutDuration = 8000;
  } else {
    _fadeOutDuration = fadeOutDuration;
  }

  hFadeOutTimer = xTimerCreate("FO_TIMER", _fadeOutDuration / FADEOUT_STEPS, pdTRUE, NULL, fadeOutTimerHandler);
}

// temp = true:  一時的な長さ
void NJU72342::setFadeoutDuration(u16_t length, bool temp) {
  if (length == FO_0) {
    _isFadeoutEnabled = false;
  } else {
    if (length != _fadeOutDuration) {
      _isFadeoutEnabled = true;
      if (temp) {
        xTimerChangePeriod(hFadeOutTimer, length / FADEOUT_STEPS, 0);
      } else {
        _fadeOutDuration = length;
        xTimerChangePeriod(hFadeOutTimer, _fadeOutDuration / FADEOUT_STEPS, 0);
      }
    }
  }
}

void NJU72342::startFadeout(u16_t length) {
  // フェードアウトは 3ch 4ch のみ
  // すでに処理中のときはなにもしない。フェード時間よりループが短い場合
  if (fadeOutStatus == FADEOUT_PROCESSING) {
    return;
  }

  if (length > 0) {
    setFadeoutDuration(length, true);  // 一時的なフェードアウト時間設定
  } else {
    setFadeoutDuration(_fadeOutDuration);  // 設定の値
  }

  _fadeOutStartMS = millis();
  _fadeOutStep = 0;
  _activeFadeTable = NJU72342_db;
  if (_isFadeoutEnabled) {
    fadeOutStatus = FADEOUT_PROCESSING;
    xTimerStart(hFadeOutTimer, 0);
  } else {
    fadeOutStatus = FADEOUT_COMPLETED;
  }
}

void NJU72342::startMdxFadeout(u16_t length) {
  if (fadeOutStatus == FADEOUT_PROCESSING) {
    return;
  }

  if (length == 0) {
    length = 1;
  }
  u16_t period = length / FADEOUT_STEPS;
  if (period == 0) {
    period = 1;
  }

  _fadeOutStartMS = millis();
  _fadeOutStep = 0;
  _activeFadeTable = NJU72342_mdx_db;
  fadeOutStatus = FADEOUT_PROCESSING;
  xTimerChangePeriod(hFadeOutTimer, period, 0);
  xTimerStart(hFadeOutTimer, 0);
}

// att 減衰量dB:
void NJU72342::reset(u8_t att) {
  _attenuation = att;
  fadeOutStatus = FADEOUT_BEFORE;
  resetFadeout();
}

void NJU72342::resetFadeout() {
  xTimerStop(hFadeOutTimer, 0);
  fadeOutStatus = FADEOUT_BEFORE;
  _fadeOutStep = 0;
  _activeFadeTable = NJU72342_db;
}

void NJU72342::setInputGain(u8_t ch, tNJU7234X_GAIN newInputGain) {
  u8_t shift = (ch - 1) * 2;
  _currentGain &= ~(0b11 << shift);                  // 指定chの2bitをクリア
  _currentGain |= ((newInputGain & 0b11) << shift);  // 指定chに新値セット

  if (xSemaphoreTake(nju72342_i2c_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    Wire.beginTransmission(_slaveAddress);
    Wire.write(0x00);
    Wire.write(_currentGain);
    Wire.endTransmission();
    xSemaphoreGive(nju72342_i2c_mutex);
  }
}

// メイン出力(ch3/ch4)の音量設定
// 0:最大音量, 96: ミュート
void NJU72342::setVolumeAll(u8_t newGain) { setVolume_3B_4B(newGain + _attenuation); }

void NJU72342::setMainAttenuation(u8_t att) {
  _attenuation = (att > 96) ? 96 : att;
  if (!_isMuted) setVolume_3B_4B(_attenuation);
}

void NJU72342::setVolume_3B_4B(u8_t newGain) {
  if (_currentVolume[2] == newGain) {
    return;  // 変更なしのとき
  }
  u8_t bit = 119 - newGain;

  if (xSemaphoreTake(nju72342_i2c_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    Wire.beginTransmission(_slaveAddress);
    Wire.write(0x03);
    Wire.write(bit);
    Wire.write(bit);
    Wire.endTransmission();
    xSemaphoreGive(nju72342_i2c_mutex);
  }

  _currentVolume[2] = newGain;  // 3ch
  _currentVolume[3] = newGain;  // 4ch
}

void NJU72342::setVolume_3B(u8_t newGain) {
  if (_currentVolume[2] == newGain) {
    return;  // 変更なしのとき
  }
  u8_t bit = 119 - newGain;

  if (xSemaphoreTake(nju72342_i2c_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    Wire.beginTransmission(_slaveAddress);
    Wire.write(0x03);
    Wire.write(bit);
    Wire.endTransmission();
    xSemaphoreGive(nju72342_i2c_mutex);
  }

  _currentVolume[2] = newGain;  // 3ch
}

void NJU72342::setVolume_4B(u8_t newGain) {
  if (_currentVolume[3] == newGain) {
    return;  // 変更なしのとき
  }
  u8_t bit = 119 - newGain;

  if (xSemaphoreTake(nju72342_i2c_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    Wire.beginTransmission(_slaveAddress);
    Wire.write(0x04);
    Wire.write(bit);
    Wire.endTransmission();
    xSemaphoreGive(nju72342_i2c_mutex);
  }

  _currentVolume[3] = newGain;  // 4ch
}

void NJU72342::mute() {
  // Serial.printf("NJU72342::mute();\n");
  setVolume_3B_4B(96);
  _isMuted = true;
}

void NJU72342::unmute() {
  // Serial.printf("NJU72342::unmute();\n");
  setVolume_3B_4B(_attenuation);
  _isMuted = false;
}

void NJU72342::panReset(u8_t att) {
  _panAttenuation = att;
  panSetPan(PAN_CENTER);
}

void NJU72342::setPanAttenuation(u8_t att) {
  _panAttenuation = (att > 96) ? 96 : att;
  if (!_isPanMuted) {
    applyPanVolume();
  }
}

void NJU72342::panMute() {
  setPanVolume(96, 96);
  _isPanMuted = true;
}

void NJU72342::panUnmute() {
  _isPanMuted = false;
  applyPanVolume();
}

void NJU72342::panSetPan(tPan pan) {
  if (ndConfig.get(CFG_M6258_PAN) == TPAN_INVERT) {
    if (pan == PAN_LEFT) {
      pan = PAN_RIGHT;
    } else if (pan == PAN_RIGHT) {
      pan = PAN_LEFT;
    }
  }

  _pan = pan;
  if (!_isPanMuted) {
    applyPanVolume();
  }
}

void NJU72342::applyPanVolume() {
  u8_t newGain1 = _panAttenuation;
  u8_t newGain2 = _panAttenuation;

  switch (_pan) {
    case PAN_CENTER:
      break;
    case PAN_LEFT:
      newGain2 = 96;
      break;
    case PAN_RIGHT:
      newGain1 = 96;
      break;
    case PAN_MUTE:
      newGain1 = 96;
      newGain2 = 96;
      break;
  }

  setPanVolume(newGain1, newGain2);
}

void NJU72342::setPanVolume(u8_t gain1, u8_t gain2) {
  if (gain1 > 96) {
    gain1 = 96;
  }
  if (gain2 > 96) {
    gain2 = 96;
  }

  if (_currentVolume[0] == gain1 && _currentVolume[1] == gain2) {
    return;
  }

  u8_t bit1 = 119 - gain1;
  u8_t bit2 = 119 - gain2;

  if (xSemaphoreTake(nju72342_i2c_mutex, pdMS_TO_TICKS(NJU72342_I2C_MUTEX_TIMEOUT_MS)) == pdTRUE) {
    Wire.beginTransmission(_slaveAddress);
    if (_currentVolume[0] != gain1 && _currentVolume[1] != gain2) {
      Wire.write(0x01);
      Wire.write(bit1);
      Wire.write(bit2);
    } else if (_currentVolume[0] != gain1) {
      Wire.write(0x01);
      Wire.write(bit1);
    } else {
      Wire.write(0x02);
      Wire.write(bit2);
    }
    u8_t error = Wire.endTransmission();
    xSemaphoreGive(nju72342_i2c_mutex);
    if (error == 0) {
      _currentVolume[0] = gain1;
      _currentVolume[1] = gain2;
    }
  }
}

void NJU72342::setAVolume(u8_t step) {
  if (step > FADEOUT_STEPS - 1) {
    step = FADEOUT_STEPS - 1;
  }
  setVolume_3B_4B(_activeFadeTable[step] + _attenuation);
}

NJU72342 nju72342 = NJU72342();
