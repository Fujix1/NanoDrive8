#include "serialman.h"

#include "NJU72342.h"
#include "SI5351.hpp"
#include "disp.h"
#include "fm.h"
#include "nd.h"
#include "okim6258.h"

// シリアルモード初期化
void SerialMan::init() {
  ND::canPlay = false;
  ND::isPaused = false;
  ND::fileFormat = FileFormat::Unknown;
  ND::freq = {SI5351_4000, SI5351_8000, SI5351_UNDEFINED};
  SI5351.setFreq(ND::freq[0], 0);
  SI5351.setFreq(ND::freq[1], 1);

  // YM2151 の IC と OKI の AC をリセットし、ソフトウェア状態も初期化する。
  FM.reset();
  okim6258.reset();
  FM.setOKIM6258command(0x01, 1);
  nju72342.setVolumeAll(0);

  ND::chipNames = {ND::formatChipName(ND::freq[0], CHIP_YM2151),
                   ND::formatChipName(ND::freq[1], CHIP_OKIM6258)};
  playerWindow.show();
}

SerialMan serialMan;
