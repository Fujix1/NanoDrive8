#ifndef VGM_H
#define VGM_H

#include <math.h>

#include <vector>

#include "SI5351_types.hpp"
#include "common.h"
#include "fm.h"
#include "freertos/semphr.h"
#include "keyinfo.h"
#include "nd.h"
#include "util.h"

// GD3 構造体
typedef struct {
  String trackEn, trackJp, gameEn, gameJp, systemEn, systemJp, authorEn, authorJp, date, converted,
      notes;
} t_gd3;

// 秩父別キー状態セマフォ
extern SemaphoreHandle_t keyInfoMutex;

// -----------------------------------------------------------
// VGM クラス
class VGM {
 public:
  u32_t version;     // VGM バージョン
  u32_t dataOffset;  // データオフセット
  u32_t loopOffset;  // ループオフセット
  u32_t gd3Offset;   // gd3オフセット
  u32_t gd3Size;     // gd3サイズ
  // u32_t totalSamples;  // 全サンプル数
  boolean SN76489_Freq0is0X400;  // SN76489 が Sega VDP ではない

  VGM();
  void init();   // 初期化
  bool ready();  // VGM の再生準備
  void vgmProcess();
  void vgmProcessMain();

  void resetKeyInfo();

  // Share the existing clock normalization with serial mode.
  si5351Freq_t normalizeFreq(u32_t freq, t_chip chip);

  u64_t getCurrentTimeSec();
  u64_t getCurrentTimeSubSec();
  u64_t startTick;

 private:
  t_gd3 gd3;

  u16_t _vgmLoop;
  u64_t _vgmSamples;
  u64_t _vgmRealSamples;
  u64_t _vgmWaitUntil;

  u8_t _ym2203_SSG_reg[2][16] = {0};
  u8_t _ym2203_FM_reg[2][7] = {0};
  u8_t _ym2203_FM_prescaler = 4;   // 仕様と異なる
  u8_t _ym2203_SSG_prescaler = 2;  // 仕様と異なる

  //
  // YM2203 SSG周波数計算
  float _getYM2203SSGFreq(byte chip, byte ch) {
    // トーン/ノイズ有効フラグ
    bool tone_enable = ((_ym2203_SSG_reg[chip][0x07] >> ch) & 0x01) == 0;
    bool noise_enable = ((_ym2203_SSG_reg[chip][0x07] >> (ch + 3)) & 0x01) == 0;

    // 音量レジスタ
    u8_t volreg = _ym2203_SSG_reg[chip][0x08 + ch];
    bool vol_active = ((volreg & 0x0F) != 0) || ((volreg & 0x10) != 0);  // bit4=エンベロープ

    bool keyon = (tone_enable || noise_enable) && vol_active;
    if (!keyon) return 0.0;

    // 周波数計算
    int coarse = _ym2203_SSG_reg[chip][ch * 2 + 1] & 0x0F;
    int fine = _ym2203_SSG_reg[chip][ch * 2 + 0];
    int TP = (coarse << 8) | fine;
    if (TP == 0) return 0.0;

    return (float)ND::freq[0] * _ym2203_SSG_prescaler / (64.0 * TP);
  }

  // YM2203 FM周波数計算
  float _getFMFreq(byte chip, int channel) {
    // F-number
    u16_t f_number_high = (_ym2203_FM_reg[chip][4 + channel] & 0x07) << 8;
    u8_t f_number_low = _ym2203_FM_reg[chip][channel];
    u16_t f_number = f_number_high | f_number_low;

    // Blockの計算
    u8_t block = (_ym2203_FM_reg[chip][4 + channel] & 0x38) >> 3;

    // F-Numberが0の場合は周波数を0と見なす
    if (f_number == 0) {
      return 0.0;
    }

    // 周波数計算の式: f_note = F-Number * φM / (144 * 2^(21-Block))
    return (float)f_number * ND::freq[0] * _ym2203_FM_prescaler / (144.0 * pow(2.0, 21.0 - block));
  }

  // YM2151 周波数取得
  float _getYM2151Freq(int ch) {
    static const int8_t note_map[16] = {1, 2, 3, -1, 4, 5, 6, -1, 7, 8, 9, -1, 10, 11, 0, -1};
    // YM2151周波数
    static const float SEMITONE_TABLE[12] = {
        0.59460f, 0.62996f, 0.66742f, 0.70711f,  // C,  C#, D,  D#
        0.74915f, 0.79370f, 0.84090f, 0.89090f,  // E,  F,  F#, G
        0.94387f, 1.00000f, 1.05946f, 1.12246f   // G#, A,  A#, B
    };

    // KF[7:2] is a 1/64-semitone fraction. Precompute 2^(KF / (64 * 12))
    // so pitch updates add only a lookup and multiplication.
    static constexpr float KEY_FRACTION_TABLE[64] = {
        1.000000000f, 1.000902943f, 1.001806701f, 1.002711275f,
        1.003616666f, 1.004522874f, 1.005429901f, 1.006337747f,
        1.007246412f, 1.008155898f, 1.009066205f, 1.009977334f,
        1.010889286f, 1.011802061f, 1.012715661f, 1.013630085f,
        1.014545335f, 1.015461411f, 1.016378315f, 1.017296046f,
        1.018214607f, 1.019133996f, 1.020054216f, 1.020975266f,
        1.021897149f, 1.022819863f, 1.023743411f, 1.024667793f,
        1.025593009f, 1.026519061f, 1.027445949f, 1.028373674f,
        1.029302237f, 1.030231638f, 1.031161878f, 1.032092958f,
        1.033024879f, 1.033957641f, 1.034891246f, 1.035825694f,
        1.036760985f, 1.037697121f, 1.038634102f, 1.039571929f,
        1.040510603f, 1.041450125f, 1.042390495f, 1.043331714f,
        1.044273782f, 1.045216702f, 1.046160473f, 1.047105096f,
        1.048050572f, 1.048996902f, 1.049944086f, 1.050892125f,
        1.051841021f, 1.052790773f, 1.053741383f, 1.054692851f,
        1.055645178f, 1.056598366f, 1.057552413f, 1.058507323f
    };

    u8_t kc_reg = FM.ym2151_reg[0x28 + ch];
    const u8_t key_fraction = (FM.ym2151_reg[0x30 + ch] >> 2) & 0x3f;

    int oct = (kc_reg >> 4) & 0x07;
    int note = kc_reg & 0x0F;

    int n = note_map[note];
    if (n < 0) return 0.0f;

    // 440Hz基準を、クロック比とオクターブ底上げを込めて事前計算
    // 3.579MHz時のベース(OCT 0のA) ≒ 6.875Hz (440 / 2^6)
    // ここに クロック比 (freq[0] / 3579545.0f) を掛ける
    float base_clock_freq = ND::freq[0] * (440.0f / (3579545.0f * 64.0f));

    // 周波数 = 基準周波数 * オクターブ倍率 * 音階倍率
    // NOTE=14(C) の時はオクターブを +1 する
    int final_oct = oct + ((note == 14) ? 1 : 0);

    // 3オクターブ上にずらす
    float fout = base_clock_freq * (float)(1 << (final_oct + 3)) * SEMITONE_TABLE[n];

    return fout * KEY_FRACTION_TABLE[key_fraction];
  }

  u32_t _gd3p;
  void _parseGD3(u32_t pos);
  void _parseGD3Cache();
  String _digGD3();
  String _digGD3Cache();
  void _resetGD3();

  // when reach the end of the song
  void endProcedure();
};

extern VGM vgm;

#endif
