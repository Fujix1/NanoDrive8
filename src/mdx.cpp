/**
 * NanoDrive MDX Parser v1.0
 * (c) 2026 Fujix
 *
 * MDX command definitions from
 * https://w.atwiki.jp/mxdrv/pages/23.html
 *
 * Main algorithm derived from the Portable mdx decoder by Yosshin
 *  https://github.com/yosshin4004/portable_mdx
 *
 * X68k MXDRV music driver version 2.06+17 Rel.X5-S
 *  (c)1988-92 milk.,K.MAEKAWA, Missy.M, Yatsube
 *
 * Converted for Win32 [MXDRVg] V2.00a
 *  Copyright (C) 2000-2002 GORRY.
 *
 **/

#include "mdx.h"

#include "SI5351.hpp"
#include "esp_heap_caps.h"
#include "file.h"
#include "leds.h"
#include "okim6258.h"
#include "sjis.h"

#ifndef MDX_USE_PCM8_SHADOW
#define MDX_USE_PCM8_SHADOW 0
#endif

MDXClass MDX;
extern u64_t lastTick;
extern u64_t excess;

namespace {
constexpr u8_t MDX_BEAT_LED = 11;  // P1_3, P1_1 -> pin 9, P1_2 -> pin 10
constexpr u8_t MDX_BEAT_LED_BRIGHTNESS = 8;
constexpr u16_t MDX_TICKS_PER_BEAT = 48;
constexpr u32_t MDX_BEAT_PULSE_US = 80000;
constexpr u8_t MDX_BEAT_PULSE_TICKS_MAX = 8;
constexpr size_t MDX_PLAYER_PATH_MAX_CHARACTERS = 64;

String truncateUtf8WithEllipsis(const String& text, size_t maxCharacters) {
  const size_t byteLength = text.length();
  size_t byteIndex = 0;
  size_t characterCount = 0;

  while (byteIndex < byteLength && characterCount < maxCharacters) {
    const u8_t lead = (u8_t)text[byteIndex];
    size_t characterBytes = 1;
    if ((lead & 0xe0) == 0xc0) {
      characterBytes = 2;
    } else if ((lead & 0xf0) == 0xe0) {
      characterBytes = 3;
    } else if ((lead & 0xf8) == 0xf0) {
      characterBytes = 4;
    }

    if (byteIndex + characterBytes > byteLength) {
      characterBytes = 1;
    } else {
      for (size_t i = 1; i < characterBytes; ++i) {
        if ((((u8_t)text[byteIndex + i]) & 0xc0) != 0x80) {
          characterBytes = 1;
          break;
        }
      }
    }

    byteIndex += characterBytes;
    characterCount++;
  }

  if (byteIndex >= byteLength) {
    return text;
  }
  return text.substring(0, byteIndex) + "...";
}
}  // namespace

// volatile SemaphoreHandle_t timerSemaphore;
// portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
// hw_timer_t* MDXTimer = NULL;  // ハードウェアタイマ

static const u8_t KEYCODETABLE[] = {
    0x00, 0x01, 0x02, 0x04, 0x05, 0x06, 0x08, 0x09, 0x0a, 0x0c, 0x0d, 0x0e, 0x10, 0x11, 0x12, 0x14,
    0x15, 0x16, 0x18, 0x19, 0x1a, 0x1c, 0x1d, 0x1e, 0x20, 0x21, 0x22, 0x24, 0x25, 0x26, 0x28, 0x29,
    0x2a, 0x2c, 0x2d, 0x2e, 0x30, 0x31, 0x32, 0x34, 0x35, 0x36, 0x38, 0x39, 0x3a, 0x3c, 0x3d, 0x3e,
    0x40, 0x41, 0x42, 0x44, 0x45, 0x46, 0x48, 0x49, 0x4a, 0x4c, 0x4d, 0x4e, 0x50, 0x51, 0x52, 0x54,
    0x55, 0x56, 0x58, 0x59, 0x5a, 0x5c, 0x5d, 0x5e, 0x60, 0x61, 0x62, 0x64, 0x65, 0x66, 0x68, 0x69,
    0x6a, 0x6c, 0x6d, 0x6e, 0x70, 0x71, 0x72, 0x74, 0x75, 0x76, 0x78, 0x79, 0x7a, 0x7c, 0x7d, 0x7e,
};

static const u8_t FMVOLTABLE[16] = {
    0x2a, 0x28, 0x25, 0x22, 0x20, 0x1d, 0x1a, 0x18, 0x15, 0x12, 0x10, 0x0d, 0x0a, 0x08, 0x05, 0x02,
};

static const u8_t PCM8VOLTABLE[16] = {
    2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24, 32, 40, 48, 64, 80,
};

static const u8_t PCM8TLTABLE[43] = {
    15, 15, 15, 14, 14, 14, 13, 13, 13, 12, 12, 11, 11, 11, 10, 10, 10, 9, 9, 8, 8, 8,
    7,  7,  7,  6,  6,  5,  5,  5,  4,  4,  4,  3,  3,  2,  2,  2,  1,  1, 1, 0, 0,
};

static u8_t quantizeAdpcmDisplayLevel(u8_t volume) {
  if (volume < 16) {
    return volume;
  }

  // PCM の @v は 0x80..0xFF で、0x80 が最大、0xFF が最小。
  const u8_t rawLevel = volume & 0x7f;
  const u8_t invertedLevel = (u8_t)(127 - rawLevel);
  return (u8_t)(invertedLevel >> 3);  // 0..127 を 0..15 に量子化
}

static void setTrackDisplayLevel(u8_t trackNo, u8_t level) {
  if (trackNo >= 16) {
    return;
  }
  if (xSemaphoreTake(KeyBoard.keyinfoMutex, 0) == pdTRUE) {
    KeyBoard.trackLevel[trackNo] = level;
    xSemaphoreGive(KeyBoard.keyinfoMutex);
  }
}

static u8_t calcFmDisplayLevel(u8_t trackNo) {
  if (trackNo >= 8) {
    return 0;
  }

  u8_t baseTl = 0;
  const u8_t v = MDX.tracks[trackNo].volume;
  if (v & 0x80) {
    baseTl = v & 0x7f;
  } else {
    baseTl = FMVOLTABLE[v & 0x0f];
  }

  static const u8_t carrierSlots[8] = {0x08, 0x08, 0x08, 0x08, 0x0c, 0x0e, 0x0e, 0x0f};
  const u8_t con = MDX.tracks[trackNo].con_fl & 0x07;
  u8_t minCarrierTl = 127;
  bool hasCarrier = false;

  for (int op = 0; op < 4; op++) {
    if ((carrierSlots[con] & (1 << op)) == 0) {
      continue;
    }
    int finalTl = MDX.tracks[trackNo].tl[op] + baseTl;
    if (finalTl < 0) {
      finalTl = 0;
    } else if (finalTl > 127) {
      finalTl = 127;
    }
    if (!hasCarrier || finalTl < minCarrierTl) {
      minCarrierTl = (u8_t)finalTl;
      hasCarrier = true;
    }
  }

  if (!hasCarrier) {
    return 0;
  }
  return (u8_t)((127 - minCarrierTl) >> 3);
}

struct Pcm8aMode {
  u32_t rateStep;
  u8_t dataKind;
};

static constexpr u8_t PCM8_DATA_ADPCM = 4;
static constexpr u8_t PCM8_DATA_PCM16 = 5;
static constexpr u8_t PCM8_DATA_PCM8 = 6;

// PCM8A 用の入力フォーマットとソースのサンプルレート
// 15.625kHz にダウンサンプリングして出力
static const Pcm8aMode PCM8A_MODE_TABLE[13] = {
    {0x04000, PCM8_DATA_ADPCM},  // F0:  ADPCM,  3.906kHz
    {0x05555, PCM8_DATA_ADPCM},  // F1:  ADPCM,  5.208kHz
    {0x08000, PCM8_DATA_ADPCM},  // F2:  ADPCM,  7.812kHz
    {0x0AAAA, PCM8_DATA_ADPCM},  // F3:  ADPCM, 10.416kHz
    {0x10000, PCM8_DATA_ADPCM},  // F4:  ADPCM, 15.625kHz
    {0x10000, PCM8_DATA_PCM16},  // F5:  16bit PCM, 15.625kHz
    {0x10000, PCM8_DATA_PCM8},   // F6:   8bit PCM, 15.625kHz
    {0x15555, PCM8_DATA_ADPCM},  // F7:  ADPCM, 20.833kHz
    {0x15555, PCM8_DATA_PCM16},  // F8:  16bit PCM, 20.833kHz
    {0x15555, PCM8_DATA_PCM8},   // F9:   8bit PCM, 20.833kHz
    {0x20000, PCM8_DATA_ADPCM},  // F10: ADPCM, 31.250kHz
    {0x20000, PCM8_DATA_PCM16},  // F11: 16bit PCM, 31.250kHz
    {0x20000, PCM8_DATA_PCM8},   // F12:  8bit PCM, 31.250kHz
};

static void writePitch(int ch, u16_t pitchWord) {
  u16_t d2 = pitchWord;
  if (d2 > 0x17ff) {
    if ((s16_t)d2 >= 0) {
      d2 = 0x17ff;
    } else {
      d2 = 0;
    }
  }

  u16_t v = d2 << 2;
  u8_t kf = (u8_t)v;
  u8_t kc = KEYCODETABLE[(v >> 8) & 0x7f];

  FM.setRegisterOPM(0x30 + ch, kf, 0);
  FM.setRegisterOPM(0x28 + ch, kc, 0);
}

static void writePitchAndCache(MDXTrackState& track, int ch, u16_t pitchWord) {
  track.noteDBendPitch = pitchWord;
  writePitch(ch, pitchWord);
}

static void writePitchIfChanged(MDXTrackState& track, int ch, u16_t pitchWord) {
  if (pitchWord == track.noteDBendPitch) return;
  writePitchAndCache(track, ch, pitchWord);
}

// トラックずらし対応
static inline u8_t getFmChannel(const MDXTrackState& track, u8_t fallback) {
  if (track.fmChannel < 8) {
    return track.fmChannel;
  }
  return fallback & 0x07;
}

static u16_t nextLfoRand() {
  static u16_t seed = 0x1234;
  u32_t v = seed;
  v = v * 0xc549 + 0x0c;
  seed = (u16_t)v;
  return (u16_t)(v >> 8);
}

static void resetPitchLFO(MDXTrackState& track) {
  track.pitchLFOLengthCounter = track.pitchLFOLengthCooked;
  track.pitchLFODelta = track.pitchLFODeltaStart;
  track.pitchLFOOffset = track.pitchLFOOffsetStart;
}

static void resetVolumeLFO(MDXTrackState& track) {
  track.volumeLFOLengthCounter = track.volumeLFOLength;
  track.volumeLFODelta = track.volumeLFODeltaStart;
  track.volumeLFOOffset = track.volumeLFODeltaCoocked;
}

static void startLFODelay(MDXTrackState& track) {
  track.LFODelayCounter = track.LFODelay;
  track.pitchLFOOffset = 0;
  track.volumeLFOOffset = 0;
  if (--track.LFODelayCounter == 0) {
    if (track.flagsB3 & 0x20) {
      resetPitchLFO(track);
    }
    if (track.flagsB3 & 0x40) {
      resetVolumeLFO(track);
    }
  }
}

static void resetOpmLFOIfNeeded(const MDXTrackState& track) {
  if (!(track.flagsB3 & 0x02)) return;

  FM.setRegisterOPM(0x01, 0x02, 0);
  FM.setRegisterOPM(0x01, 0x00, 0);
}

