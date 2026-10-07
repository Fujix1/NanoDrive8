///---------------------------------------------------------------------------------------------
/// NanoDrive 状態保持

#ifndef ND_H
#define ND_H

#define ND_FIRMWARE_VERSION "1.0b8"

#include <Arduino.h>

#include <vector>

#include "SI5351_types.hpp"
#include "config.h"

// チップ定義
typedef enum {
  CHIP_NONE,
  CHIP_SN76489_0,
  CHIP_SN76489_1,
  CHIP_YM2413,    // OPLL
  CHIP_YM2612,    // OPN2
  CHIP_YM2151,    // OPM
  CHIP_YM2203_0,  // OPN
  CHIP_YM2203_1,  // OPN
  CHIP_YM2608,    // OPNA
  CHIP_YM2610,    // OPNB
  CHIP_YM3526,    // OPL
  CHIP_YM3812,    // OPL2
  CHIP_AY8910,    // PSG
  CHIP_YMF262,    // OPL3
  CHIP_OKIM6258,  // OKI MSM6258
} t_chip;

// チップ名
const std::vector<String> CHIP_LABEL = {"",       "SN76489", "SN76489", "YM2413", "YM2612",
                                        "YM2151", "YM2203",  "YM2203",  "YM2608", "YM2610",
                                        "YM3526", "YM3812",  "AY8910",  "YMF262", "M6258"};

// クロック使用番号
typedef enum { CLK_0, CLK_1, CLK_2, CLK_NONE, CLK_FIXED } t_clockSlot;

// 現在のファイルフォーマット
enum class FileFormat {
  Unknown,
  VGM,
  VGZ,
  MDX,
  XGM1,
  XGM2,
  S98,
};

const std::vector<String> FORMAT_LABEL = {"--", "VGM", "VGZ", "MDX", "XGM1", "XGM2", "S98"};

class ND {
 public:
  static tMode currentMode;      // 現在の動作モード
  static FileFormat fileFormat;  //
  static bool canPlay;           // ファイル処理可能
  static bool isPaused;          // 再生ホールド中

  // クロック出力 3ch
  static std::array<si5351Freq_t, 3> freq;

  // チップスロット
  static byte chipSlot[15];
  static byte clockSlot[15];

  // フォーマット済みチップ名
  static std::vector<String> chipNames;  // チップ名+周波数

  // チップ名のフォーマット
  static String formatChipName(si5351Freq_t freq, t_chip chip);
};

#endif
