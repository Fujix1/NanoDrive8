/**
 * Nano Drive OKI MSM6258 Controller v1.0
 * (c) 2026 Fujix
 *
 * OKI M6258 codec logic derived from MAME at
 * https://github.com/mamedev/mame/blob/master/src/devices/sound/okim6258.cpp
 * license:BSD-3-Clause
 * copyright-holders:Barry Rodewald
 *
 */

#include "okim6258.h"

#include "config.h"
#include "file.h"
#include "fm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static TaskHandle_t hPCMEncode = NULL;
static void taskPCMEncode(void* pvParameters);
static u8_t buildPcm8AdpcmByte();
static void fillPcmOutputQueue();
static u8_t buildVgmAdpcmByte();
static void fillVgmOutputQueue();

struct Pcm8LowPassCoefficients {
  float b0;
  float b1;
  float b2;
  float b3;
  float a1;
  float a2;
  float a3;
};

struct Pcm8HighPassCoefficients {
  float b0;
  float a1;
};

// X68000実回路のLPF利得へ合わせた3次IIR。実回路は約7.50kHzの1極と、
// 約3.76kHz/Q=0.79の2極で近似できる。Nyquistに強制ゼロを置かず、
// 8MHzクロック時の各OKIM6258サンプルレートで実回路の利得へ直接フィットした。
constexpr Pcm8LowPassCoefficients Pcm8LpfDiv1024 = {0.923442900f, 0.414558421f, -0.089510489f,
                                                    0.005842403f, 0.313071062f, -0.069654530f,
                                                    0.010916704f};
constexpr Pcm8LowPassCoefficients Pcm8LpfDiv768 = {0.804478093f, 0.696917325f, 0.108619066f,
                                                   0.009198081f, 0.522773632f, 0.055804592f,
                                                   0.040634341f};
constexpr Pcm8LowPassCoefficients Pcm8LpfDiv512 = {0.548918487f, 0.564018086f, 0.141551943f,
                                                   0.011922191f, 0.201042424f, -0.019938263f,
                                                   0.085306545f};

// 183Hz 1st-order HPF. Bilinear-transform coefficients for the same three
// OKIM6258 sample rates. y[n] = b0 * (x[n] - x[n-1]) + a1 * y[n-1].
constexpr Pcm8HighPassCoefficients Pcm8HpfDiv1024 = {0.931339952f, 0.862679904f};
constexpr Pcm8HighPassCoefficients Pcm8HpfDiv768 = {0.947644887f, 0.895289774f};
constexpr Pcm8HighPassCoefficients Pcm8HpfDiv512 = {0.964495990f, 0.928991979f};

// 異常VGMブロックのDC偏りだけを除く20Hz 1st-order HPF。
// X68000回路再現用183Hz HPFとは独立し、異常判定されたブロックだけに適用する。
constexpr Pcm8HighPassCoefficients VgmDcHpfDiv1024 = {0.992021518f, 0.984043035f};
constexpr Pcm8HighPassCoefficients VgmDcHpfDiv768 = {0.994004235f, 0.988008470f};
constexpr Pcm8HighPassCoefficients VgmDcHpfDiv512 = {0.995994845f, 0.991989691f};

static float pcm8LpfState1 = 0.0f;
static float pcm8LpfState2 = 0.0f;
static float pcm8LpfState3 = 0.0f;
static float pcm8HpfState = 0.0f;
static float vgmDcHpfState = 0.0f;
static bool vgmDcRemovalActive = false;

static int vgmDecodeSignal = -2;
static int vgmDecodeStepIndex = 0;
static constexpr u32_t VgmDcDetectionSamples = 128;
static constexpr int VgmDcDetectionThreshold = 1024;
static constexpr u32_t VgmRouteSwitchMuteUs = 5000;

static int readAdpcmConfigMode() {
  int mode = ndConfig.get(CFG_ADPCM);
  if (mode < ADPCM_THROUGH || mode > ADPCM_LPF) {
    mode = ADPCM_THROUGH;
  }
  return mode;
}

static void applyAdpcmFilterNjuGain(bool filterEnabled) {
  const tNJU7234X_GAIN gain = filterEnabled ? GAIN3 : GAIN0;
  // NJU72342 ch1/ch2はOKIM6258の左右経路。X68フィルタ時だけ入力を+3dBにする。
  // メイン出力ch3/ch4とPAN出力レベルには触れない。
  nju72342.setInputGain(1, gain);
  nju72342.setInputGain(2, gain);
}

static void resetAdpcmFilterState() {
  pcm8LpfState1 = 0.0f;
  pcm8LpfState2 = 0.0f;
  pcm8LpfState3 = 0.0f;
  pcm8HpfState = 0.0f;
}

static void resetVgmDcRemovalState() {
  vgmDcHpfState = 0.0f;
  vgmDcRemovalActive = false;
}

static inline __attribute__((always_inline)) int32_t filterPcm8Sample(int32_t sample,
                                                                      tOKIM6258Divider divider) {
  const Pcm8LowPassCoefficients* coefficients = &Pcm8LpfDiv512;
  if (divider == OKIM6258_DIV_1024) {
    coefficients = &Pcm8LpfDiv1024;
  } else if (divider == OKIM6258_DIV_768) {
    coefficients = &Pcm8LpfDiv768;
  }

  const float input = (float)sample;
  // Transposed Direct Form II。同じ3次伝達関数を3つの状態だけで計算する。
  const float output = coefficients->b0 * input + pcm8LpfState1;
  pcm8LpfState1 = coefficients->b1 * input - coefficients->a1 * output + pcm8LpfState2;
  pcm8LpfState2 = coefficients->b2 * input - coefficients->a2 * output + pcm8LpfState3;
  pcm8LpfState3 = coefficients->b3 * input - coefficients->a3 * output;

  return (int32_t)(output + ((output >= 0.0f) ? 0.5f : -0.5f));
}

static inline __attribute__((always_inline)) void trackPcm8LowPassBypass(int32_t sample,
                                                                         tOKIM6258Divider divider) {
  const float value = (float)sample;
  const Pcm8LowPassCoefficients* coefficients = &Pcm8LpfDiv512;
  if (divider == OKIM6258_DIV_1024) {
    coefficients = &Pcm8LpfDiv1024;
  } else if (divider == OKIM6258_DIV_768) {
    coefficients = &Pcm8LpfDiv768;
  }

  // 従来形でx1..x3とy1..y3を同じ値にした状態と等価な転置形の状態。
  pcm8LpfState1 = (coefficients->b1 + coefficients->b2 + coefficients->b3 - coefficients->a1 -
                   coefficients->a2 - coefficients->a3) *
                  value;
  pcm8LpfState2 =
      (coefficients->b2 + coefficients->b3 - coefficients->a2 - coefficients->a3) * value;
  pcm8LpfState3 = (coefficients->b3 - coefficients->a3) * value;
}

