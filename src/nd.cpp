#include "nd.h"

// ND ステートの初期化
tMode ND::currentMode = MODE_PLAYER;
FileFormat ND::fileFormat = FileFormat::Unknown;
bool ND::canPlay = false;
bool ND::isPaused = false;
uint8_t ND::att = 0;

std::array<si5351Freq_t, 3> ND::freq = {SI5351_UNDEFINED, SI5351_UNDEFINED, SI5351_UNDEFINED};

byte ND::chipSlot[15];
byte ND::clockSlot[15];  // クロック使用番号

std::vector<String> ND::chipNames;

// チップ名のフォーマット
String ND::formatChipName(si5351Freq_t freq, t_chip chip) {
  char buf[10];

  if (freq != SI5351_UNDEFINED) {
    const unsigned int rounded100Hz = (static_cast<unsigned int>(freq) + 50u) / 100u;
    snprintf(buf, sizeof(buf), "%u.%04u", rounded100Hz / 10000u, rounded100Hz % 10000u);
    buf[5] = '\0';  // 周波数表示は先頭5文字まで。
    return CHIP_LABEL[chip] + " @ " + buf + " MHz";
  }
  return CHIP_LABEL[chip];
}