static void updatePitchLFO(MDXTrackState& track) {
  if (!(track.flagsB3 & 0x20) || track.pitchLFOType == 0) return;

  switch (track.pitchLFOType) {
    case 1: {  // L0010be 鋸波
      track.pitchLFOOffset = (u32_t)((s32_t)track.pitchLFOOffset + (s32_t)track.pitchLFODelta);
      if (--track.pitchLFOLengthCounter == 0) {
        track.pitchLFOLengthCounter = track.pitchLFOLength;
        track.pitchLFOOffset = (u32_t)(-(s32_t)track.pitchLFOOffset);
      }
      break;
    }
    case 2: {  // L0010d4 矩形波
      track.pitchLFOOffset = track.pitchLFODelta;
      if (--track.pitchLFOLengthCounter == 0) {
        track.pitchLFOLengthCounter = track.pitchLFOLength;
        track.pitchLFODelta = (u32_t)(-(s32_t)track.pitchLFODelta);
      }
      break;
    }
    case 3: {  // L0010ea 三角波
      track.pitchLFOOffset = (u32_t)((s32_t)track.pitchLFOOffset + (s32_t)track.pitchLFODelta);
      if (--track.pitchLFOLengthCounter == 0) {
        track.pitchLFOLengthCounter = track.pitchLFOLength;
        track.pitchLFODelta = (u32_t)(-(s32_t)track.pitchLFODelta);
      }
      break;
    }

    case 4: {  // L001100 乱数
      if (--track.pitchLFOLengthCounter == 0) {
        u16_t rnd = nextLfoRand();
        s32_t val = (s16_t)rnd;
        val = (s32_t)val * (s32_t)track.pitchLFODelta;
        track.pitchLFOOffset = (u32_t)val;
        track.pitchLFOLengthCounter = track.pitchLFOLength;
      }
      break;
    }
  }
}

static void updateVolumeLFO(MDXTrackState& track) {
  if (!(track.flagsB3 & 0x40) || track.volumeLFOType == 0) return;

  switch (track.volumeLFOType) {
    case 1: {  // L001120 鋸波
      track.volumeLFOOffset = track.volumeLFOOffset + track.volumeLFODelta;
      if (--track.volumeLFOLengthCounter == 0) {
        track.volumeLFOLengthCounter = track.volumeLFOLength;
        track.volumeLFOOffset = track.volumeLFODeltaCoocked;
      }
      break;
    }
    case 2: {  // L001138 矩形波
      if (--track.volumeLFOLengthCounter == 0) {
        track.volumeLFOLengthCounter = track.volumeLFOLength;
        track.volumeLFOOffset = track.volumeLFOOffset + track.volumeLFODelta;
        track.volumeLFODelta = (u16_t)(-(s16_t)track.volumeLFODelta);
      }
      break;
    }
    case 3: {  // L00114e 三角波
      track.volumeLFOOffset = track.volumeLFOOffset + track.volumeLFODelta;
      if (--track.volumeLFOLengthCounter == 0) {
        track.volumeLFOLengthCounter = track.volumeLFOLength;
        track.volumeLFODelta = (u16_t)(-(s16_t)track.volumeLFODelta);
      }
      break;
    }
    case 4: {  // L001164 乱数
      if (--track.volumeLFOLengthCounter == 0) {
        u16_t rnd = nextLfoRand();
        s32_t val = (s16_t)rnd;
        val = (s32_t)val * (s16_t)track.volumeLFODelta;
        track.volumeLFOOffset = (u16_t)val;
        track.volumeLFOLengthCounter = track.volumeLFOLength;
      }
      break;
    }
  }
}

static u8_t getPcm8Gain(u8_t vol) {
  if (vol & 0x80) {
    const u8_t tlValue = vol & 0x7f;
    const u8_t level = (tlValue < sizeof(PCM8TLTABLE)) ? PCM8TLTABLE[tlValue] : 0;
    return PCM8VOLTABLE[level];
  }

  return PCM8VOLTABLE[vol & 0x0f];
}

//---------------------------------------------------------------------------

void MDXClass::init() {
  // タイマ割り込み設定
  // MDXTimer = timerBegin(0, 80, true);  // 1MHz
  // timerAttachInterrupt(MDXTimer, &onMDXTick, true);
  // timerAlarmWrite(MDXTimer, 4166, true);  // 240Hz 4166us
}

// タイマー開始
void MDXClass::startTimer() {
  //
  // if (!timerAlarmEnabled(MDXTimer)) {
  //  timerAlarmEnable(MDXTimer);
  //}
}

// タイマー停止
void MDXClass::stopTimer() {
  //
  // if (timerAlarmEnabled(MDXTimer)) {
  //  timerAlarmDisable(MDXTimer);
  //}
}

// テンポ設定
void MDXClass::_setTempo(u8_t tempo) {
  if (tempo == 0) tempo = 1;

  _tempo = tempo;

  // タイマーB周期  TB(ms) = (1024 * (256 - CLKB)) / 4000 (KHz)
  _interval = (1024 * (256 - _tempo)) / 4;
  u32_t pulseTicks = (MDX_BEAT_PULSE_US + (_interval / 2)) / _interval;
  if (pulseTicks == 0) {
    pulseTicks = 1;
  } else if (pulseTicks > MDX_BEAT_PULSE_TICKS_MAX) {
    pulseTicks = MDX_BEAT_PULSE_TICKS_MAX;
  }
  _beatPulseTicks = (u8_t)pulseTicks;
  // Serial.printf("Tempo byte: %d, Interval: %d us\n", _tempo, _interval);
}

u16_t MDXClass::getTempoBpm() const {
  const u32_t beatIntervalUs = 256UL * (256 - _tempo) * MDX_TICKS_PER_BEAT;
  return (u16_t)((60000000UL + beatIntervalUs / 2) / beatIntervalUs);
}

bool MDXClass::hasExplicitTempo() const {
  return _hasExplicitTempo;
}

void MDXClass::_updateBeatLed() {
  if (!Leds.ready()) {
    return;
  }

  if ((_beatTickCounter % MDX_TICKS_PER_BEAT) == 0) {
    Leds.set(MDX_BEAT_LED, MDX_BEAT_LED_BRIGHTNESS);
  } else if ((_beatTickCounter % MDX_TICKS_PER_BEAT) == _beatPulseTicks) {
    Leds.set(MDX_BEAT_LED, 0);
  }
}

// FM のステート適用
void MDXClass::_applyPendingFmState(u8_t trackNo) {
  if (trackNo >= 8) return;

  u8_t fmCh = getFmChannel(tracks[trackNo], trackNo);
  if (tracks[trackNo].flags & 0x02) {
    // MXDRV mirrors voice changes at the next key-on boundary.
    _setVoice(trackNo, tracks[trackNo].voiceNo);
    tracks[trackNo].flags &= (u8_t)~0x02;
  }
  if (tracks[trackNo].flags & 0x04) {
    // パンの適用はグローバルをトラック設定で上書き
    FM.setRegisterOPM(0x20 + fmCh, tracks[trackNo].pan | (tracks[trackNo].con_fl & 0x3F), 0);
    tracks[trackNo].flags &= (u8_t)~0x04;
  }
}

void MDXClass::_setVoice(u8_t trackNo, u8_t voiceNo) {
  if (trackNo >= 8) return;  // FM

  tracks[trackNo].voiceNo = voiceNo;

  u8_t fmCh = getFmChannel(tracks[trackNo], trackNo);
  u8_t internalIdx = voiceTable[voiceNo];
  if (xSemaphoreTake(KeyBoard.keyinfoMutex, 0) == pdTRUE) {
    KeyBoard.trackTone[trackNo] = (internalIdx == 0xff || voiceNo > 96) ? 0xff : voiceNo;
    xSemaphoreGive(KeyBoard.keyinfoMutex);
  }
  if (internalIdx == 0xff) return;  // 未定義

  u32_t addr = voiceDataOffset + (internalIdx * 27) + 1;  // 音色開始位置
  u8_t con_fl = ndFile.get_ui8_at(addr++);                // CON, FL
  u8_t keyonSlot = ndFile.get_ui8_at(addr++);             // キーオンスロットマスク
  tracks[trackNo].con_fl = con_fl;                        // 保持
  static const u8_t carrierSlots[8] = {0x08, 0x08, 0x08, 0x08, 0x0c, 0x0e, 0x0e, 0x0f};
  const u8_t carrierMask = carrierSlots[con_fl & 0x07];
  tracks[trackNo].carrierSlot = carrierMask;
  tracks[trackNo].keyonSlot = (u8_t)(((keyonSlot & 0x0f) << 3) | fmCh);
  tracks[trackNo].flags |= 0x04;

  static const u8_t regs[] = {0x40, 0x60, 0x80, 0xA0, 0xC0, 0xE0};
  static const u8_t opmSlotOffset[] = {0x00, 0x08, 0x10, 0x18};  // M1, C1, M2, C2

  for (int r = 0; r < 6; r++) {
    for (int op = 0; op < 4; op++) {
      u8_t dat = ndFile.get_ui8_at(addr++);
      if (r == 1) {  // 音量調整用 TL を保持
        tracks[trackNo].tl[op] = dat;
        if (carrierMask & (u8_t)(1u << op)) {
          dat = 0x7f;
        }
      }
      FM.setRegisterOPM(regs[r] + opmSlotOffset[op] + fmCh, dat, 0);
      // Serial.printf("0x%x 0x%x\n", regs[r] + opmSlotOffset[op] + fmCh, dat);
    }
  }
  _setVolume(trackNo);
}

