/*
 *  NJU72342 I2C Volume Controller
 */

#ifndef __NJU72342_H
#define __NJU72342_H

#include <Arduino.h>

#define NJU72342_ADDR 0x40  // I2C address

typedef enum { PAN_CENTER = 0x00, PAN_LEFT = 0x01, PAN_RIGHT = 0x02, PAN_MUTE = 0x03 } tPan;
// パン 0x00 = 両方ON, 0x01 = 左ON, 0x02 = 右ON, 0x03 = 両方オフ

typedef enum { GAIN0 = 0b0000, GAIN3 = 0b0101, GAIN6 = 0b1010, GAIN9 = 0b1111 } tNJU7234X_GAIN;

typedef enum {
  FADEOUT_BEFORE,      // フェードアウト前
  FADEOUT_PROCESSING,  // フェードアウト処理中
  FADEOUT_COMPLETED,   // フェードアウト完了
} tFadeOutStatus;

class NJU72342 {
 public:
  tFadeOutStatus fadeOutStatus = FADEOUT_BEFORE;
  void init(int SDA, int SCL, int CLOCK, u16_t fadeOutDuration, bool NJU72342);
  void reset(u8_t att);
  void setInputGain(u8_t ch, tNJU7234X_GAIN newInputGain);
  void setVolume_3B_4B(u8_t newGain);  // ch3と4の音量設定
  void setVolume_3B(u8_t newGain);     // ch3の音量設定
  void setVolume_4B(u8_t newGain);     // ch4の音量設定
  void setVolumeAll(u8_t newGain);     // メイン出力(ch3/ch4)の音量設定
  void setAVolume(u8_t step);
  void mute();
  void unmute();
  void panReset(u8_t att = 0);
  void setPanAttenuation(u8_t att);
  void panMute();
  void panUnmute();
  void panSetPan(tPan pan);
  void startFadeout(u16_t length = 0);  // ms
  void startMdxFadeout(u16_t length);    // ms
  void setFadeoutDuration(u16_t fadeOutDuration, bool temp = false);
  void resetFadeout();

 private:
  void setPanVolume(u8_t gain1, u8_t gain2);
  void applyPanVolume();

  bool _isMuted = false;
  bool _isPanMuted = false;
  u8_t _currentVolume[4] = {100, 100, 100, 100};  // 各チャンネルの音量
  u8_t _attenuation = 0;                          // 音量調整値
  u8_t _panAttenuation = 0;
  tPan _pan = PAN_CENTER;
  u32_t _fadeoutStarted;                          // フェードアウト開始時間
  bool _isFadeoutEnabled;                         // フェードアウトするか
  u16_t _fadeOutDuration;
  u8_t _slaveAddress;
  bool _isNJU72342;
  u8_t _currentGain;
};

extern NJU72342 nju72342;

#endif