static inline __attribute__((always_inline)) int32_t
filterPcm8HighPassSample(int32_t sample, tOKIM6258Divider divider) {
  const Pcm8HighPassCoefficients* coefficients = &Pcm8HpfDiv512;
  if (divider == OKIM6258_DIV_1024) {
    coefficients = &Pcm8HpfDiv1024;
  } else if (divider == OKIM6258_DIV_768) {
    coefficients = &Pcm8HpfDiv768;
  }

  const float input = (float)sample;
  const float scaledInput = coefficients->b0 * input;
  const float output = scaledInput + pcm8HpfState;
  pcm8HpfState = -scaledInput + coefficients->a1 * output;

  return (int32_t)(output + ((output >= 0.0f) ? 0.5f : -0.5f));
}

static inline __attribute__((always_inline)) int32_t filterVgmDcSample(int32_t sample,
                                                                       tOKIM6258Divider divider) {
  const Pcm8HighPassCoefficients* coefficients = &VgmDcHpfDiv512;
  if (divider == OKIM6258_DIV_1024) {
    coefficients = &VgmDcHpfDiv1024;
  } else if (divider == OKIM6258_DIV_768) {
    coefficients = &VgmDcHpfDiv768;
  }

  const float input = (float)sample;
  const float scaledInput = coefficients->b0 * input;
  const float output = scaledInput + vgmDcHpfState;
  vgmDcHpfState = -scaledInput + coefficients->a1 * output;

  return (int32_t)(output + ((output >= 0.0f) ? 0.5f : -0.5f));
}

portMUX_TYPE okim6258Mux = portMUX_INITIALIZER_UNLOCKED;

constexpr int StepTable[49] = {
    16,  17,  19,  21,  23,  25,  28,  31,  34,  37,  41,   45,   50,   55,   60,   66,  73,
    80,  88,  97,  107, 118, 130, 143, 157, 173, 190, 209,  230,  253,  279,  307,  337, 371,
    408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
};

