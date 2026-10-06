/**
 * NanoDrive MDX Parser
 * (c) 2025 - 2026 Fujix
 *
 * MDX command definitions at
 *  https://w.atwiki.jp/mxdrv/pages/23.html
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
 * X68Sound_src020615
 *      Copyright (C) m_puusan.
 */

#ifndef MDX_H
#define MDX_H

#include <Arduino.h>

#include "keyinfo.h"

struct MDXTrackState {
  u32_t address;  // トラック開始アドレス
  // u32_t dataLength;     // トラックデータ長
  u16_t waitTicks;      // 待ち時間カウント
  bool syncWait;        // sync wait (W)
  bool active;          // 演奏中
  u16_t keyOffTicks;    // S001b (gate) counter, decremented in L0011b4

  u8_t con_fl;        // 音色設定時のときのCON/FL
  bool keyOnPending;  // keyon is pending until counter reaches 0
  u8_t tl[4];         // オペレータ毎の元のTL

  u32_t pc;  // S0000 Ptrプログラムカウンタ
  // u32_t voicePtr;        // S0004 PCM ポインタ
  u8_t PCMBank;          // S0004_b PCMバンク
  u32_t bendDelta;       // S0008 ポルタメント/ベンド
  u32_t bendOffset;      // S000c
  int16_t D;             // S0010 デチューン
  s8_t transpose;        // 移調 (-127..127)
  u16_t noteD;           // S0012 note+D
  u16_t noteDBendPitch;  // S0014 LFO offset
  int16_t baseNote;      // base note (0..95), -1=no note
  u8_t flagsB3;          // S0016 flags
                         //       bit 7: pitch bend enable
                         //       bit 3: key-on active (L000e7e)
                         //       bit 5: pitch LFO enable
                         //       bit 6: volume LFO enable
                         //       bit 2: key-off disable ($f7)
                         //       bit 1: OPL LFO enable
  u8_t flags;            // S0017 flags
                         //       bit 1: voice update pending
                         //       bit 2: pan update pending
  // u8_t ch;                       // S0018 ch PCM のとき 0x80
  u8_t carrierSlot;  // S0019 carrier slog
  // u8_t gate;               // S001b gate (L0011b4)
  u8_t pan;                // S001c p パン
  u8_t adpcmPan;           // ADPCM pan (NJU72342 tPan)
  u8_t fmChannel;          // FM出力チャンネル(通常は trackNo と同じ)
  u8_t voiceNo;            // 最後に指定された音色番号(FD)
  u8_t keyonSlot;          // S001d keyon slot
  u8_t gateTime;           // S001e Q
  u8_t keyOnDelay;         // S001f keyon delay (ticks)
  u8_t keyOnDelayCounter;  // S0020 keyon delay counter
  u8_t PmsAms;             // S0021
  u8_t volume;             // S0022
  bool adpcmHold;          // ADPCM: key-off disabled hold active
  // u8_t vLast;                    // S2023
  u8_t LFODelay;                // S2024
  u8_t LFODelayCounter;         // S0025
  u32_t pitchLFOType;           // S0026
  u32_t pitchLFOOffsetStart;    // S002a
  u32_t pitchLFODeltaStart;     // S002e
  u32_t pitchLFODelta;          // S0032
  u32_t pitchLFOOffset;         // S0036
  u16_t pitchLFOLengthCooked;   // S003a
  u16_t pitchLFOLength;         // S003c
  u16_t pitchLFOLengthCounter;  // S003e
  u32_t volumeLFOType;          // S0040
  u16_t volumeLFODeltaStart;    // S0044
  u16_t volumeLFODeltaCoocked;  // S0046
  u16_t volumeLFODelta;         // S0048
  u16_t volumeLFOOffset;        // S004a
  // u16_t volumeLFOLengthCooked;   // S004b
  u16_t volumeLFOLength;         // S004c
  u16_t volumeLFOLengthCounter;  // S004e
  u32_t pcmRateCounter;          // Q16.16
  u32_t pcmRateStep;             // Q16.16
  u8_t pcmDataKind;              // PCM8A data: 4 ADPCM, 5 16bit PCM, 6 8bit PCM

