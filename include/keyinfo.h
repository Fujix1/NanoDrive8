// キーボード表示

#ifndef KEYINFO_H
#define KEYINFO_H

#include <Arduino.h>

#include <vector>

#include "NJU72342.h"

#define MAX_CHANNELS 18  // 最大ch数

// 音階情報
struct NoteInfo {
  int octave;  // 1〜8, これ以外はキーオフ扱い
  int note;    // 0=C, 1=C#, 2=D, 3=D#, 4=E, ..., 9=A, 10=A#, 11=B
};

// キーボード表示デバイス定義
typedef enum {
  YM2151,
  OKIM6258_KEY,
  DEVICE_COUNT  // デバイスの総数
} t_device;

// デバイスごとのチャンネル数
static const int device_channels[DEVICE_COUNT] = {[YM2151] = 8, [OKIM6258_KEY] = 8};

class keyboard {
 public:
  SemaphoreHandle_t keyinfoMutex;
  keyboard();
  void reset();
  void set(t_device device, u8_t ch, NoteInfo ni);

  struct NoteInfo keyInfo[DEVICE_COUNT][MAX_CHANNELS];
  u8_t ym2151ChannelMask = 0;
  tPan trackPan[16];
  bool trackKeyOn[16];
  u8_t trackLevel[16];  // 0-15
  u8_t trackTone[16];   // 0-96, 0xff=未定義

 private:
};

extern keyboard KeyBoard;

#endif