constexpr int IndexShift[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

static inline __attribute__((always_inline)) void applyNibble(int nibble, int& signal,
                                                              int& stepIndex) {
  int step = StepTable[stepIndex];
  int diff = step >> 3;
  if (nibble & 0x01) diff += step >> 2;
  if (nibble & 0x02) diff += step >> 1;
  if (nibble & 0x04) diff += step;

  if (nibble & 0x08) {
    signal -= diff;
  } else {
    signal += diff;
  }

  if (signal > 2047) signal = 2047;
  if (signal < -2048) signal = -2048;

  stepIndex += IndexShift[nibble & 0x07];
  if (stepIndex < 0) stepIndex = 0;
  if (stepIndex > 48) stepIndex = 48;
}

struct Pcm8WorkItem {
  int blockId;
  u32_t startPos;
  u32_t sizeSamples;
  u32_t posNib0;
  u32_t posNib1;
  u32_t frac0;
  u32_t frac1;
  int32_t gain;
  u8_t pcmDataKind;
  bool pcm16Is15Khz;
  bool hasSecondSample;
};

static inline __attribute__((always_inline)) void stopPcm8Channel(OKIM6258::OKIM6258State& state,
                                                                  int ch, bool hold,
                                                                  u32_t blockSizeSamples) {
  if (hold) {
    state.posInBlock[ch] = blockSizeSamples;
    return;
  }
  state.currentBlockId[ch] = -1;
  state.posInBlock[ch] = 0;
  state.blockStartPos[ch] = 0;
  state.blockSizeBytes[ch] = 0;
  state.hold[ch] = false;
}

static inline __attribute__((always_inline)) int32_t getPcm8Sample(u8_t pcmDataKind,
                                                                   bool pcm16Is15Khz,
                                                                   u32_t startPos, u32_t pos) {
  if (pcmDataKind == 5) {
    u32_t bytePos = PDX_OFFSET + startPos + (pos << 1);
    int16_t sample = (int16_t)((ndFile.data[bytePos] << 8) | ndFile.data[bytePos + 1]);
    // PCM8A F5 (15.625kHz) の16-bit PCMは実データの12-bit振幅を維持する。
    // F8/F11の高レートPCMは16-bitフルスケールなので、12-bitへ正規化する。
    if (pcm16Is15Khz) {
      if (sample > 2047) return 2047;
      if (sample < -2048) return -2048;
      return sample;
    }
    return (int32_t)sample >> 4;
  }
  if (pcmDataKind == 6) {
    u32_t bytePos = PDX_OFFSET + startPos + pos;
    return ((int8_t)ndFile.data[bytePos]) << 4;
  }
  // 特定位置のニブルに対応する PCM サンプル取得
  const u32_t sampleIndex = startPos + (pos >> 1);
  const u32_t pcmBase = PCM_OFFSET + (sampleIndex << 2) + ((pos & 1U) << 1);
  return (int16_t)(ndFile.data[pcmBase] | (ndFile.data[pcmBase + 1] << 8));
}

static inline __attribute__((always_inline)) int32_t interpolatePcm8Sample(
    u8_t pcmDataKind, bool pcm16Is15Khz, u32_t startPos, u32_t pos, u32_t frac,
    u32_t sizeSamples, int32_t sample) {
  if (frac == 0) {
    return sample;
  }
  u32_t nextPos = pos + 1;
  if (nextPos >= sizeSamples) {
    return sample;
  }
  int32_t nextSample = getPcm8Sample(pcmDataKind, pcm16Is15Khz, startPos, nextPos);
  int32_t delta = nextSample - sample;
  // delta は -4095..4095、frac は 0..65535 なので 32-bit 符号付き乗算に収まる。
  return sample + ((delta * (int32_t)frac) >> 16);
}

static inline __attribute__((always_inline)) u8_t encodeNibbleFast(int target, int& signal,
                                                                   int& stepIndex) {
  if (target > 2047) target = 2047;
  if (target < -2048) target = -2048;

  int diff = target - signal;
  u8_t nibble = 0;
  if (diff < 0) {
    nibble = 0x08;
    diff = -diff;
  }

  int step = StepTable[stepIndex];
  if (diff >= step) {
    nibble |= 0x04;
    diff -= step;
  }
  int half = step >> 1;
  if (diff >= half) {
    nibble |= 0x02;
    diff -= half;
  }
  if (diff >= (step >> 2)) {
    nibble |= 0x01;
  }

  applyNibble(nibble, signal, stepIndex);
  return nibble & 0x0f;
}

bool OKIM6258::detectVgmBlockDcOffset(u32_t startPos, u32_t size) {
  // キャッシュモードではデータブロックを走査しない。
  if (ndFile.accessMode != ACCESS_PSRAM || size == 0 || startPos > ndFile.size ||
      size > ndFile.size - startPos) {
    return false;
  }

  // 末尾の0x80はPCMゼロではなく直前のADPCM予測値を保持するため、
  // DC偏りの判定からは除外する。
  u32_t activeSize = size;
  while (activeSize > 0 && ndFile.data[startPos + activeSize - 1] == 0x80) {
    activeSize--;
  }
  if (activeSize == 0) {
    return false;
  }

  const u32_t sampleCount = activeSize << 1;
  const u32_t detectionStart =
      (sampleCount > VgmDcDetectionSamples) ? (sampleCount - VgmDcDetectionSamples) : 0;
  int decodeSignal = -2;
  int decodeStepIndex = 0;
  int64_t tailSum = 0;
  u32_t tailSamples = 0;
  for (u32_t sampleIndex = 0; sampleIndex < sampleCount; sampleIndex++) {
    const u8_t source = ndFile.data[startPos + (sampleIndex >> 1)];
    const u8_t nibble = (source >> ((sampleIndex & 1U) << 2)) & 0x0f;
    applyNibble(nibble, decodeSignal, decodeStepIndex);
    if (sampleIndex >= detectionStart) {
      tailSum += decodeSignal;
      tailSamples++;
    }
  }

  if (tailSamples == 0) {
    return false;
  }
  const int32_t tailMean = (int32_t)(tailSum / (int64_t)tailSamples);
  return tailMean <= -VgmDcDetectionThreshold || tailMean >= VgmDcDetectionThreshold;
}

static inline __attribute__((always_inline)) int32_t clampOkiSample12(int32_t sample) {
  if (sample > 2047) return 2047;
  if (sample < -2048) return -2048;
  return sample;
}

static u8_t buildPcm8AdpcmByte() {
  int32_t mix0 = 0;
  int32_t mix1 = 0;
  tOKIM6258Divider divider;
  Pcm8WorkItem work[8] = {};

  OKIM6258::OKIM6258State& state = okim6258.state;
  portENTER_CRITICAL(&okim6258Mux);
  divider = state.divider;
  for (int ch = 0; ch < 8; ch++) {
    Pcm8WorkItem& item = work[ch];
    item.blockId = state.currentBlockId[ch];
    if (item.blockId == -1) {
      continue;
    }

    const bool hold = state.hold[ch];
    const u32_t pos = state.posInBlock[ch];
    const u8_t pcmDataKind = state.pcmDataKind[ch];
    u32_t blockSizeSamples = state.blockSizeBytes[ch] << 1;
    if (pcmDataKind == 5) {
      blockSizeSamples = state.blockSizeBytes[ch] >> 1;
    } else if (pcmDataKind == 6) {
      blockSizeSamples = state.blockSizeBytes[ch];
    }

    // 範囲チェック
    if (pos >= blockSizeSamples) {
      stopPcm8Channel(state, ch, hold, blockSizeSamples);
      item.blockId = -1;
      continue;
    }

    // ニブル0の処理
    item.startPos = state.blockStartPos[ch];
    item.sizeSamples = blockSizeSamples;
    item.pcmDataKind = pcmDataKind;
    item.posNib0 = pos;
    // オリジナルPCM8の仕様では、単音再生（PCM1）時は音量設定を無視して原音量で出力する。
    // このミキサーは gain=16 が1倍なので、PCM1の再圧縮時は固定してRAW経路と音量を揃える。
    item.gain = MDX.pcm8 ? (int32_t)state.adpcmBlockVolume[ch] : 16;

    MDXTrackState& trk = MDX.tracks[8 + ch];
    item.pcm16Is15Khz = pcmDataKind == 5 && trk.pcmRateStep == 0x10000;

    item.frac0 = trk.pcmRateCounter;                   // 進み量
    u32_t acc = trk.pcmRateCounter + trk.pcmRateStep;  // 進み位置
    u32_t adv = acc >> 16;                             // 進めるフレーム数
    trk.pcmRateCounter = acc & 0xFFFF;

    u32_t posAfter0 = pos;
    if (adv != 0) {
      u32_t newPos = pos + adv;
      posAfter0 = (newPos >= blockSizeSamples) ? blockSizeSamples : newPos;
    }

    // ニブル0の後に終端に達した場合でも、ニブル0自体は出力有効
    if (posAfter0 >= blockSizeSamples) {
      stopPcm8Channel(state, ch, hold, blockSizeSamples);
      continue;
    }

    // ニブル1の処理
    item.posNib1 = posAfter0;
    item.hasSecondSample = true;

    item.frac1 = trk.pcmRateCounter;
    acc = trk.pcmRateCounter + trk.pcmRateStep;
    adv = acc >> 16;
    trk.pcmRateCounter = acc & 0xFFFF;

    u32_t posAfter1 = posAfter0;
    if (adv != 0) {
      u32_t newPos = posAfter0 + adv;
      if (newPos >= blockSizeSamples) {
        posAfter1 = blockSizeSamples;
        stopPcm8Channel(state, ch, hold, blockSizeSamples);
      } else {
        posAfter1 = newPos;
        state.posInBlock[ch] = posAfter1;
      }
    } else {
      // adv == 0 の場合は pos 据え置き（ホールド）
      state.posInBlock[ch] = posAfter1;
    }
  }
  portEXIT_CRITICAL(&okim6258Mux);

  for (int ch = 0; ch < 8; ch++) {
    const Pcm8WorkItem& item = work[ch];
    if (item.blockId == -1) {
      continue;
    }

    // Q16.16 の小数位相を全て反映して線形補間する。
    if (item.gain != 0) {
      int32_t s0 = getPcm8Sample(item.pcmDataKind, item.pcm16Is15Khz, item.startPos, item.posNib0);
      s0 = interpolatePcm8Sample(item.pcmDataKind, item.pcm16Is15Khz, item.startPos, item.posNib0,
                                 item.frac0, item.sizeSamples, s0);
      mix0 += (s0 * item.gain);
    }
    if (item.hasSecondSample && item.gain != 0) {
      int32_t s1 = getPcm8Sample(item.pcmDataKind, item.pcm16Is15Khz, item.startPos, item.posNib1);
      s1 = interpolatePcm8Sample(item.pcmDataKind, item.pcm16Is15Khz, item.startPos, item.posNib1,
                                 item.frac1, item.sizeSamples, s1);
      mix1 += (s1 * item.gain);
    }
  }

  // 丸めのあと、OKI6258 の 12bit 入力範囲へ収める。
  // int16_t へ直接キャストすると、PCM8 多重再生時に範囲外の合成値がラップして大きく歪む。
  if (mix0 < 0x7ff8) mix0 += 8;
  if (mix1 < 0x7ff8) mix1 += 8;
  mix0 = clampOkiSample12(mix0 >> 4);
  mix1 = clampOkiSample12(mix1 >> 4);

  const bool lowRatePcm1 = !MDX.pcm8 && ND::freq[1] == SI5351_4000;
  if (okim6258.state.lowPassEnabled && !lowRatePcm1) {
    // Approximate the X68000 ADPCM output LPF before recompressing the PCM8 mix.
    mix0 = clampOkiSample12(filterPcm8Sample(mix0, divider));
    mix1 = clampOkiSample12(filterPcm8Sample(mix1, divider));
  } else if (okim6258.state.lowPassEnabled) {
    // 3.9/5.2kHz PCM1では3.7kHzがNyquist以上なのでLPFを迂回し、履歴だけ追従する。
    trackPcm8LowPassBypass(mix0, divider);
    trackPcm8LowPassBypass(mix1, divider);
  }

  if (okim6258.state.lowPassEnabled) {
    // 実機のADPCM出力段にあるカップリング由来の約183Hz HPFも近似する。
    mix0 = clampOkiSample12(filterPcm8HighPassSample(mix0, divider));
    mix1 = clampOkiSample12(filterPcm8HighPassSample(mix1, divider));
  }

  return okim6258.encodePair((int16_t)mix0, (int16_t)mix1);
}

static bool getVgmSourceByte(u8_t& value, bool& dcRemoval) {
  OKIM6258::OKIM6258State& state = okim6258.state;
  dcRemoval = false;

  bool logicalPlaying;
  portENTER_CRITICAL(&okim6258Mux);
  logicalPlaying = state.isPlaying;
  portEXIT_CRITICAL(&okim6258Mux);
  if (!logicalPlaying) {
    return false;
  }

  // レジスタ0x01で列挙されたデータを、従来経路と同様にブロックより優先する。
  if (state.cache.pop(value)) {
    return true;
  }

  // 大容量VGMのACCESS_CACHEではランダムアクセスを行わない。
  if (ndFile.accessMode != ACCESS_PSRAM) {
    return false;
  }

  int blockId = -1;
  u32_t pos = 0;
  u32_t startPos = 0;
  u32_t size = 0;
  portENTER_CRITICAL(&okim6258Mux);
  blockId = state.currentBlockId[0];
  if (blockId != -1 && (size_t)blockId < okim6258.dataBlocks.size()) {
    size = okim6258.dataBlocks[blockId].size;
    startPos = okim6258.dataBlocks[blockId].startPos;
    pos = state.posInBlock[0];
    if (pos < size && startPos <= ndFile.size && pos < ndFile.size - startPos) {
      state.posInBlock[0] = pos + 1;
    } else {
      state.currentBlockId[0] = -1;
      blockId = -1;
    }
  } else {
    blockId = -1;
  }
  portEXIT_CRITICAL(&okim6258Mux);

  if (blockId == -1) {
    return false;
  }

  if ((size_t)blockId < okim6258.vgmBlockDcRemoval.size()) {
    dcRemoval = okim6258.vgmBlockDcRemoval[blockId] != 0;
  }
  value = ndFile.data[startPos + pos];
  return true;
}

static u8_t buildVgmAdpcmByte() {
  int32_t sample0 = 0;
  int32_t sample1 = 0;
  u8_t source = 0;
  bool dcRemoval = false;
  bool logicalPlaying = false;
  const bool sourceAvailable = getVgmSourceByte(source, dcRemoval);
  if (sourceAvailable) {
    applyNibble(source & 0x0f, vgmDecodeSignal, vgmDecodeStepIndex);
    sample0 = vgmDecodeSignal;
    applyNibble((source >> 4) & 0x0f, vgmDecodeSignal, vgmDecodeStepIndex);
    sample1 = vgmDecodeSignal;
  } else {
    portENTER_CRITICAL(&okim6258Mux);
    logicalPlaying = okim6258.state.isPlaying;
    portEXIT_CRITICAL(&okim6258Mux);
    if (logicalPlaying) {
      // RAW経路の入力待ち0x80と同様、PLAY中は直前のADPCM予測値を保持する。
      // PCMゼロへ戻すと、レジスタ0x01の逐次送信間隔ごとに不要なパルスが発生する。
      sample0 = vgmDecodeSignal;
      sample1 = vgmDecodeSignal;
    }
  }

  // VGMの次ブロック開始までに数byteの空きがあっても、復号器のDC保持値を直送しない。
  // 明示停止までは同じHPFへ通し続け、補正済みの終端から自然にゼロへ収束させる。
  const bool continueDcRemoval = vgmDcRemovalActive && (sourceAvailable || logicalPlaying);
  if ((sourceAvailable && dcRemoval) || continueDcRemoval) {
    if (!vgmDcRemovalActive) {
      resetVgmDcRemovalState();
      vgmDcRemovalActive = true;
    }
    sample0 = clampOkiSample12(filterVgmDcSample(sample0, okim6258.state.divider));
    sample1 = clampOkiSample12(filterVgmDcSample(sample1, okim6258.state.divider));
  } else if (vgmDcRemovalActive) {
    // 明示停止またはDC除去対象外データへの切替時に状態を破棄する。
    resetVgmDcRemovalState();
  }

  if (okim6258.state.lowPassEnabled) {
    sample0 = clampOkiSample12(filterPcm8Sample(sample0, okim6258.state.divider));
    sample1 = clampOkiSample12(filterPcm8Sample(sample1, okim6258.state.divider));
    sample0 = clampOkiSample12(filterPcm8HighPassSample(sample0, okim6258.state.divider));
    sample1 = clampOkiSample12(filterPcm8HighPassSample(sample1, okim6258.state.divider));
  }

  return okim6258.encodePair((int16_t)sample0, (int16_t)sample1);
}

static void fillVgmOutputQueue() {
  OKIM6258::OKIM6258State& state = okim6258.state;
  portENTER_CRITICAL(&okim6258Mux);
  state.vgmProducerBusy = true;
  portEXIT_CRITICAL(&okim6258Mux);

  while (okim6258.isVgmPcmReencodeActive()) {
    portENTER_CRITICAL(&okim6258Mux);
    const bool blocked =
        state.vgmProducerPaused || state.vgmOutputCount >= OKIM6258_VGM_OUTPUT_QUEUE_SIZE;
    portEXIT_CRITICAL(&okim6258Mux);
    if (blocked) {
      break;
    }

    const u32_t encodeStartUs = micros();
    const u8_t encoded = buildVgmAdpcmByte();
    const u32_t encodeElapsedUs = micros() - encodeStartUs;
    portENTER_CRITICAL(&okim6258Mux);
    if (!state.vgmPcmReencodeActive) {
      portEXIT_CRITICAL(&okim6258Mux);
      break;
    }
    state.vgmOutputQueue[state.vgmOutputTail] = encoded;
    state.vgmOutputTail = (state.vgmOutputTail + 1) % OKIM6258_VGM_OUTPUT_QUEUE_SIZE;
    state.vgmOutputCount++;
    state.vgmEncodedBytes++;
    if (encodeElapsedUs > state.vgmEncodeMaxUs) {
      state.vgmEncodeMaxUs = encodeElapsedUs;
    }
    portEXIT_CRITICAL(&okim6258Mux);
  }

  portENTER_CRITICAL(&okim6258Mux);
  state.vgmProducerBusy = false;
  state.vgmProducerPaused = false;
  portEXIT_CRITICAL(&okim6258Mux);
}

static void fillPcmOutputQueue() {
  OKIM6258::OKIM6258State& state = okim6258.state;
  portENTER_CRITICAL(&okim6258Mux);
  state.pcmProducerBusy = true;
  portEXIT_CRITICAL(&okim6258Mux);

  while (true) {
    portENTER_CRITICAL(&okim6258Mux);
    const bool blocked =
        state.pcmProducerPaused || state.pcmOutputCount >= OKIM6258_PCM_OUTPUT_QUEUE_SIZE;
    portEXIT_CRITICAL(&okim6258Mux);
    if (blocked) {
      break;
    }

    const u32_t encodeStartUs = micros();
    const u8_t data = buildPcm8AdpcmByte();
    const u32_t encodeElapsedUs = micros() - encodeStartUs;

    portENTER_CRITICAL(&okim6258Mux);
    if (state.pcmProducerPaused) {
      portEXIT_CRITICAL(&okim6258Mux);
      break;
    }
    state.pcmOutputQueue[state.pcmOutputTail] = data;
    state.pcmOutputTail = (state.pcmOutputTail + 1) % OKIM6258_PCM_OUTPUT_QUEUE_SIZE;
    state.pcmOutputCount++;
    state.pcmEncodedBytes++;
    if (encodeElapsedUs > state.pcmEncodeMaxUs) {
      state.pcmEncodeMaxUs = encodeElapsedUs;
    }
    portEXIT_CRITICAL(&okim6258Mux);
  }

  portENTER_CRITICAL(&okim6258Mux);
  state.pcmProducerBusy = false;
  portEXIT_CRITICAL(&okim6258Mux);
}

static void taskPCMEncode(void* pvParameters) {
  (void)pvParameters;
  while (1) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    if ((ND::fileFormat == FileFormat::VGM || ND::fileFormat == FileFormat::VGZ) &&
        okim6258.isVgmPcmReencodeActive()) {
      fillVgmOutputQueue();
      continue;
    }

    const bool usePcmEncoder = MDX.pcm8 || okim6258.state.pcm1ReencodeEnabled;
    if (ND::fileFormat != FileFormat::MDX || !MDX.pdxLoaded || !usePcmEncoder) {
      portENTER_CRITICAL(&okim6258Mux);
      okim6258.state.pcmOutputHead = 0;
      okim6258.state.pcmOutputTail = 0;
      okim6258.state.pcmOutputCount = 0;
      portEXIT_CRITICAL(&okim6258Mux);
      continue;
    }

    portENTER_CRITICAL(&okim6258Mux);
    if (okim6258.state.pcmProducerPaused) {
      okim6258.state.pcmOutputHead = 0;
      okim6258.state.pcmOutputTail = 0;
      okim6258.state.pcmOutputCount = 0;
      portEXIT_CRITICAL(&okim6258Mux);
      continue;
    }
    portEXIT_CRITICAL(&okim6258Mux);

    fillPcmOutputQueue();
  }
}