// MDX 再生準備
bool MDXClass::ready() {
  ndFile.pos = 0;
  ND::canPlay = false;
  ND::chipNames.clear();
  nju72342.panMute();

  pdxLoaded = false;
  pcm8 = false;
  adpcmWarmupActive = false;
  mxdrv16y = false;
  adpcmGlobalPan = PAN_CENTER;
  nju72342.panSetPan(PAN_CENTER);
  parseDataEnd = ndFile.size;
  fadeoutLengh = -1;

  KeyBoard.reset();
  okim6258.reset();
  okim6258.latchAdpcmConfig();
  FM.setOKIM6258divider(okim6258.state.divider);

  // ステート初期化
  for (int i = 0; i < 16; i++) {
    tracks[i].reset(i);
  }
  for (int i = 8; i < 16; ++i) {
    portENTER_CRITICAL(&okim6258Mux);
    okim6258.state.adpcmVolume[i - 8] = getPcm8Gain(8);
    okim6258.state.adpcmBlockVolume[i - 8] = okim6258.state.adpcmVolume[i - 8];
    portEXIT_CRITICAL(&okim6258Mux);
  }

  _hasExplicitTempo = false;
  _setTempo(200);  // D1=0x12 D2=0xC8 初期値@t200。
  lastTick = 0;
  excess = 0;
  _nextTick = 0;
  _beatTickCounter = 0;

  _playingTracks = 0;  // トラックプレイ中ビット

  for (int i = 0; i < 8; i++) {
    FM.setRegisterOPM(0x38 + i, 0, 0);  // LFO位相変調・振幅変調感度設定レジスタ
  }
  FM.setRegisterOPM(0x01, 0, 0);     // bit 1 LFO_RESET
  FM.setRegisterOPM(0x0f, 0, 0);     // ノイズジェネレータ無効
  FM.setRegisterOPM(0x19, 0, 0);     // 振幅変調深度設定レジスタ
  FM.setRegisterOPM(0x19, 0x80, 0);  // 位相変調深度設定レジスタ

  //  L001e14 = 0;  グローバルな音量オフセット、フェードアウト用未使用
  //  mxdrv: タイマー割り込みはここで開始

  /// タイトル
  // CR LF EOFまで
  std::vector<u8_t> title_sjis;

  int charCount = 0;
  while (ndFile.pos < ndFile.size) {
    u8_t b = ndFile.get_ui8();

    if (b == 0x0d) {  // CR
      if (ndFile.pos + 1 < ndFile.size && ndFile.get_ui8_at(ndFile.pos) == 0x0a &&
          ndFile.get_ui8_at(ndFile.pos + 1) == 0x1a) {  // LF + EOF
        ndFile.pos += 2;
        break;
      }
    }
    title_sjis.push_back(b);
    charCount++;

    if (charCount >= 255) {
      return false;
    }
  }

  /// PDX ファイル名
  std::vector<u8_t> pdx_sjis;
  charCount = 0;
  bool pdxTerminated = false;
  while (ndFile.pos < ndFile.size) {
    u8_t b = ndFile.get_ui8();
    if (b == 0x00) {
      pdxTerminated = true;
      break;
    }
    pdx_sjis.push_back(b);
  }
  if (!pdxTerminated) {
    return false;
  }

  // Some MDX files include an extra 0x1A-delimited title line before the PDX
  // name. If found, append that part to the title and keep the bytes after the
  // last 0x1A as the PDX name.
  int lastEof = -1;
  for (int i = 0; i < (int)pdx_sjis.size(); i++) {
    if (pdx_sjis[i] == 0x1a) {
      lastEof = i;
    }
  }
  if (lastEof >= 0) {
    title_sjis.insert(title_sjis.end(), pdx_sjis.begin(), pdx_sjis.begin() + lastEof);
    pdx_sjis.erase(pdx_sjis.begin(), pdx_sjis.begin() + lastEof + 1);
  }

  // タイトル中の改行相当文字は1行表示用にスペースへ寄せる
  for (u8_t& b : title_sjis) {
    if (b == 0x0d || b == 0x0a || b == 0x1a) {
      b = 0x20;
    }
  }

  // 連続スペース 0x20, 0x8140 を一個にする
  {
    size_t write = 0;
    bool prevSpace = false;
    for (size_t read = 0; read < title_sjis.size();) {
      const bool isSjisSpace =
          read + 1 < title_sjis.size() && title_sjis[read] == 0x81 && title_sjis[read + 1] == 0x40;
      if (isSjisSpace) {
        if (!prevSpace) {
          title_sjis[write++] = 0x81;
          title_sjis[write++] = 0x40;
        }
        prevSpace = true;
        read += 2;
        continue;
      } else {
        const bool isAsciiSpace = title_sjis[read] == 0x20;
        if (!isAsciiSpace || !prevSpace) {
          title_sjis[write++] = title_sjis[read];
        }
        prevSpace = isAsciiSpace;
        read++;
      }
    }
    title_sjis.resize(write);
  }

  title = sjisToUtf8(title_sjis);
  subTitle = "";

  Serial.printf("タイトル: %s\n", title.c_str());

  String pdxFileName = sjisToUtf8(pdx_sjis);
  String pdxFileNameRaw = "";
  for (u8_t b : pdx_sjis) {
    pdxFileNameRaw += (char)b;
  }
  Serial.printf("PDX: %s\n", pdxFileName.c_str());

  u32_t size = 0;
  ndFile.pdxName = "";
  if (pdxFileName != "") {
    ndFile.pdxName = pdxFileName;
    size = ndFile.readPDX(pdxFileName);  // PDX読み込み
    if (size == 0 && pdxFileNameRaw != pdxFileName) {
      size = ndFile.readPDX(pdxFileNameRaw);
    }
    pdxLoaded = (size != 0);

    // pdx バンク対応でコメント
    // データブロック追加
    /*if (pdxLoaded) {
      okim6258.dataBlocks.clear();
      for (int i = 0; i < 96; i++) {
        okim6258.dataBlocks.push_back({ndFile.get_pdx_ui32_be_at(i * 8),
    ndFile.get_pdx_ui32_be_at(i * 8 + 0x04)});
      }
    }
    */
    if (size != 0) Serial.printf("PDX loaded size 0x%x.\n", size);
  }

  /// 音色データオフセット
  voiceDataOffset = ndFile.pos + ndFile.get_ui16_be();

  /// 各トラックの位置取得
  // トラック数判定
  trackCount = 9;

  const u32_t trackTablePos = ndFile.pos;
  u16_t firstTrack = ndFile.pos + ndFile.get_ui16_be_at(trackTablePos) - 2;
  if (firstTrack - ndFile.pos != 18) {
    // 0xE8 確認
    if (firstTrack < ndFile.size && ndFile.get_ui8_at(firstTrack) == 0xE8) {
      trackCount = 16;
    }
  }

  u32_t rawTrackAddress[16] = {};
  bool rawTrackValid[16] = {};
  for (int i = 0; i < trackCount; i++) {
    u32_t currentPos = ndFile.pos;
    u16_t relAddress = ndFile.get_ui16_be();
    if (relAddress == 0xffff) {  // 未使用トラック
      continue;
    }
    const u32_t baseOffset = 2 * (i + 1);
    if (relAddress < baseOffset) {
      continue;
    }
    u32_t addr = currentPos + relAddress - baseOffset;
    if (addr >= ndFile.size) {
      continue;
    }
    rawTrackAddress[i] = addr;
    rawTrackValid[i] = true;
  }

  u32_t voiceDataEnd = ndFile.size;
  mxdrv16y = false;
  bool voiceDataEndDetected = false;

  {
    // mxdrv16y対策
    // 音色領域終端を推定:
    // 1) voiceDataOffset より後ろを直接指すトラック開始位置
    // 2) トラック先頭の短い導入部で F1 ジャンプ等により
    //    voiceDataOffset 以降へ遷移する位置
    auto findTrackEntryAfterVoice = [&](u32_t startAddr) -> u32_t {
      u32_t pc = startAddr;

      // 先頭導入部だけを見る。通常ここでトラック本体への F1 ジャンプが出る。
      for (u32_t step = 0; step < 64; ++step) {
        if (pc >= ndFile.size) return 0;
        if (pc >= voiceDataOffset) return pc;

        u8_t command = ndFile.get_ui8_at(pc++);
        if (command <= 0xdf) {
          // 休符/音符に到達したら通常トラック本体とみなし、探索終了
          return 0;
        }

        switch (command) {
          case 0xff:
          case 0xfd:
          case 0xfc:
          case 0xfb:
          case 0xf8:
          case 0xf0:
          case 0xef:
          case 0xed:
          case 0xe9:
            if (pc >= ndFile.size) return 0;
            pc += 1;
            break;
          case 0xfe:
          case 0xf3:
          case 0xf2:
            if (pc + 1 >= ndFile.size) return 0;
            pc += 2;
            break;
          case 0xfa:
          case 0xf9:
          case 0xf7:
          case 0xee:
          case 0xe8:
            break;
          case 0xf1: {
            if (pc >= ndFile.size) return 0;
            u8_t nextByte = ndFile.get_ui8_at(pc);
            if (nextByte == 0x00) {
              return 0;
            }
            if (pc + 1 >= ndFile.size) return 0;
            s16_t offset = (s16_t)ndFile.get_ui16_be_at(pc);
            pc += 2;
            s32_t nextPc = (s32_t)pc + (s32_t)offset;
            if (nextPc < 0) return 0;
            pc = (u32_t)nextPc;
            break;
          }
          case 0xec:
          case 0xeb: {
            if (pc >= ndFile.size) return 0;
            u8_t nextByte = ndFile.get_ui8_at(pc);
            if (nextByte >= 0x80) {
              pc += 1;
            } else {
              if (pc + 4 >= ndFile.size) return 0;
              pc += 5;
            }
            break;
          }
          case 0xea: {
            if (pc >= ndFile.size) return 0;
            u8_t d2 = ndFile.get_ui8_at(pc++);
            if (!(d2 & 0x80)) {
              if (pc + 3 >= ndFile.size) return 0;
              pc += 4;
            }
            break;
          }
          case 0xe7: {
            if (pc >= ndFile.size) return 0;
            u8_t sub = ndFile.get_ui8_at(pc++);
            switch (sub) {
              case 0x01:
              case 0x03:
              case 0x04:
              case 0x05:
              case 0x06:
              case 0x0a:
                if (pc >= ndFile.size) return 0;
                pc += 1;
                break;
              case 0x02:
                if (pc + 5 >= ndFile.size) return 0;
                pc += 6;
                break;
              default:
                break;
            }
            break;
          }
          case 0xe6: {
            if (pc >= ndFile.size) return 0;
            u8_t sub = ndFile.get_ui8_at(pc++);
            switch (sub) {
              case 0x01:
                if (pc + 1 >= ndFile.size) return 0;
                pc += 2;
                break;
              case 0x02:
              case 0x03:
                if (pc >= ndFile.size) return 0;
                pc += 1;
                break;
              default:
                break;
            }
            break;
          }
          default:
            return 0;
        }
      }
      return 0;
    };

    for (int i = 0; i < trackCount; i++) {
      if (!rawTrackValid[i]) continue;
      u32_t addr = rawTrackAddress[i];
      if (addr >= voiceDataOffset && addr < voiceDataEnd) {
        voiceDataEnd = addr;
        voiceDataEndDetected = true;
      }
    }
    for (int i = 0; i < trackCount; i++) {
      if (!rawTrackValid[i]) continue;
      u32_t addr = rawTrackAddress[i];
      if (addr >= voiceDataOffset) continue;
      u32_t entryAfterVoice = findTrackEntryAfterVoice(addr);
      if (entryAfterVoice >= voiceDataOffset && entryAfterVoice < voiceDataEnd) {
        voiceDataEnd = entryAfterVoice;
        voiceDataEndDetected = true;
      }
    }
    if (!voiceDataEndDetected || voiceDataEnd <= voiceDataOffset) {
      // 境界推定失敗時は従来互換で EOF までを音色候補にする
      voiceDataEnd = ndFile.size;
    }
  }
  mxdrv16y = voiceDataEndDetected && (voiceDataEnd > voiceDataOffset);
  // Serial.printf("Voice Data Offset: 0x%x, End: 0x%x (%s)\n", voiceDataOffset, voiceDataEnd,
  //               mxdrv16y ? "MXDRV16y" : "default");

  // 変換テーブル初期化
  memset(voiceTable, 0xff, 256);
  u32_t p = voiceDataOffset;
  u8_t count = 0;

  while (p + 27 <= voiceDataEnd && count < 255) {
    bool emptySlot = true;
    for (int b = 0; b < 27; b++) {
      if (ndFile.get_ui8_at(p + b) != 0x00) {
        emptySlot = false;
        break;
      }
    }
    if (mxdrv16y && emptySlot) {
      // Some 16y files pad the inferred voice area with zero-filled slots.
      // Stop here so voice number 0 does not get remapped to an empty slot.
      break;
    }

    u8_t voiceNum = ndFile.get_ui8_at(p);
    if (voiceTable[voiceNum] == 0xff) {
      // Keep the first occurrence for duplicated IDs.
      voiceTable[voiceNum] = count;
    }
    // Serial.printf("音色ID: %x\n", voiceNum);
    p += 27;
    count++;
  }

  pcm8 = (trackCount == 16);
  Serial.printf("Track count: %d\n", trackCount);

  // パン状態の初期化
  if (xSemaphoreTake(KeyBoard.keyinfoMutex, portMAX_DELAY) == pdTRUE) {
    for (int i = 8; i < 16; i++) {
      KeyBoard.trackPan[i] = PAN_MUTE;
    }
    xSemaphoreGive(KeyBoard.keyinfoMutex);
  }

  if (mxdrv16y && !pcm8 && !pdxLoaded) {
    // Search from voice area start, because some files place embedded comments
    // before the inferred voiceDataEnd boundary.
    u32_t searchStart = voiceDataOffset;

    for (u32_t p = searchStart; p + 5 < ndFile.size; ++p) {
      if (ndFile.get_ui8_at(p) != 0x1a) continue;
      if (ndFile.get_ui8_at(p + 1) != 0x0d || ndFile.get_ui8_at(p + 2) != 0x0a) continue;

      const u32_t windowEnd = min(ndFile.size, p + 192u);
      int crlfCount = 0;
      int badCtrlCount = 0;
      for (u32_t q = p + 1; q < windowEnd; ++q) {
        u8_t c = ndFile.get_ui8_at(q);
        if (c < 0x20 && c != 0x09 && c != 0x0a && c != 0x0d) {
          badCtrlCount++;
        }
        if (q + 1 < windowEnd && c == 0x0d && ndFile.get_ui8_at(q + 1) == 0x0a) {
          crlfCount++;
        }
      }
      if (crlfCount >= 2 && badCtrlCount <= 4) {
        parseDataEnd = p;
        break;
      }
    }
  }

  u32_t playTrackAddress[16] = {};
  bool playTrackValid[16] = {};
  for (int i = 0; i < 16; ++i) {
    playTrackAddress[i] = rawTrackAddress[i];
    playTrackValid[i] = rawTrackValid[i];
  }

  // PCM1/PCM8に関係なく、新しく読み込んだPDXをPCMへ展開する。
  // readPDX()が1を返す場合は同じPDXを展開済みなのでスキップする。
  if (size > 1) {
    u32_t decodeStartUs = micros();
    okim6258.decode();
    Serial.printf("OKIM6258 decode time: %u us\n", micros() - decodeStartUs);
  }

  for (int i = 0; i < trackCount; i++) {
    if (!playTrackValid[i]) {  // 未使用トラック/不正アドレス
      tracks[i].address = 0;
      tracks[i].active = false;
      tracks[i].countForSongEnd = false;
      _playingTracks &= ~(1 << i);
    } else {
      tracks[i].address = playTrackAddress[i];
      if (tracks[i].address >= voiceDataOffset && tracks[i].address < voiceDataEnd) {
        // 音色領域中を直接指している場合は無効
        tracks[i].address = 0;
        tracks[i].active = false;
        tracks[i].countForSongEnd = false;
        _playingTracks &= ~(1 << i);
      } else {
        tracks[i].pc = tracks[i].address;
        tracks[i].active = true;
        if (i < 8) {
          tracks[i].countForSongEnd = true;
        } else {
          // Keep legacy behavior by default. Narrowly exclude only the known
          // obfuscated 9-track MXDRV16y/no-PDX case on track 8.
          tracks[i].countForSongEnd = true;
          if (!pcm8 && i == 8 && mxdrv16y && !pdxLoaded) {
            tracks[i].countForSongEnd = false;
          }
        }
        _playingTracks |= (1 << i);
      }
    }
    // Serial.printf("トラック%d, 0x%x\n", i, tracks[i].address);
  }

  // 周波数固定
  ND::freq[0] = SI5351_4000;
  ND::chipNames.push_back(ND::formatChipName(SI5351_4000, CHIP_YM2151));
  ND::freq[1] = SI5351_8000;
  ND::chipNames.push_back(ND::formatChipName(SI5351_8000, CHIP_OKIM6258));
  SI5351.setFreq(ND::freq[0], 0);
  SI5351.setFreq(ND::freq[1], 1);
  SI5351.enableOutputs(true);

  // 表示更新
  int fileIndex = fileTree.getFileIndexInParent(ndFile.currentNode);
  u32_t n = (fileIndex >= 0) ? (u32_t)(fileIndex + 1) : 0;  // フォルダ内曲番
  String author = "";
  auto appendAttribute = [&](const char* label) {
    if (author.length() > 0) author += ", ";
    author += label;
  };
  if (pcm8) appendAttribute("PCM8");
  if (isLZX) appendAttribute("LZX");
  if (mxdrv16y) appendAttribute("MXDRV16y");
  Node* currentDirNode = ndFile.currentNode->parent;
  u32_t maxFiles = currentDirNode->fileCount;
  String currentDir = truncateUtf8WithEllipsis(fileTree.getFullPath(currentDirNode),
                                               MDX_PLAYER_PATH_MAX_CHARACTERS);

  String pdx = "";
  if (ndFile.pdxName != "") {
    pdx = ndFile.pdxName;
    if (!pdx.endsWith(".pdx") && !pdx.endsWith(".PDX")) {
      pdx += ".pdx";
    }
    if (!pdxLoaded) {
      pdx += " (missing)";
    }
  }

  playerWindow.updateDisp({title, title, String(ndFile.currentNode->name),
                           String(ndFile.currentNode->name), currentDir, currentDir, author, author,
                           pdx, ND::chipNames[0], ND::chipNames[1],
                           FORMAT_LABEL[(int)ND::fileFormat], 0, n, maxFiles});

  nju72342.panMute();
  ets_delay_us(520);
  if (pdxLoaded) {
    adpcmWarmupActive = true;
    FM.setOKIM6258command(0b00000010, 1);  // ADPCM オン
    // OKIM6258仕様書の4MHz時PLAY開始最大16msを安全側でそのまま採用する。
    // PCM1/PCM8ともPANミュート中に無音ADPCMを送り、
    // 出力とデータ経路を安定させてから曲再生を開始する。
    ets_delay_us(16000);
    nju72342.panUnmute();
  }
  // ets_delay_us(2000);
  // nju72342.panUnmute();
  _startTick = millis();
  if (Leds.ready()) {
    Leds.set(MDX_BEAT_LED, 0);
  }
  Serial.printf("Ready heap - Free %'d, Min free %'d, Largest block %'d\n", ESP.getFreeHeap(),
                ESP.getMinFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  Serial.printf("Ready PSRAM - Free %'d, Min free %'d, Largest block %'d\n", ESP.getFreePsram(),
                heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
                heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
  return true;
}

void MDXClass::_setVolume(u8_t trackNo) {
  if (trackNo >= 8) return;  // FM only

  u8_t fmCh = getFmChannel(tracks[trackNo], trackNo);
  s16_t tlValue = 0, finalTL;
  u8_t v = tracks[trackNo].volume;  // セットされてる vの値
  if (v & 0x80) {
    tlValue = v & 0x7f;
  } else {
    tlValue = FMVOLTABLE[v & 0x0f];
  }

  tlValue += tracks[trackNo].volumeLFOOffset >> 8;

  /*
    アルゴリズム別のキャリアを示すビット
    1000 1000 1000 1000 1100 1110 1110 1111

    0: 1->2->3->4
    1: 1->*->3->4
       2->*
    2: 1->*
       2-3->*->4
    3: 1->2->*->4
       3->4
    4: 1->2
       3->4
    5: 1->2
       1->3
       1->4
    6: 1->2
          3
          4
    7: 1
       2
       3
       4

  */

  static const u8_t carrierSlots[8] = {0x08, 0x08, 0x08, 0x08, 0x0c, 0x0e, 0x0e, 0x0f};

  const u8_t* voiceTL = tracks[trackNo].tl;
  u8_t pendingTL[4];
  if (tracks[trackNo].flags & 0x02) {
    const u8_t internalIdx = voiceTable[tracks[trackNo].voiceNo];
    if (internalIdx != 0xff) {
      const u32_t tlAddr = voiceDataOffset + internalIdx * 27 + 7;
      for (int op = 0; op < 4; ++op) {
        pendingTL[op] = ndFile.get_ui8_at(tlAddr + op);
      }
      voiceTL = pendingTL;
    }
  }

  u8_t con = tracks[trackNo].con_fl & 0x07;  // Connect(AL) アルゴリズム

  for (int op = 0; op < 4; op++) {
    if (carrierSlots[con] & (1 << op)) {  // キャリアなら
      finalTL = voiceTL[op] + tlValue;    // TL調整
      if (finalTL < 0) {
        finalTL = 0;
      } else if (finalTL > 127) {
        finalTL = 127;
      }

    } else {
      finalTL = voiceTL[op];
    }
    if (finalTL > 127 || finalTL < 0) {
      finalTL = 127;
    }
    FM.setRegisterOPM(0x60 + op * 8 + fmCh, finalTL, 0);
  }
}

u64_t lastTick;
u64_t excess;

void MDXClass::process() {
  // フェードアウト完了
  if (nju72342.fadeOutStatus == FADEOUT_COMPLETED) {
    _endProcedure();
    return;
  }

  u64_t now = micros64();
  if (lastTick == 0) {
    lastTick = now;
  }
  _nextTick = lastTick + _interval;

  processTick();

  // countForSongEnd 対象トラックがすべて終了したら曲終了
  bool anySongTrackActive = false;
  for (int i = 0; i < trackCount; i++) {
    if (!tracks[i].active || !tracks[i].countForSongEnd || tracks[i].syncWait) {
      continue;
    }
    anySongTrackActive = true;
    break;
  }
  if (!anySongTrackActive) {
    Serial.printf("演奏終了\n");
    _endProcedure();
    return;
  }

  while (1) {
    now = micros64();
    if (now >= _nextTick) {
      break;
    }
    // ets_delay_us(1);
    yield();
  }

  if (now > _nextTick) {
    u64_t late = now - _nextTick;
    u64_t skip = late / _interval;
    _nextTick += skip * _interval;
    excess = late % _interval;
  } else {
    excess = 0;
  }

  // Serial.printf("%d\n", now - lastTick);
  lastTick = _nextTick;
}

void MDXClass::processTick() {
  _updateBeatLed();
  _beatTickCounter++;

  const bool usePcm8Shadow =
      (MDX_USE_PCM8_SHADOW && pcm8 && pdxLoaded);  // 割り込み時の state を保証する

  struct Pcm8ShadowState {
    int currentBlockId[8];
    u32_t posInBlock[8];
    u32_t blockStartPos[8];
    u32_t blockSizeBytes[8];
    u32_t playSerial[8];
    u8_t adpcmVolume[8];
    u8_t adpcmBlockVolume[8];
    u8_t pcmDataKind[8];
    bool hold[8];
  } pcm8Shadow{};

  bool pcm8StateDirty = false;
  bool pcm8TransportDirty[8] = {};
  bool pcm8HoldDirty[8] = {};
  bool pcm8VolumeDirty[8] = {};
  bool pcm8RateResetDirty[8] = {};

  if (usePcm8Shadow) {
    portENTER_CRITICAL(&okim6258Mux);
    for (int ch = 0; ch < 8; ch++) {
      pcm8Shadow.currentBlockId[ch] = okim6258.state.currentBlockId[ch];
      pcm8Shadow.posInBlock[ch] = okim6258.state.posInBlock[ch];
      pcm8Shadow.blockStartPos[ch] = okim6258.state.blockStartPos[ch];
      pcm8Shadow.blockSizeBytes[ch] = okim6258.state.blockSizeBytes[ch];
      pcm8Shadow.playSerial[ch] = okim6258.state.playSerial[ch];
      pcm8Shadow.adpcmVolume[ch] = okim6258.state.adpcmVolume[ch];
      pcm8Shadow.adpcmBlockVolume[ch] = okim6258.state.adpcmBlockVolume[ch];
      pcm8Shadow.pcmDataKind[ch] = okim6258.state.pcmDataKind[ch];
      pcm8Shadow.hold[ch] = okim6258.state.hold[ch];
    }
    portEXIT_CRITICAL(&okim6258Mux);
  }

  auto pcm8SetStop = [&](int ch) {
    if (usePcm8Shadow) {
      pcm8Shadow.currentBlockId[ch] = -1;
      pcm8Shadow.posInBlock[ch] = 0;
      pcm8Shadow.blockStartPos[ch] = 0;
      pcm8Shadow.blockSizeBytes[ch] = 0;
      pcm8Shadow.pcmDataKind[ch] = 4;
      pcm8Shadow.hold[ch] = false;
      pcm8TransportDirty[ch] = true;
      pcm8HoldDirty[ch] = false;
      pcm8StateDirty = true;
    } else {
      portENTER_CRITICAL(&okim6258Mux);
      okim6258.state.currentBlockId[ch] = -1;
      okim6258.state.posInBlock[ch] = 0;
      okim6258.state.blockStartPos[ch] = 0;
      okim6258.state.blockSizeBytes[ch] = 0;
      okim6258.state.hold[ch] = false;
      portEXIT_CRITICAL(&okim6258Mux);
    }
    KeyBoard.set(OKIM6258_KEY, ch, {0, 0});
  };

  auto pcm8SetHold = [&](int ch, bool holdValue) {
    if (usePcm8Shadow) {
      pcm8Shadow.hold[ch] = holdValue;
      if (!pcm8TransportDirty[ch]) {
        pcm8HoldDirty[ch] = true;
      }
      pcm8StateDirty = true;
    } else {
      portENTER_CRITICAL(&okim6258Mux);
      okim6258.state.hold[ch] = holdValue;
      portEXIT_CRITICAL(&okim6258Mux);
    }
  };

  auto pcm8SetVolume = [&](int ch, u8_t gain) {
    if (usePcm8Shadow) {
      pcm8Shadow.adpcmVolume[ch] = gain;
      pcm8VolumeDirty[ch] = true;
      pcm8StateDirty = true;
    } else {
      portENTER_CRITICAL(&okim6258Mux);
      okim6258.state.adpcmVolume[ch] = gain;
      portEXIT_CRITICAL(&okim6258Mux);
    }
  };

  auto pcm8GetCurrentId = [&](int ch) -> int {
    if (usePcm8Shadow) {
      return pcm8Shadow.currentBlockId[ch];
    }
    int currentId = -1;
    portENTER_CRITICAL(&okim6258Mux);
    currentId = okim6258.state.currentBlockId[ch];
    portEXIT_CRITICAL(&okim6258Mux);
    return currentId;
  };

  auto countTrackLoop = [&](int trackNo) {
    tracks[trackNo].trackLoops++;

    if (nju72342.fadeOutStatus == FADEOUT_BEFORE && ndConfig.get(CFG_NUM_LOOP) != LOOP_INIFITE) {
      const u16_t targetLoops = ndConfig.get(CFG_NUM_LOOP);
      bool allReachedTrackEnd = true;
      for (int t = 0; t < trackCount; t++) {
        if (tracks[t].address == 0x00 || !tracks[t].active || !tracks[t].countForSongEnd ||
            tracks[t].syncWait) {
          continue;
        }
        if (tracks[t].trackLoops < targetLoops) {
          allReachedTrackEnd = false;
          break;
        }
      }
      if (allReachedTrackEnd) {
        nju72342.startFadeout();
      }
    }
  };

  for (int i = 0; i < MDX.trackCount; i++) {
    if (tracks[i].address == 0x00 || !tracks[i].active) {
      continue;
    }

    // 待ちtick減らす
    if (tracks[i].waitTicks > 0) {
      tracks[i].waitTicks--;
    }
    if (i >= 8 && tracks[i].adpcmHold) {
      const int currentId = pcm8GetCurrentId(i - 8);
      if (currentId == -1) {
        tracks[i].adpcmHold = false;
      }
    }

    // FMのみの処理
    if (i < 8) {
      u16_t prevVolumeLFOOffset = tracks[i].volumeLFOOffset;
      // pitch bend
      if ((tracks[i].flagsB3 & 0x80) && (tracks[i].keyOnDelayCounter == 0)) {
        s32_t bend = (s32_t)tracks[i].bendOffset;
        s32_t delta = (s32_t)tracks[i].bendDelta;
        bend += delta;
        tracks[i].bendOffset = (u32_t)bend;
      }
      bool skipLFO = false;
      if (tracks[i].LFODelay > 0) {
        if (tracks[i].keyOnDelayCounter != 0) {
          skipLFO = true;
        } else if (tracks[i].LFODelayCounter > 0) {
          tracks[i].LFODelayCounter--;
          if (tracks[i].LFODelayCounter == 0) {
            if (tracks[i].flagsB3 & 0x20) {
              resetPitchLFO(tracks[i]);
            }
            if (tracks[i].flagsB3 & 0x40) {
              resetVolumeLFO(tracks[i]);
            }
          }
          skipLFO = true;
        }
      }
      if (!skipLFO) {
        updatePitchLFO(tracks[i]);
        updateVolumeLFO(tracks[i]);
      }
      if (tracks[i].baseNote >= 0) {
        u8_t fmCh = getFmChannel(tracks[i], i);
        u16_t bend = (u16_t)(tracks[i].bendOffset >> 16);
        s16_t lfo = (s16_t)((s32_t)tracks[i].pitchLFOOffset >> 16);
        u16_t pitchWord = (u16_t)((s32_t)tracks[i].noteD + (s32_t)bend + (s32_t)lfo);
        writePitchIfChanged(tracks[i], fmCh, pitchWord);
      }
      if (tracks[i].volumeLFOOffset != prevVolumeLFOOffset) {
        _setVolume(i);
      }
    }

    // キーオフチェック
    if (tracks[i].keyOffTicks > 0) {
      if (!(tracks[i].flagsB3 & 0x04)) {  // キーオンフラグオン
        tracks[i].keyOffTicks--;
        if (tracks[i].keyOffTicks == 0) {
          if (i < 8) {
            if (tracks[i].flagsB3 & 0x08) {
              u8_t fmCh = getFmChannel(tracks[i], i);
              tracks[i].flagsB3 &= ~0x08;  // キーオンフラグオフ (L000fe6)
              FM.setRegisterOPM(0x08, fmCh, 0);
              setTrackDisplayLevel(i, 0);
              KeyBoard.set(YM2151, fmCh,
                           {0, 0});  // キーオフ後 LFO/音程更新を続けるため baseNote を継続
            }

          } else if (i >= 8) {
            int adpcmIndex = i - 8;
            if (!pcm8) {
              nju72342.panMute();
              // ets_delay_us(520);
              // FM.setOKIM6258command(0b00000001, 1);  // ADPCM オフ
            }
            pcm8SetStop(adpcmIndex);
            tracks[i].adpcmHold = false;
            tracks[i].flagsB3 &= ~0x08;
            setTrackDisplayLevel(i, 0);
            //  Serial.printf("ADPCMオフ\n");
          }
        }
      }
    }

    // キーオンディレイカウンタを減らし、0になったら音出す
    if (tracks[i].keyOnDelayCounter > 0) {
      tracks[i].keyOnDelayCounter--;
      if (tracks[i].keyOnDelayCounter == 0 && tracks[i].keyOnPending) {
        if (i < 8) {
          _applyPendingFmState(i);
          u8_t fmCh = getFmChannel(tracks[i], i);
          u8_t con = tracks[i].con_fl & 0x07;
          static const u8_t carriers[8] = {0x40, 0x40, 0x40, 0x40, 0x50, 0x70, 0x70, 0x78};
          u8_t keyonSlot = tracks[i].keyonSlot;
          if ((keyonSlot & 0xF8) == 0) {
            keyonSlot = (u8_t)(carriers[con] | fmCh);
          }
          if (!(tracks[i].flagsB3 & 0x08) && tracks[i].LFODelay > 0) {
            startLFODelay(tracks[i]);
          }
          if (!(tracks[i].flagsB3 & 0x08)) {
            resetOpmLFOIfNeeded(tracks[i]);
            FM.setRegisterOPM(0x08, keyonSlot, 0);
            tracks[i].flagsB3 |= 0x08;  // key-on active (L000e7e)
            setTrackDisplayLevel(i, calcFmDisplayLevel(i));
          }
          tracks[i].bendOffset = 0;
        }
        tracks[i].keyOnPending = false;
      }
    }

    if (tracks[i].syncWait) {
      continue;
    }
    if (tracks[i].waitTicks > 0) {  // 待ち時間
      continue;
    } else {
      // mxdrv: 新規イベント前に tick
      // ごとにポルタメントとキーオフ無効化フラグをクリア
      tracks[i].flagsB3 &= ~(0x80 | 0x04);
      while (tracks[i].active && tracks[i].waitTicks == 0) {
        if (tracks[i].pc >= parseDataEnd) {
          tracks[i].active = false;
          KeyBoard.trackPan[i] = PAN_MUTE;  // トラック終了表示
          setTrackDisplayLevel(i, 0);
          _playingTracks &= ~(1 << i);
          break;
        }
        u32_t pos = tracks[i].pc;
        u8_t command = ndFile.get_ui8_at(tracks[i].pc++);

        switch (command) {
          case 0x00 ... 0x7F: {  // 休符データ
            u16_t restLen = (u16_t)command + 1;
            tracks[i].waitTicks = restLen;
            tracks[i].keyOffTicks = restLen;  // S001b
            tracks[i].hasTimedEvent = true;
            // Rest cancels key-off disable and ADPCM hold (tie)
            tracks[i].flagsB3 &= ~0x04;
            if (i >= 8 && tracks[i].adpcmHold) {
              int adpcmIndex = i - 8;
              tracks[i].adpcmHold = false;
              pcm8SetHold(adpcmIndex, false);
            }
            // Serial.printf("ch%d 0x%x 休符 0x%x\n", i, pos, command + 1);
            break;
          }

          case 0x80 ... 0xdf: {  // 音符データ
            u8_t note = command;
            u8_t rawLen = ndFile.get_ui8_at(tracks[i].pc++);
            u16_t length = (u16_t)rawLen + 1;  // 1byte length, +1 tick

            tracks[i].waitTicks = length;
            tracks[i].hasTimedEvent = true;

            bool keyOffDisabled = (tracks[i].flagsB3 & 0x04) != 0;  // キーオフ無効 (F7)
            if (keyOffDisabled) {
              tracks[i].keyOffTicks = 0;
              tracks[i].flagsB3 &= ~0x04;
            } else {
              // 発音長
              s32_t kot;
              s8_t gt = (s8_t)tracks[i].gateTime;
              if (gt >= 0) {
                kot = ((s32_t)rawLen * (s32_t)gt) >> 3;
              } else {
                kot = (s32_t)rawLen + (s32_t)gt;
                if (kot < 0) {
                  kot = 0;
                }
              }
              kot += 1;
              tracks[i].keyOffTicks = (u8_t)kot;
            }

            if (i < 8) {
              // FM音源
              u8_t fmCh = getFmChannel(tracks[i], i);
              u8_t noteNumRaw = note - 0x80;
              s16_t transposed = (s16_t)noteNumRaw + (s16_t)tracks[i].transpose;
              if (transposed < 0) {
                transposed = 0;
              } else if (transposed > 127) {
                transposed = 127;
              }
              u8_t noteNum = (u8_t)transposed;
              int display_value = noteNum + 3;  // O0のAからスタート
              int display_oct = display_value / 12;
              int display_note = display_value % 12;
              KeyBoard.set(YM2151, fmCh, {display_oct, display_note});  // キーボードセット
              tracks[i].baseNote = noteNum;
              tracks[i].noteD = (u16_t)(((u16_t)noteNum << 6) + 5 + (s16_t)tracks[i].D);
              writePitchAndCache(tracks[i], fmCh, tracks[i].noteD);

              if (tracks[i].keyOnDelay > 0) {
                tracks[i].keyOnDelayCounter = tracks[i].keyOnDelay;
                tracks[i].keyOnPending = true;

              } else {
                _applyPendingFmState(i);  // FM のステート適用はキーオン直前に行う
                fmCh = getFmChannel(tracks[i], i);
                u8_t keyonSlot = tracks[i].keyonSlot;
                if ((keyonSlot & 0xF8) == 0) {
                  static const u8_t carriers[8] = {0x40, 0x40, 0x40, 0x40, 0x50, 0x70, 0x70, 0x78};
                  keyonSlot = (u8_t)(carriers[tracks[i].con_fl & 0x07] | fmCh);
                }
                if (!(tracks[i].flagsB3 & 0x08) && tracks[i].LFODelay > 0) {
                  startLFODelay(tracks[i]);
                }
                tracks[i].bendOffset = 0;
                if (!(tracks[i].flagsB3 & 0x08)) {
                  resetOpmLFOIfNeeded(tracks[i]);
                  FM.setRegisterOPM(0x08, keyonSlot, 0);  // キーオン
                  tracks[i].flagsB3 |= 0x08;              // キーオンアクティブ (L000e7e)
                  setTrackDisplayLevel(i, calcFmDisplayLevel(i));
                }
              }

            } else if (i >= 8) {
              // ADPCM キーオン
              u8_t noteNum = note - 0x80;
              int adpcmIndex = i - 8;
              if (!pdxLoaded) {
                pcm8SetStop(adpcmIndex);
                tracks[i].adpcmHold = false;
                tracks[i].flagsB3 &= ~0x08;
                setTrackDisplayLevel(i, 0);
                break;
              }

              const int blockId = (int)(((u32_t)tracks[i].PCMBank * 96u) + (u32_t)noteNum);

              if (tracks[i].adpcmHold) {
                int currentId = pcm8GetCurrentId(adpcmIndex);
                if (currentId == blockId && currentId != -1) {
                  // ホールド中は同一IDの再トリガしない(わぴこ対策)
                  break;
                }
              }

              if (!pcm8) {
                nju72342.panMute();
                ets_delay_us(1600);
                //  FM.setOKIM6258command(0b00000010, 1);  // ADPCM オン
              }

              const u32_t entry = ((u32_t)tracks[i].PCMBank * 96u + (u32_t)noteNum) * 8u;
              const u32_t minStartPos = ((u32_t)tracks[i].PCMBank + 1u) * 96u * 8u;
              const u32_t startPos = ndFile.get_pdx_ui32_be_at(entry);
              u32_t sizeBytes = ndFile.get_pdx_ui32_be_at(entry + 0x04);
              if (startPos < minStartPos || startPos >= ndFile.pdxSize) {
                sizeBytes = 0;
              } else if (startPos + sizeBytes > ndFile.pdxSize) {
                sizeBytes = ndFile.pdxSize - startPos;
              }
              if (sizeBytes != 0) {
                u64_t pcmBase64 = (u64_t)PCM_OFFSET + ((u64_t)startPos * 4ULL);
                if (pcmBase64 >= MAX_FILE_SIZE) {
                  sizeBytes = 0;
                } else {
                  u64_t maxDecodedBytes = (MAX_FILE_SIZE - pcmBase64) / 4ULL;
                  if ((u64_t)sizeBytes > maxDecodedBytes) {
                    sizeBytes = (u32_t)maxDecodedBytes;
                  }
                }
              }
              if (sizeBytes == 0) {
                pcm8SetStop(adpcmIndex);
                tracks[i].adpcmHold = false;
                tracks[i].flagsB3 &= ~0x08;
                setTrackDisplayLevel(i, 0);
                break;
              }

              if (tracks[i].adpcmPan != adpcmGlobalPan) {
                nju72342.panSetPan((tPan)tracks[i].adpcmPan);
                adpcmGlobalPan = tracks[i].adpcmPan;
              }
              KeyBoard.trackPan[i] = (tPan)tracks[i].adpcmPan;

              if (usePcm8Shadow) {
                pcm8Shadow.playSerial[adpcmIndex]++;
                pcm8Shadow.currentBlockId[adpcmIndex] = blockId;
                pcm8Shadow.posInBlock[adpcmIndex] = 0;
                pcm8Shadow.blockStartPos[adpcmIndex] = startPos;
                pcm8Shadow.blockSizeBytes[adpcmIndex] = sizeBytes;
                pcm8Shadow.adpcmBlockVolume[adpcmIndex] = pcm8Shadow.adpcmVolume[adpcmIndex];
                pcm8Shadow.pcmDataKind[adpcmIndex] = tracks[i].pcmDataKind;
                pcm8Shadow.hold[adpcmIndex] = keyOffDisabled;
                pcm8TransportDirty[adpcmIndex] = true;
                pcm8HoldDirty[adpcmIndex] = false;
                pcm8RateResetDirty[adpcmIndex] = true;
                pcm8StateDirty = true;
              } else {
                portENTER_CRITICAL(&okim6258Mux);
                okim6258.state.playSerial[adpcmIndex]++;
                okim6258.state.currentBlockId[adpcmIndex] = blockId;
                okim6258.state.posInBlock[adpcmIndex] = 0;
                okim6258.state.blockStartPos[adpcmIndex] = startPos;
                okim6258.state.blockSizeBytes[adpcmIndex] = sizeBytes;
                okim6258.state.adpcmBlockVolume[adpcmIndex] =
                    okim6258.state.adpcmVolume[adpcmIndex];
                okim6258.state.pcmDataKind[adpcmIndex] = tracks[i].pcmDataKind;
                okim6258.state.hold[adpcmIndex] = keyOffDisabled;
                tracks[i].pcmRateCounter = 0;
                portEXIT_CRITICAL(&okim6258Mux);
              }
              tracks[i].adpcmHold = keyOffDisabled;
              tracks[i].flagsB3 |= 0x08;
              setTrackDisplayLevel(i, quantizeAdpcmDisplayLevel(tracks[i].volume));
              {
                int display_value = noteNum + 9;  // O0のAからスタート
                int display_oct = display_value / 12;
                int display_note = display_value % 12;
                KeyBoard.set(OKIM6258_KEY, i - 8, {display_oct, display_note});
              }

              if (tracks[i].keyOnDelay > 0) {
                tracks[i].keyOnDelayCounter = tracks[i].keyOnDelay;
                tracks[i].keyOnPending = true;
              } else {
                tracks[i].bendOffset = 0;
                if (!pcm8) {
                  ets_delay_us(520);
                  nju72342.panUnmute();
                }
              }
            }

            // Serial.printf("ch%d 0x%x キー 0x%x 長さ 0x%x, waitTicks %d
            // keyoffTicks %d gateTime %d \n", i, pos, note,
            //               length, length, tracks[i].keyOffTicks,
            //               tracks[i].gateTime);
            /*
            if (i == 3) {
              for (int x = 0; x < 16; x++) {
                for (int y = 0; y < 16; y++) {
                  Serial.printf("%02x ", FM.ym2151_reg[x * 16 + y]);
                }
                Serial.printf("\n");
              }
              Serial.printf("\n");
            }*/

            break;
          }
          case 0xff: {  // テンポ設定
            u8_t tempo = ndFile.get_ui8_at(tracks[i].pc++);
            _setTempo(tempo);
            _hasExplicitTempo = true;
            // Serial.printf("ch%d 0x%x テンポ: %d\n", i, pos, tempo);
            break;
          }
          case 0xfe: {  // OPMレジスタ設定
            u8_t reg = ndFile.get_ui8_at(tracks[i].pc++);
            u8_t dat = ndFile.get_ui8_at(tracks[i].pc++);
            if (mxdrv16y && i < 8) {
              u8_t prevFmCh = tracks[i].fmChannel;
              bool channelHint = false;
              if (reg == 0x08) {
                tracks[i].fmChannel = dat & 0x07;
                channelHint = true;
              }
              if (channelHint && tracks[i].fmChannel != prevFmCh && tracks[i].voiceNo != 0xff) {
                // 一部ファイルで 0xfe 前に 0xfd がある問題
                tracks[i].flags |= 0x02;
              }
            }
            if (i < 8 || !pcm8) {
              FM.setRegisterOPM(reg, dat, 0);
              if (i >= 8) {
                tracks[i].countForSongEnd = true;
              }
            }
            break;
          }
          case 0xfd: {  // 音色設定/PCMバンク設定
            u8_t voice = ndFile.get_ui8_at(tracks[i].pc++);
            if (i < 8) {
              tracks[i].voiceNo = voice;
              // Split-note songs rely on the new voice taking effect on the
              // next key-on.
              tracks[i].flags |= 0x02;
            } else {
              tracks[i].PCMBank = voice;
              // Serial.printf("ch%d 0x%x PCMバンク 0x%x\n", i, pos, voice);
            }
            break;
          }
          case 0xfc: {  // パン設定
            u8_t pan = ndFile.get_ui8_at(tracks[i].pc++);
            if (i < 8) {  // FM
              // pan: 0=無, 1=右, 2=左, 3=中(両方)
              u8_t panBits = 0;
              if (pan == 1) {
                panBits = 0x40;
                KeyBoard.trackPan[i] = PAN_RIGHT;
              } else if (pan == 2) {
                panBits = 0x80;
                KeyBoard.trackPan[i] = PAN_LEFT;
              } else if (pan == 3) {
                panBits = 0xC0;
                KeyBoard.trackPan[i] = PAN_CENTER;
              } else {
                KeyBoard.trackPan[i] = PAN_MUTE;
              }
              tracks[i].pan = panBits;
              // mxdrv のキーオン境界ステート更新に合わせる
              tracks[i].flags |= 0x04;
            } else if (i >= 8) {  // ADPCM
              switch (pan) {
                case 0x00:
                  tracks[i].adpcmPan = PAN_MUTE;
                  KeyBoard.trackPan[i] = PAN_MUTE;
                  // Serial.printf("OKIM6258パン: ミュート\n");
                  break;
                case 0x02:
                  tracks[i].adpcmPan = PAN_RIGHT;
                  KeyBoard.trackPan[i] = PAN_RIGHT;
                  // Serial.printf("OKIM6258パン: 右\n");
                  break;
                case 0x01:
                  tracks[i].adpcmPan = PAN_LEFT;
                  KeyBoard.trackPan[i] = PAN_LEFT;
                  // Serial.printf("OKIM6258パン: 左\n");
                  break;
                case 0x03:
                  tracks[i].adpcmPan = PAN_CENTER;
                  KeyBoard.trackPan[i] = PAN_CENTER;
                  // Serial.printf("OKIM6258パン: センター\n");
                  break;
              }
            }
            // Serial.printf("ch%d 0x%x パン 0x%x\n", i, pos, pan);
            break;
          }
          case 0xfb: {  // 音量設定
            u8_t vol = ndFile.get_ui8_at(tracks[i].pc++);
            tracks[i].volume = vol;
            if (i < 8) {
              _setVolume(i);
            } else {
              pcm8SetVolume(i - 8, getPcm8Gain(vol));
              // Serial.printf("ch%d 音量 vol 0x%x gain 0x%x\n", i, vol,
              // okim6258.state.adpcmVolume[i - 8]);
            }
            break;
          }
          case 0xfa: {                         // 音量減少
            if (!(tracks[i].volume & 0x80)) {  // 0..15
              if (tracks[i].volume > 0) tracks[i].volume--;
            } else {  // @v (0x80..0xFF)
              if (tracks[i].volume != 0xFF) tracks[i].volume++;
            }
            if (i < 8) {
              _setVolume(i);
            } else {
              pcm8SetVolume(i - 8, getPcm8Gain(tracks[i].volume));
            }
            // Serial.printf("ch%d 0x%x 音量減少 v%d\n", i, pos,
            // tracks[i].volume);
            break;
          }
          case 0xf9: {  // 音量増大
            if (!(tracks[i].volume & 0x80)) {
              if (tracks[i].volume < 15) tracks[i].volume++;
            } else {
              if (tracks[i].volume != 0x80) tracks[i].volume--;
            }
            if (i < 8) {
              _setVolume(i);
            } else {
              pcm8SetVolume(i - 8, getPcm8Gain(tracks[i].volume));
            }
            // Serial.printf("ch%d 0x%x 音量増大 v%d\n", i, pos,
            // tracks[i].volume);
            break;
          }
          case 0xf8: {  // 発音長指定
            u8_t len = ndFile.get_ui8_at(tracks[i].pc++);
            tracks[i].gateTime = len;
            // Serial.printf("ch%d 0x%x 発音長: %d\n", i, pos, len);
            break;
          }
          case 0xf7: {                  // キーオフ無効
            tracks[i].flagsB3 |= 0x04;  // フラグ立てる
            // Serial.printf("キーオフ無効\n");
            break;
          }
          case 0xf6: {  // リピート開始 ($F6 [回数] [作業用カウンター])
            if (ndFile.size - tracks[i].pc < 2) {
              tracks[i].pc = parseDataEnd;
              break;
            }
            // MXDRV: MDX のメモリ上に回数をコピーする。曲の再読込で元データに戻る。
            const u8_t times = ndFile.get_ui8_at(tracks[i].pc++);
            ndFile.data[tracks[i].pc++] = times;
            break;
          }
          case 0xf5: {  // リピート終端 ($F5 [オフセット(16bit)])
            if (ndFile.size - tracks[i].pc < 2) {
              tracks[i].pc = parseDataEnd;
              break;
            }
            const s16_t offset = (s16_t)ndFile.get_ui16_be_at(tracks[i].pc);
            tracks[i].pc += 2;
            const s32_t target = (s32_t)tracks[i].pc + offset;
            // 分岐先の直前がカウンター。F6 を経由しない分岐や正方向の offset も扱う。
            if (target <= 0 || (u32_t)target > ndFile.size) {
              tracks[i].pc = parseDataEnd;
              break;
            }
            u8_t& counter = ndFile.data[target - 1];
            counter = (u8_t)(counter - 1);  // 0 は 255 に戻るため、初期値 0 なら 256 回。
            if (counter != 0) {
              // ND の曲周回判定は維持する。
              const bool followedByTrackEnd =
                  (tracks[i].pc + 1 < parseDataEnd && ndFile.get_ui8_at(tracks[i].pc) == 0xf1 &&
                   ndFile.get_ui8_at(tracks[i].pc + 1) == 0x00);
              if (followedByTrackEnd) {
                countTrackLoop(i);
              }
              tracks[i].pc = (u32_t)target;
            }
            break;
          }
          case 0xf4: {  // リピート脱出 ($F4 [オフセット(16bit)])
            if (ndFile.size - tracks[i].pc < 2) {
              tracks[i].pc = parseDataEnd;
              break;
            }
            const s16_t forward = (s16_t)ndFile.get_ui16_be_at(tracks[i].pc);
            tracks[i].pc += 2;
            // F4 の参照先は F5 のオペランド。そこにある offset から残り回数を読む。
            const s32_t operand = (s32_t)tracks[i].pc + forward;
            if (operand < 0 || (u32_t)operand + 1 >= ndFile.size) {
              tracks[i].pc = parseDataEnd;
              break;
            }
            const u32_t afterRepeat = (u32_t)operand + 2;
            const s16_t offset = (s16_t)ndFile.get_ui16_be_at((u32_t)operand);
            const s32_t target = (s32_t)afterRepeat + offset;
            if (target <= 0 || (u32_t)target > ndFile.size) {
              tracks[i].pc = parseDataEnd;
              break;
            }
            if (ndFile.get_ui8_at((u32_t)target - 1) == 1) {
              tracks[i].pc = afterRepeat;
            }
            break;
          }
          case 0xf3: {  // デチューン
            tracks[i].D = (s16_t)ndFile.get_ui16_be_at(tracks[i].pc);
            tracks[i].pc += 2;
            // Serial.printf("ch%d 0x%x デチューン %d\n", i, pos, tracks[i].D);
            break;
          }
          case 0xf2: {  // ポルタメント : L0013c6
            s16_t portament = (s16_t)ndFile.get_ui16_be_at(tracks[i].pc);
            tracks[i].pc += 2;

            tracks[i].bendDelta = (u32_t)((s32_t)portament << 8);
            tracks[i].flagsB3 |= 0x80;

            // Serial.printf("ポルタメント: %d\n", portament);
            break;
          }
          case 0xf1: {  // データエンド
            u8_t nextByte = ndFile.get_ui8_at(tracks[i].pc);
            if (nextByte == 0x00) {  // 演奏終了
              tracks[i].pc++;
              tracks[i].active = false;
              KeyBoard.trackPan[i] = PAN_MUTE;  // トラック終了表示
              tracks[i].flagsB3 &= ~0x08;
              setTrackDisplayLevel(i, 0);
              _playingTracks &= ~(1 << i);
              // Serial.printf("ch%d 終了\n", i);
            } else {
              s16_t offset = (s16_t)ndFile.get_ui16_be_at(tracks[i].pc);
              tracks[i].pc += 2;
              tracks[i].pc += offset;

              // 0xF1 ジャンプしてるハックデータのループカウント処理
              bool allowLoopCount = (offset < 0);
              if (mxdrv16y) {
                allowLoopCount = allowLoopCount && tracks[i].hasTimedEvent;
              }
              if (allowLoopCount) {
                countTrackLoop(i);
              }
              // Serial.printf("ch%d ループ\n", i);
            }
            break;
          }
          case 0xf0: {  // キーオンディレイ
            u8_t delay = ndFile.get_ui8_at(tracks[i].pc++);
            // set key-on delay (S001f)
            tracks[i].keyOnDelay = delay;
            tracks[i].keyOnDelayCounter = 0;
            tracks[i].keyOnPending = false;
            // Serial.printf("キーオンディレイ: %d\n", delay);
            break;
          }
          case 0xef: {  // 同期信号送出
            // 0xef n
            // Sync send on channel n. If channel n is in Sync Wait, resume
            // playback on that channel. MML command S#
            u8_t sync = ndFile.get_ui8_at(tracks[i].pc++);
            if (sync < trackCount) {
              if (tracks[sync].syncWait) {
                tracks[sync].syncWait = false;
                tracks[sync].waitTicks = 0;
              }
            }
            break;
          }
          case 0xee: {  // 同期信号待機
            tracks[i].syncWait = true;
            tracks[i].waitTicks = 1;
            break;
          }
          case 0xed: {  // ADPCM/ノイズ周波数設定
            u8_t freq = ndFile.get_ui8_at(tracks[i].pc++);
            if (i < 8) {
              // bit7 のノイズ有効指定も含め、引数をそのまま OPM に渡す (portable_mdx L0014dc)。
              FM.setRegisterOPM(0x0f, freq, 0);
            } else {
              if (!MDX.pcm8) {
                // ADPCM 周波数
                switch (freq) {
                  case 0: {
                    // 3.9KHz
                    ND::freq[1] = SI5351_4000;
                    SI5351.setFreq(ND::freq[1], 1);
                    okim6258.state.divider = OKIM6258_DIV_1024;
                    FM.setOKIM6258divider(okim6258.state.divider);
                    break;
                  }
                  case 1: {
                    // 5.2KHz
                    ND::freq[1] = SI5351_4000;
                    SI5351.setFreq(ND::freq[1], 1);
                    okim6258.state.divider = OKIM6258_DIV_768;
                    FM.setOKIM6258divider(okim6258.state.divider);
                    break;
                  }
                  case 2: {
                    // 7.8KHz
                    ND::freq[1] = SI5351_8000;
                    SI5351.setFreq(ND::freq[1], 1);
                    okim6258.state.divider = OKIM6258_DIV_1024;
                    FM.setOKIM6258divider(okim6258.state.divider);
                    break;
                  }
                  case 3: {
                    // 10.4KHz
                    ND::freq[1] = SI5351_8000;
                    SI5351.setFreq(ND::freq[1], 1);
                    okim6258.state.divider = OKIM6258_DIV_768;
                    FM.setOKIM6258divider(okim6258.state.divider);
                    break;
                  }
                  case 4: {
                    // 15.6KHz
                    ND::freq[1] = SI5351_8000;
                    SI5351.setFreq(ND::freq[1], 1);
                    okim6258.state.divider = OKIM6258_DIV_512;
                    FM.setOKIM6258divider(okim6258.state.divider);
                    break;
                  }
                }
              } else {
                // PCM8A F0..F12
                // F13以降 (PCM8++) は未対応
                const u8_t kPcm8aModeCount = sizeof(PCM8A_MODE_TABLE) / sizeof(PCM8A_MODE_TABLE[0]);
                if (freq >= kPcm8aModeCount) {
                  break;
                }
                const Pcm8aMode& mode = PCM8A_MODE_TABLE[freq];
                const u8_t dataKind = mode.dataKind;
                if (dataKind == 5 && tracks[i].pcmDataKind != 5) {
                  Serial.println("16-bit PCM PDX.");
                }
                portENTER_CRITICAL(&okim6258Mux);
                tracks[i].pcmRateStep = mode.rateStep;
                tracks[i].pcmRateCounter = 0;
                tracks[i].pcmDataKind = dataKind;
                portEXIT_CRITICAL(&okim6258Mux);
              }
            }

            break;
          }
          case 0xec: {                  // 音程 LFO
            tracks[i].flagsB3 |= 0x20;  // mxdrv: S0016 bit5 (音程 LFO)
            u8_t nextByte = ndFile.get_ui8_at(tracks[i].pc);
            if (nextByte & 0x80) {
              tracks[i].pc++;
              if (nextByte & 0x01) {
                tracks[i].pitchLFOLengthCounter = tracks[i].pitchLFOLengthCooked;
                tracks[i].pitchLFODelta = tracks[i].pitchLFODeltaStart;
                tracks[i].pitchLFOOffset = tracks[i].pitchLFOOffsetStart;
                // Serial.printf("音程 LFO ON\n");
              } else {
                tracks[i].flagsB3 &= ~0x20;
                tracks[i].pitchLFOOffset = 0;
                // Serial.printf("音程 LFO OFF\n");
              }
            } else {
              u8_t waveform = ndFile.get_ui8_at(tracks[i].pc++);
              u16_t freq = ndFile.get_ui16_be_at(tracks[i].pc);
              tracks[i].pc += 2;
              u16_t amp = ndFile.get_ui16_be_at(tracks[i].pc);
              tracks[i].pc += 2;

              u8_t type = waveform & 0x03;
              u8_t mode = (u8_t)(type << 1);

              tracks[i].pitchLFOType = (u32_t)(type + 1);
              tracks[i].pitchLFOLength = freq;

              u16_t cooked = freq;
              if (mode != 0x02) {
                cooked >>= 1;
                if (mode == 0x06) {
                  cooked = 1;
                }
              }
              tracks[i].pitchLFOLengthCooked = cooked;

              s32_t delta = (s16_t)amp;
              delta <<= 8;
              u8_t waveCheck = waveform;
              if (waveCheck >= 0x04) {
                delta <<= 8;
                waveCheck &= 0x03;
              }
              tracks[i].pitchLFODeltaStart = (u32_t)delta;
              if (waveCheck == 0x02) {
                tracks[i].pitchLFOOffsetStart = (u32_t)delta;
              } else {
                tracks[i].pitchLFOOffsetStart = 0;
              }

              tracks[i].pitchLFOLengthCounter = tracks[i].pitchLFOLengthCooked;
              tracks[i].pitchLFODelta = tracks[i].pitchLFODeltaStart;
              tracks[i].pitchLFOOffset = tracks[i].pitchLFOOffsetStart;
              // Serial.printf("音程 LFO: wave %d, freq %d, amp %d\n", waveform,
              // freq, amp);
            }
            break;
          }
          case 0xeb: {                  // 音量 LFO
            tracks[i].flagsB3 |= 0x40;  // mxdrv: S0016 bit6 (音量 LFO)
            u8_t nextByte = ndFile.get_ui8_at(tracks[i].pc);
            if (nextByte & 0x80) {
              tracks[i].pc++;
              if (nextByte & 0x01) {
                tracks[i].volumeLFOLengthCounter = tracks[i].volumeLFOLength;
                tracks[i].volumeLFODelta = tracks[i].volumeLFODeltaStart;
                tracks[i].volumeLFOOffset = tracks[i].volumeLFODeltaCoocked;
                // Serial.printf("Volume LFO ON\n");
              } else {
                tracks[i].flagsB3 &= ~0x40;
                tracks[i].volumeLFOOffset = 0;
                // Serial.printf("音量 LFO OFF\n");
              }
            } else {
              u8_t waveform = ndFile.get_ui8_at(tracks[i].pc++);
              u16_t freq = ndFile.get_ui16_be_at(tracks[i].pc);
              tracks[i].pc += 2;
              u16_t amp = ndFile.get_ui16_be_at(tracks[i].pc);
              tracks[i].pc += 2;

              u8_t mode = (u8_t)(waveform << 1);

              tracks[i].volumeLFOType = (u32_t)(waveform + 1);
              tracks[i].volumeLFOLength = freq;
              tracks[i].volumeLFODeltaStart = amp;

              s32_t cooked = (s16_t)amp;
              if ((mode & 0x02) == 0) {
                cooked = cooked * (s16_t)freq;
              }
              cooked = -cooked;
              if (cooked < 0) {
                cooked = 0;
              }
              tracks[i].volumeLFODeltaCoocked = (u16_t)cooked;

              tracks[i].volumeLFOLengthCounter = tracks[i].volumeLFOLength;
              tracks[i].volumeLFODelta = tracks[i].volumeLFODeltaStart;
              tracks[i].volumeLFOOffset = tracks[i].volumeLFODeltaCoocked;
              // Serial.printf("音量 LFO: wave %d, freq %d, amp %d\n", waveform,
              // freq, amp);
            }
            break;
          }
          case 0xea: {  // OPM LFO
            // Serial.printf("ch%d, OPMLFO\n", i);
            u8_t d2 = ndFile.get_ui8_at(tracks[i].pc++);
            if (d2 & 0x80) {
              d2 &= 0x01;
              if (i < 8) {
                u8_t fmCh = getFmChannel(tracks[i], i);
                FM.setRegisterOPM(0x38 + fmCh, d2 ? tracks[i].PmsAms : 0x00, 0);
              }
            } else {
              tracks[i].flagsB3 &= ~0x02;
              if (d2 & 0x40) {
                tracks[i].flagsB3 |= 0x02;
              }
              d2 &= (u8_t)~0x40;
              d2 |= (FM.ym2151_reg[0x1B] & 0xC0);
              u8_t lfrq = ndFile.get_ui8_at(tracks[i].pc++);
              u8_t pmd = ndFile.get_ui8_at(tracks[i].pc++);
              u8_t amd = ndFile.get_ui8_at(tracks[i].pc++);
              u8_t pmsams = ndFile.get_ui8_at(tracks[i].pc++);
              tracks[i].PmsAms = pmsams;
              if (i < 8) {
                u8_t fmCh = getFmChannel(tracks[i], i);
                FM.setRegisterOPM(0x1B, d2, 0);
                FM.setRegisterOPM(0x18, lfrq, 0);
                FM.setRegisterOPM(0x19, pmd, 0);
                FM.setRegisterOPM(0x19, amd, 0);
                FM.setRegisterOPM(0x38 + fmCh, pmsams, 0);
              }
            }
            break;
          }
          case 0xe9: {  // LFOディレイ設定
            tracks[i].LFODelay = ndFile.get_ui8_at(tracks[i].pc++);
            break;
          }
          case 0xe8: {  // PCM4/8使用宣言
            // Serial.printf("PCM4/8使用宣言\n");
            break;
          }
          case 0xe7: {  // 拡張MMLコマンド
            u8_t command_e7 = ndFile.get_ui8_at(tracks[i].pc++);
            switch (command_e7) {
              case 0x00: {
                // Serial.printf("拡張MMLコマンド: ERROR\n");
                break;
              }
              case 0x01: {
                u8_t fadeout = ndFile.get_ui8_at(tracks[i].pc++);
                //
                fadeoutLengh = (u32_t)(((u64_t)_interval * ((fadeout >> 1) + 2) * 63) / 1000);
                nju72342.startMdxFadeout(fadeoutLengh);
                Serial.printf("拡張MMLコマンド: フェードアウト %d, %d ms\n", fadeout, fadeoutLengh);
                break;
              }
              case 0x02: {
                // 仕様: [$02]b + [d0.w] + [d1.l] = 6byteスキップ
                tracks[i].pc += 6;
                // u8_t d0w = ndFile.get_ui8_at(tracks[i].pc++);
                // u8_t d1l = ndFile.get_ui8_at(tracks[i].pc++);
                // Serial.printf("拡張MMLコマンド: PCM8直接ドライブ 0x%x
                // 0x%x\n", d0w, d1l);
                break;
              }
              case 0x03: {
                u8_t flag = ndFile.get_ui8_at(tracks[i].pc++);
                // Serial.printf("拡張MMLコマンド: キーオフする %d\n", flag);
                break;
              }
              case 0x04: {
                u8_t ch = ndFile.get_ui8_at(tracks[i].pc++);
                // Serial.printf("拡張MMLコマンド: 他チャンネル制御 ch%d\n",
                // ch);
                break;
              }
              case 0x05: {
                u8_t data = ndFile.get_ui8_at(tracks[i].pc++);
                // Serial.printf("拡張MMLコマンド: 音長加算 %d\n", data);
                break;
              }
              case 0x06: {
                u8_t flag = ndFile.get_ui8_at(tracks[i].pc++);
                // Serial.printf("拡張MMLコマンド: フラグ未使用か %d\n", flag);
                break;
              }
              case 0x0a: {
                u8_t flag = ndFile.get_ui8_at(tracks[i].pc++);
                break;
              }
              default: {
                Serial.printf("拡張MMLコマンド0xe7: 未定義 sub=0x%x\n", command_e7);
                break;
              }
            }
            break;
          }
          case 0xe6: {  // 拡張MMLコマンド2
            u8_t command_e6 = ndFile.get_ui8_at(tracks[i].pc++);
            switch (command_e6) {
              case 0x00:
                // ERROR 強制終了
                break;
              case 0x01: {  // 相対ディチューン
                u16_t det = ndFile.get_ui16_be_at(tracks[i].pc);
                tracks[i].pc += 2;
                break;
              }
              case 0x02: {  // 移調 (-127～127)
                s8_t trans = (int8_t)ndFile.get_ui8_at(tracks[i].pc++);
                tracks[i].transpose = trans;
                break;
              }
              case 0x03: {  // 相対移調 (-127～127)
                s8_t trans = (int8_t)ndFile.get_ui8_at(tracks[i].pc++);
                s16_t next = (s16_t)tracks[i].transpose + (s16_t)trans;
                if (next > 127) {
                  next = 127;
                } else if (next < -127) {
                  next = -127;
                }
                tracks[i].transpose = (s8_t)next;
                break;
              }
              default: {
                Serial.printf("拡張MMLコマンド0xe6: 未定義 sub=0x%x\n", command_e6);
                break;
              }
            }
            break;
          }
        }
      }
    }
  }
  if (usePcm8Shadow && pcm8StateDirty) {
    portENTER_CRITICAL(&okim6258Mux);
    for (int ch = 0; ch < 8; ch++) {
      if (pcm8VolumeDirty[ch]) {
        okim6258.state.adpcmVolume[ch] = pcm8Shadow.adpcmVolume[ch];
      }
      if (pcm8TransportDirty[ch]) {
        okim6258.state.playSerial[ch] = pcm8Shadow.playSerial[ch];
        okim6258.state.currentBlockId[ch] = pcm8Shadow.currentBlockId[ch];
        okim6258.state.posInBlock[ch] = pcm8Shadow.posInBlock[ch];
        okim6258.state.blockStartPos[ch] = pcm8Shadow.blockStartPos[ch];
        okim6258.state.blockSizeBytes[ch] = pcm8Shadow.blockSizeBytes[ch];
        okim6258.state.adpcmBlockVolume[ch] = pcm8Shadow.adpcmBlockVolume[ch];
        okim6258.state.pcmDataKind[ch] = pcm8Shadow.pcmDataKind[ch];
        okim6258.state.hold[ch] = pcm8Shadow.hold[ch];
      } else if (pcm8HoldDirty[ch]) {
        okim6258.state.hold[ch] = pcm8Shadow.hold[ch];
      }
      if (pcm8RateResetDirty[ch]) {
        tracks[8 + ch].pcmRateCounter = 0;
      }
    }
    portEXIT_CRITICAL(&okim6258Mux);
  }
}

void MDXClass::_endProcedure() {
  if (pcm8 && pdxLoaded) {
    nju72342.panMute();
  }
  ND::canPlay = false;
  if (Leds.ready()) {
    Leds.set(MDX_BEAT_LED, 0);
  }

  delay(8);

  switch (ndConfig.get(CFG_REPEAT)) {
    case REPEAT_ONE: {
      ndFile.filePlay(0);
      break;
    }
    case REPEAT_FOLDER: {
      ndFile.filePlay(1);
      break;
    }
    case REPEAT_ALL: {
      if (fileTree.getNextFileNode(ndFile.currentNode, false) == nullptr)
        ndFile.dirPlay(1);
      else
        ndFile.filePlay(1);
      break;
    }
  }
}

u64_t MDXClass::getCurrentTimeSec() {
  return (millis() - _startTick) / 1000;
}

u64_t MDXClass::getCurrentTimeSubSec() {
  return (millis() - _startTick) / 100;
}