  u16_t trackLoops;
  bool countForSongEnd;
  bool hasTimedEvent;

  // 初期化
  void reset(int i) {
    address = 0x00;
    // dataLength = 0;
    pc = 0;       // S0000
    PCMBank = 0;  // S0002b
    D = 0;        // S0010
    transpose = 0;
    noteDBendPitch = 0xffff;  // S0014
    flagsB3 = 0;              // S0016
    baseNote = -1;
    flags = 0;  // S0017
    // ch = (i < 9) ? i : 0x80 | (i - 9);  // S0018 FM と PCM の種別
    carrierSlot = 0;              // S0019
    keyonSlot = 0;                // S001d
    pan = (i < 9) ? 0xc0 : 0x10;  // S001c, FM 0xc0 左右 0b1100 0000,
                                  //        PCM 0x10
    adpcmPan = 0x00;              // PAN_CENTER
    fmChannel = (i < 8) ? i : 0xff;
    voiceNo = 0xff;
    gateTime = 8;    // S001e Q
    keyOnDelay = 0;  // S001f
    volume = 8;      // S0022
    // vLast = 0xff;                 // S2023
    LFODelay = 0;         // S0024
    pitchLFOType = 0;     // S0026
    pitchLFOOffset = 0;   // S0036
    volumeLFOType = 0;    // S0040
    volumeLFOOffset = 0;  // S004a

    waitTicks = 0;
    syncWait = false;
    keyOffTicks = 0;
    active = false;

    keyOnDelayCounter = 0;
    keyOnPending = false;

    bendDelta = 0;
    bendOffset = 0;
    con_fl = 0;
    PmsAms = 0;
    adpcmHold = false;

    pcmRateCounter = 0;
    pcmRateStep = 0x10000;  // 15.6kHz
    pcmDataKind = 4;

    trackLoops = 0;
    countForSongEnd = false;
    hasTimedEvent = false;
  }
};

class MDXClass {
 public:
  void init();         // 初期化、タイマ設定
  bool ready();        // MDX 再生準備
  void startTimer();   // タイマ開始
  void stopTimer();    // タイマ停止
  void process();      // 処理進行
  void processTick();  // Tick処理

  String title;  // 曲名
  String subTitle;

  u32_t voiceDataOffset;     // 音色データ開始アドレス
  u8_t voiceTable[256];      // 音色データの変換テーブル　
  u8_t trackCount;           // トラック数 9 か 16
  MDXTrackState tracks[16];  // トラックステート

  u8_t _tempo;  // L001e0c @t
  // u8_t L001e14;  // MXDRV: global volume offset (fade) 使わない
  int fadeoutLengh;  // 秒換算済み -1 無効

  // u32_t L002228;  // voice data

  u64_t _nextTick;
  u64_t _startTick;
  u32_t parseDataEnd;
  u16_t _interval;  // 発火周期 us
  u16_t _beatTickCounter;
  u8_t _beatPulseTicks;

  u16_t _playingTracks;  // プレイ中トラックビット（曲終了検知）
  bool pdxLoaded;
  bool pcm8;      // pcm4/8モード
  volatile bool adpcmWarmupActive;  // 曲開始前のMDX ADPCM無音送信をISRに許可
  bool mxdrv16y;  // mxdrv16yモード
  bool isLZX;     // LZ圧縮MDXを展開して再生中
  u8_t adpcmGlobalPan;

  u64_t getCurrentTimeSec();
  u64_t getCurrentTimeSubSec();
  u16_t getTempoBpm() const;
  bool hasExplicitTempo() const;

 private:
  volatile bool _hasExplicitTempo = false;
  void _applyPendingFmState(u8_t trackNo);
  void _setVoice(u8_t trackNo, u8_t voiceNo);  // 音色設定
  void _setVolume(u8_t trackNo);               // 音量設定
  void _setTempo(u8_t tempo);                  // タイマ周期更新
  void _updateBeatLed();
  void _endProcedure();  // 演奏終了処理
};

extern MDXClass MDX;

#endif

#ifndef USE_MDX
#define USE_MDX
#endif