// 割り込みハンドラ
// OKIM6258 送信処理
void IRAM_ATTR okim6258_mck_isr(void* arg) {
  // During file load / ready(), shared buffers are being rebuilt.
  // Bail out early until playback is explicitly enabled.
  // MDXのready()中だけは、曲を進めずに無音ADPCMを送って出力を安定させる。
  const bool allowAdpcmWarmup = ND::fileFormat == FileFormat::MDX && MDX.adpcmWarmupActive;
  if (!ND::canPlay && !allowAdpcmWarmup) {
    return;
  }

  if (ND::fileFormat == FileFormat::VGM || ND::fileFormat == FileFormat::VGZ) {
    if (okim6258.state.vgmPcmReencodeActive) {
      u8_t data = 0x80;
      portENTER_CRITICAL_ISR(&okim6258Mux);
      if (okim6258.state.vgmOutputCount != 0) {
        data = okim6258.state.vgmOutputQueue[okim6258.state.vgmOutputHead];
        okim6258.state.vgmOutputHead =
            (okim6258.state.vgmOutputHead + 1) % OKIM6258_VGM_OUTPUT_QUEUE_SIZE;
        okim6258.state.vgmOutputCount--;
      } else {
        okim6258.state.vgmOutputUnderflows++;
      }
      portEXIT_CRITICAL_ISR(&okim6258Mux);
      FM.setOKIM6258dataISR(data, 1);

      if (hPCMEncode != NULL) {
        BaseType_t hpTaskWoken = pdFALSE;
        vTaskNotifyGiveFromISR(hPCMEncode, &hpTaskWoken);
        if (hpTaskWoken) {
          portYIELD_FROM_ISR();
        }
      }
      return;
    }

    u8_t value = 0;
    bool popped = okim6258.state.cache.pop(value);

    if (popped) {
      FM.setOKIM6258dataISR(value, 1);
      return;
    }

    int blockId = -1;
    u32_t pos = 0;
    u32_t startPos = 0;
    u32_t size = 0;

    portENTER_CRITICAL_ISR(&okim6258Mux);
    blockId = okim6258.state.currentBlockId[0];
    if (blockId != -1 && (size_t)blockId < okim6258.dataBlocks.size()) {
      size = okim6258.dataBlocks[blockId].size;
      startPos = okim6258.dataBlocks[blockId].startPos;
      pos = okim6258.state.posInBlock[0];
      if (pos >= size) {
        okim6258.state.currentBlockId[0] = -1;
        portEXIT_CRITICAL_ISR(&okim6258Mux);
        return;
      }
      okim6258.state.posInBlock[0] = pos + 1;
    } else {
      blockId = -1;
    }
    portEXIT_CRITICAL_ISR(&okim6258Mux);

    if (blockId != -1) {
      u8_t data = ndFile.data[startPos + pos];
      FM.setOKIM6258dataISR(data, 1);
    } else {
      FM.setOKIM6258dataISR(0x80, 1);  // 送るものがないとき
    }
  } else if (ND::fileFormat == FileFormat::MDX) {
    // MDX
    if (!MDX.pdxLoaded) return;

    if (!MDX.pcm8 && !okim6258.state.pcm1ReencodeEnabled) {  // PCM 1ch の従来ADPCM直接送信
      portENTER_CRITICAL_ISR(&okim6258Mux);
      int blockId;
      u32_t currentPos = 0;
      u32_t startPos = 0;
      u32_t length = 0;
      bool hold = false;

      blockId = okim6258.state.currentBlockId[0];
      hold = okim6258.state.hold[0];
      if (blockId != -1) {
        currentPos = okim6258.state.posInBlock[0];
        startPos = okim6258.state.blockStartPos[0];
        length = okim6258.state.blockSizeBytes[0];

        // 範囲チェック
        if (currentPos >= length || (startPos + currentPos) >= ndFile.pdxSize) {
          if (hold) {
            okim6258.state.posInBlock[0] = length;
            blockId = -1;
          } else {
            okim6258.state.currentBlockId[0] = -1;
            okim6258.state.posInBlock[0] = 0;
            okim6258.state.blockStartPos[0] = 0;
            okim6258.state.blockSizeBytes[0] = 0;
            okim6258.state.hold[0] = false;
            blockId = -1;
          }
        } else {
          okim6258.state.posInBlock[0] = currentPos + 1;
        }
      }
      portEXIT_CRITICAL_ISR(&okim6258Mux);

      u8_t data = 0x80;
      if (blockId != -1) {
        data = ndFile.data[PDX_OFFSET + startPos + currentPos];
      }
      FM.setOKIM6258dataISR(data, 1);

    } else {
      // PCM8 / PCM 1ch再圧縮: 事前計算済みキューから1byteを送るだけ。
      // 生成中に次の割り込みが来ても、未送信データは上書きしない。
      u8_t data = 0x80;
      portENTER_CRITICAL_ISR(&okim6258Mux);
      if (okim6258.state.pcmOutputCount != 0) {
        data = okim6258.state.pcmOutputQueue[okim6258.state.pcmOutputHead];
        okim6258.state.pcmOutputHead =
            (okim6258.state.pcmOutputHead + 1) % OKIM6258_PCM_OUTPUT_QUEUE_SIZE;
        okim6258.state.pcmOutputCount--;
      } else {
        okim6258.state.pcmOutputUnderflows++;
      }
      portEXIT_CRITICAL_ISR(&okim6258Mux);
      FM.setOKIM6258dataISR(data, 1);

      // 次の 1byte 生成をタスクに依頼
      if (hPCMEncode != NULL) {
        BaseType_t hpTaskWoken = pdFALSE;
        vTaskNotifyGiveFromISR(hPCMEncode, &hpTaskWoken);
        if (hpTaskWoken) {
          portYIELD_FROM_ISR();
        }
      }
    }
  }
}

// 初期化
void OKIM6258::init() {
  // PCM8 エンコードタスク
  hPCMEncode = NULL;
  BaseType_t taskOk =
      xTaskCreatePinnedToCore(taskPCMEncode, "pcmEncode", 4096, NULL, 2, &hPCMEncode, APP_CPU_NUM);
  if (taskOk != pdPASS || hPCMEncode == NULL) {
    Serial.println("ERROR: pcmEncode task create failed.");
  }

  // 割り込み初期化
  pinMode(MCK, INPUT);

  // 立ち上がりエッジで割り込み登録
  attachInterruptArg(MCK, okim6258_mck_isr, NULL, RISING | ESP_INTR_FLAG_NMI | ESP_INTR_FLAG_IRAM);

  // Serial.println("OKIM6258 MCK interrupt registered");
  // Serial.printf("GPIO Pin: %d\n", MCK);
}

void OKIM6258::latchAdpcmConfig() {
  const int mode = readAdpcmConfigMode();

  portENTER_CRITICAL(&okim6258Mux);
  state.adpcmMode = (u8_t)mode;
  state.pcm1ReencodeEnabled = mode != ADPCM_THROUGH;
  state.vgmReencodeEnabled = mode != ADPCM_THROUGH;
  state.lowPassEnabled = mode == ADPCM_LPF;
  portEXIT_CRITICAL(&okim6258Mux);

  applyAdpcmFilterNjuGain(mode == ADPCM_LPF);
}

void OKIM6258::applyAdpcmConfigRealtime() {
  const int newMode = readAdpcmConfigMode();
  const int oldMode = state.adpcmMode;
  if (newMode == oldMode) {
    return;
  }

  const bool oldReencode = oldMode != ADPCM_THROUGH;
  const bool newReencode = newMode != ADPCM_THROUGH;
  const bool newLowPass = newMode == ADPCM_LPF;
  const bool isVgm = ND::fileFormat == FileFormat::VGM || ND::fileFormat == FileFormat::VGZ;
  const bool isMdx = ND::fileFormat == FileFormat::MDX;

  // ファイル準備中・OKI非搭載VGM・PDXなしでは、次の再生用フラグだけを更新する。
  const bool needsLiveTransport =
      ND::canPlay && ((isVgm && state.vgmChipPresent) || (isMdx && MDX.pdxLoaded));
  if (!needsLiveTransport) {
    portENTER_CRITICAL(&okim6258Mux);
    state.adpcmMode = (u8_t)newMode;
    state.pcm1ReencodeEnabled = newReencode;
    state.vgmReencodeEnabled = newReencode;
    state.lowPassEnabled = newLowPass;
    portEXIT_CRITICAL(&okim6258Mux);
    applyAdpcmFilterNjuGain(newLowPass);
    return;
  }

  // Resample <-> LPF、または常に再圧縮するPCM8は送信経路を変えずに切り替えられる。
  const bool topologyUnchanged = oldReencode == newReencode || (isMdx && MDX.pcm8);
  if (topologyUnchanged) {
    if (isMdx) {
      portENTER_CRITICAL(&okim6258Mux);
      state.pcmProducerPaused = true;
      portEXIT_CRITICAL(&okim6258Mux);
      while (state.pcmProducerBusy) {
        taskYIELD();
      }
    } else {
      portENTER_CRITICAL(&okim6258Mux);
      state.vgmProducerPaused = true;
      portEXIT_CRITICAL(&okim6258Mux);
      while (state.vgmProducerBusy) {
        taskYIELD();
      }
    }

    portENTER_CRITICAL(&okim6258Mux);
    state.adpcmMode = (u8_t)newMode;
    state.pcm1ReencodeEnabled = newReencode;
    state.vgmReencodeEnabled = newReencode;
    state.lowPassEnabled = newLowPass;
    if (isVgm) {
      state.vgmProducerPaused = false;
    }
    portEXIT_CRITICAL(&okim6258Mux);
    resetAdpcmFilterState();
    applyAdpcmFilterNjuGain(newLowPass);

    if (isMdx) {
      portENTER_CRITICAL(&okim6258Mux);
      state.pcmProducerPaused = false;
      portEXIT_CRITICAL(&okim6258Mux);
    } else {
      requestVgmPcmEncode();
    }
    return;
  }

  // RAW <-> 再圧縮では出力の連続性が切れるため、ADPCM左右だけを一時ミュートする。
  // 実チップPLAY状態とMCKは変更せず、ISRの送信元だけを切り替える。
  nju72342.panMute();
  applyAdpcmFilterNjuGain(newLowPass);

  if (isVgm) {
    if (state.vgmPcmReencodeActive) {
      endVgmPcmReencode(true);
    }

    bool wasLogicalPlaying;
    bool wasPhysicalPlaying;
    int blockId;
    u32_t blockPos;
    portENTER_CRITICAL(&okim6258Mux);
    wasLogicalPlaying = state.isPlaying;
    wasPhysicalPlaying = state.vgmPhysicalPlaying;
    blockId = state.currentBlockId[0];
    blockPos = state.posInBlock[0];
    portEXIT_CRITICAL(&okim6258Mux);
    // 未送信データやブロック指定はPLAY前にも存在するため、再生判定には使わない。
    // RAW時の論理状態と実チップ状態のどちらかがPLAYなら、再圧縮側も再生中として引き継ぐ。
    const bool wasEffectivelyPlaying = wasLogicalPlaying || wasPhysicalPlaying;

    portENTER_CRITICAL(&okim6258Mux);
    state.adpcmMode = (u8_t)newMode;
    state.pcm1ReencodeEnabled = newReencode;
    state.vgmReencodeEnabled = newReencode;
    state.lowPassEnabled = newLowPass;
    state.currentBlockId[0] = blockId;
    state.posInBlock[0] = blockPos;
    state.isPlaying = wasEffectivelyPlaying;
    portEXIT_CRITICAL(&okim6258Mux);
    resetAdpcmFilterState();

    if (newReencode) {
      beginVgmPcmReencode(true);
      // Restore the source cursor after the reencode queue has been initialized.
      // Long VGM ADPCM blocks must continue from the route-switch position.
      portENTER_CRITICAL(&okim6258Mux);
      state.currentBlockId[0] = blockId;
      state.posInBlock[0] = blockPos;
      portEXIT_CRITICAL(&okim6258Mux);
      setVgmPcmPlaying(wasEffectivelyPlaying, wasEffectivelyPlaying);
    }
    // Keep the discontinuity hidden while the new ADPCM route settles.
    ets_delay_us(VgmRouteSwitchMuteUs);
    nju72342.panUnmute();
  } else {
    portENTER_CRITICAL(&okim6258Mux);
    state.pcmProducerPaused = true;
    portEXIT_CRITICAL(&okim6258Mux);
    while (state.pcmProducerBusy) {
      taskYIELD();
    }

    portENTER_CRITICAL(&okim6258Mux);
    state.adpcmMode = (u8_t)newMode;
    state.pcm1ReencodeEnabled = newReencode;
    state.vgmReencodeEnabled = newReencode;
    state.lowPassEnabled = newLowPass;
    if (state.currentBlockId[0] != -1) {
      state.posInBlock[0] = 0;
      MDX.tracks[8].pcmRateCounter = 0;
    }
    state.pcmOutputHead = 0;
    state.pcmOutputTail = 0;
    state.pcmOutputCount = 0;
    portEXIT_CRITICAL(&okim6258Mux);
    resetEncodeState();
    portENTER_CRITICAL(&okim6258Mux);
    state.pcmProducerPaused = false;
    portEXIT_CRITICAL(&okim6258Mux);

    // MCKを継続したまま、数byte分だけミュート期間を置いて新経路を安定させる。
    ets_delay_us(520);
    nju72342.panUnmute();
  }
}

bool OKIM6258::beginVgmPcmReencode(bool preserveInputCache) {
  if (!state.vgmReencodeEnabled) {
    return false;
  }

  portENTER_CRITICAL(&okim6258Mux);
  state.vgmPcmReencodeActive = false;
  state.vgmProducerPaused = false;
  state.vgmOutputHead = 0;
  state.vgmOutputTail = 0;
  state.vgmOutputCount = 0;
  state.vgmOutputUnderflows = 0;
  state.vgmInputOverflows = 0;
  state.vgmEncodedBytes = 0;
  state.vgmEncodeMaxUs = 0;
  state.vgmProducerBusy = false;
  state.isPlaying = false;
  portEXIT_CRITICAL(&okim6258Mux);
  if (!preserveInputCache) {
    state.cache.clear();
  }

  vgmDecodeSignal = -2;
  vgmDecodeStepIndex = 0;
  resetEncodeState();

  // canPlay前に、PCMゼロから作ったADPCMを先行充填する。
  for (u8_t i = 0; i < OKIM6258_VGM_OUTPUT_QUEUE_SIZE; ++i) {
    const u8_t encoded = buildVgmAdpcmByte();
    state.vgmOutputQueue[state.vgmOutputTail] = encoded;
    state.vgmOutputTail = (state.vgmOutputTail + 1) % OKIM6258_VGM_OUTPUT_QUEUE_SIZE;
    state.vgmOutputCount++;
  }

  portENTER_CRITICAL(&okim6258Mux);
  state.vgmPcmReencodeActive = true;
  portEXIT_CRITICAL(&okim6258Mux);
  return true;
}

void OKIM6258::endVgmPcmReencode(bool preserveInputCache) {
  portENTER_CRITICAL(&okim6258Mux);
  state.vgmPcmReencodeActive = false;
  state.vgmProducerPaused = true;
  portEXIT_CRITICAL(&okim6258Mux);

  // PSRAMから1byte取得中の変換タスクが終わってから、ファイルバッファを解放可能にする。
  while (state.vgmProducerBusy) {
    vTaskDelay(1);
  }

  if (!preserveInputCache) {
    constexpr const char* filterForm = "TDF2";
    u32_t encodedBytes;
    u32_t encodeMaxUs;
    u32_t outputUnderflows;
    u32_t inputOverflows;
    bool lowPassEnabled;
    portENTER_CRITICAL(&okim6258Mux);
    encodedBytes = state.vgmEncodedBytes;
    encodeMaxUs = state.vgmEncodeMaxUs;
    outputUnderflows = state.vgmOutputUnderflows;
    inputOverflows = state.vgmInputOverflows;
    lowPassEnabled = state.lowPassEnabled;
    portEXIT_CRITICAL(&okim6258Mux);
    Serial.printf(
        "VGM ADPCM filter: %s, LPF=%s, bytes=%u, max=%u us, "
        "underflows=%u, inputOverflows=%u\n",
        filterForm, lowPassEnabled ? "on" : "off", encodedBytes, encodeMaxUs, outputUnderflows,
        inputOverflows);
  }

  portENTER_CRITICAL(&okim6258Mux);
  if (!preserveInputCache) {
    state.isPlaying = false;
    state.currentBlockId[0] = -1;
    state.posInBlock[0] = 0;
  }
  state.vgmOutputHead = 0;
  state.vgmOutputTail = 0;
  state.vgmOutputCount = 0;
  portEXIT_CRITICAL(&okim6258Mux);
  if (!preserveInputCache) {
    state.cache.clear();
  }
}

void OKIM6258::requestVgmPcmEncode() {
  if (hPCMEncode != NULL && isVgmPcmReencodeActive()) {
    xTaskNotifyGive(hPCMEncode);
  }
}

void OKIM6258::setVgmPcmPlaying(bool playing, bool forceDecoderReset, bool preserveDecoderState) {
  if (!playing && preserveDecoderState) {
    // 0x95ブロック切替用。論理PLAY状態と復号・DC履歴を保ったまま、
    // 旧ブロックの変換が終わるまでプロデューサだけを停止する。
    portENTER_CRITICAL(&okim6258Mux);
    state.vgmProducerPaused = true;
    portEXIT_CRITICAL(&okim6258Mux);
    while (state.vgmProducerBusy) {
      taskYIELD();
    }
    return;
  }

  bool wasPlaying;
  portENTER_CRITICAL(&okim6258Mux);
  state.vgmProducerPaused = true;
  wasPlaying = state.isPlaying;
  state.isPlaying = playing;
  portEXIT_CRITICAL(&okim6258Mux);

  // 変換途中の1byteが旧デコーダ状態で確定するのを待つ。
  while (state.vgmProducerBusy) {
    taskYIELD();
  }

  // 真のSTOPからPLAYへ入るときだけ入力側ADPCMデコーダを初期化する。
  // 0x95によるデータブロック切替ではRAW経路と同じく履歴を継続する。
  const bool resetDecoder = playing && !preserveDecoderState && (!wasPlaying || forceDecoderReset);
  if (resetDecoder) {
    vgmDecodeSignal = -2;
    vgmDecodeStepIndex = 0;
  }
  if (!preserveDecoderState && (!playing || resetDecoder)) {
    resetVgmDcRemovalState();
  }

  portENTER_CRITICAL(&okim6258Mux);
  state.vgmProducerPaused = false;
  portEXIT_CRITICAL(&okim6258Mux);
  requestVgmPcmEncode();
}

bool OKIM6258::isVgmPcmReencodeActive() const {
  return state.vgmPcmReencodeActive;
}

// ADPCM デコード
void OKIM6258::decode() {
  if (ndFile.pdxSize == 0) {
    return;
  }

  static const u32_t kEntriesPerBank = 96;
  static const u32_t kEntrySize = 8;
  static const u32_t kBankStride = kEntriesPerBank * kEntrySize;  // 0x300
  static const u32_t kMaxBanks = 32;

  u32_t maxBanks = ndFile.pdxSize / kBankStride;
  if (maxBanks > kMaxBanks) {
    maxBanks = kMaxBanks;
  }

  // PDX は先頭にバンクテーブルが連続し、その後ろに PCM データが並ぶ。
  // いったん見つけた最小データ開始位置以降は、テーブルとして解釈しない。
  u32_t minDataStart = ndFile.pdxSize;

  for (u32_t bank = 0; bank < maxBanks; ++bank) {
    const u32_t bankBase = bank * kBankStride;
    if (bankBase >= minDataStart) {
      break;
    }
    for (u32_t note = 0; note < kEntriesPerBank; ++note) {
      const u32_t entryPos = bankBase + note * kEntrySize;
      if (entryPos + 7 >= ndFile.pdxSize) {
        break;
      }

      const u32_t startPos = ndFile.get_pdx_ui32_be_at(entryPos);
      const u32_t blockSize = ndFile.get_pdx_ui32_be_at(entryPos + 0x04);
      const u32_t minStartForBank = bankBase + kBankStride;

      if (blockSize == 0) {
        continue;
      }
      if (startPos < minStartForBank || startPos >= ndFile.pdxSize) {
        continue;
      }

      u32_t inSize = blockSize;
      if (startPos + inSize > ndFile.pdxSize) {
        inSize = ndFile.pdxSize - startPos;
      }
      if (inSize == 0) {
        continue;
      }
      if (startPos < minDataStart) {
        minDataStart = startPos;
      }

      u64_t outPos64 = (u64_t)PCM_OFFSET + ((u64_t)startPos * 4ULL);
      if (outPos64 >= MAX_FILE_SIZE) {
        continue;
      }
      u64_t outBytes64 = (u64_t)inSize * 4ULL;
      if (outPos64 + outBytes64 > MAX_FILE_SIZE) {
        outBytes64 = MAX_FILE_SIZE - outPos64;
        inSize = (u32_t)(outBytes64 / 4ULL);
        outBytes64 = (u64_t)inSize * 4ULL;
      }
      if (inSize == 0) {
        continue;
      }

      u32_t outPos = (u32_t)outPos64;
      const u32_t outEnd = (u32_t)(outPos64 + outBytes64);
      const u32_t inBase = PDX_OFFSET + startPos;

      int signal = -2;
      int stepIndex = 0;

      for (u32_t i = 0; i < inSize && outPos < outEnd; i++) {
        u8_t byte = ndFile.data[inBase + i];

        for (int shift = 0; shift <= 4; shift += 4) {
          u8_t nib = (byte >> shift) & 0x0f;

          int step = StepTable[stepIndex];
          int diff = step >> 3;
          if (nib & 0x01) diff += step >> 2;
          if (nib & 0x02) diff += step >> 1;
          if (nib & 0x04) diff += step;

          if (nib & 0x08) {
            signal -= diff;
          } else {
            signal += diff;
          }

          if (signal > 2047) signal = 2047;
          if (signal < -2048) signal = -2048;

          stepIndex += IndexShift[nib & 0x07];
          if (stepIndex < 0) stepIndex = 0;
          if (stepIndex > 48) stepIndex = 48;

          int16_t sample = (int16_t)signal;
          if (outPos + 1 >= outEnd) {
            break;
          }
          ndFile.data[outPos++] = (u8_t)(sample & 0xff);
          ndFile.data[outPos++] = (u8_t)((sample >> 8) & 0xff);
        }
      }
    }
  }
}

// OKI ステートリセット
void OKIM6258::resetEncodeState() {
  encodeSignal = 0;  // Furnace oki_codec.c の history 初期値に合わせる
  encodeStepIndex = 0;
  resetAdpcmFilterState();
  resetVgmDcRemovalState();
}

// ADPCM エンコード
// 12-bit signed int - > 4-bit nibble
u8_t OKIM6258::encode(int16_t sample12) {
  return encodeNibbleFast((int)sample12, encodeSignal, encodeStepIndex);
}

u8_t OKIM6258::encodePair(int16_t sample12a, int16_t sample12b) {
  int signal = encodeSignal;
  int stepIndex = encodeStepIndex;
  u8_t nib0 = encodeNibbleFast((int)sample12a, signal, stepIndex);
  u8_t nib1 = encodeNibbleFast((int)sample12b, signal, stepIndex);
  encodeSignal = signal;
  encodeStepIndex = stepIndex;
  return (u8_t)(nib0 | (nib1 << 4));
}

OKIM6258 okim6258;
