#include "vgm.h"

#include <cassert>
#include <codecvt>
#include <locale>
#include <string>

#include "SI5351.hpp"
#include "esp_heap_caps.h"
#include "file.h"
#include "fm.h"
#include "okim6258.h"

// 周波数から音階に変換
NoteInfo freqToNote(float freq) {
  if (freq <= 0) return {0, 0};

  // A4 = 440Hz を基準
  float n = 12.0 * log2(freq / 440.0);
  // A=9 に合わせる
  int noteIndexFromC0 = (int)round(n) + 57;

  if (noteIndexFromC0 < 12) {  // 音域外, C1未満
    return {0, 0};
  }

  int octave = noteIndexFromC0 / 12;
  int note = noteIndexFromC0 % 12;

  return {octave, note};
}

//---------------------------------------------------------------------
static std::string wstringToUTF8(const std::wstring& src) {
  std::wstring_convert<std::codecvt_utf8<wchar_t> > converter;
  return converter.to_bytes(src);
}

namespace {
constexpr u8_t kVgmOkimTrackNo = 8;
constexpr u8_t kVgmOkimLevelOn = 8;
constexpr u8_t kVgmCommandBudgetPerLoop = 64;
constexpr u8_t kYm2151CarrierSlots[8] = {0x08, 0x08, 0x08, 0x08, 0x0c, 0x0e, 0x0e, 0x0f};

static void setTrackDisplayLevel(u8_t trackNo, u8_t level) {
  if (trackNo >= 16) {
    return;
  }
  if (level > 15) {
    level = 15;
  }
  if (xSemaphoreTake(KeyBoard.keyinfoMutex, 0) == pdTRUE) {
    KeyBoard.trackLevel[trackNo] = level;
    xSemaphoreGive(KeyBoard.keyinfoMutex);
  }
}

static u8_t getYm2151DisplayLevel(u8_t ch) {
  if (ch >= 8 || !FM.ym2151_iskeyOn[ch]) {
    return 0;
  }

  const u8_t alg = FM.ym2151_reg[0x20 + ch] & 0x07;
  const u8_t carrierMask = kYm2151CarrierSlots[alg];
  const u8_t keyOnMask = FM.ym2151_keyOnSlots[ch];
  u8_t minTl = 127;
  bool hasCarrier = false;

  for (u8_t op = 0; op < 4; op++) {
    const u8_t slotBit = (u8_t)(1u << op);
    if ((carrierMask & slotBit) == 0 || (keyOnMask & slotBit) == 0) {
      continue;
    }
    const u8_t tl = FM.ym2151_reg[0x60 + ch + op * 8] & 0x7f;
    if (!hasCarrier || tl < minTl) {
      minTl = tl;
      hasCarrier = true;
    }
  }

  if (!hasCarrier) {
    return 0;
  }
  return (u8_t)((127 - minTl) >> 3);
}

static void updateYm2151TrackLevel(u8_t ch) {
  setTrackDisplayLevel(ch, getYm2151DisplayLevel(ch));
}
}  // namespace

//---------------------------------------------------------------------
// VGM クラス
VGM::VGM() {
  // チップスロット クロックスロット
  for (int i = 0; i < sizeof ND::chipSlot / sizeof ND::chipSlot[0]; i++) {
    ND::chipSlot[i] = CHIP_NONE;
    ND::clockSlot[i] = CLK_NONE;
  }

  if (CHIP0 != CHIP_NONE) {
    ND::chipSlot[CHIP0] = 0;
  }
  if (CHIP1 != CHIP_NONE) {
    ND::chipSlot[CHIP1] = 1;
  }
  if (CHIP2 != CHIP_NONE) {
    ND::chipSlot[CHIP2] = 2;
  }
  if (CHIP3 != CHIP_NONE) {
    ND::chipSlot[CHIP3] = 3;
  }

  ND::clockSlot[CHIP0] = CHIP0_CLOCK;
  ND::clockSlot[CHIP1] = CHIP1_CLOCK;
  ND::clockSlot[CHIP2] = CHIP2_CLOCK;
  ND::clockSlot[CHIP3] = CHIP3_CLOCK;
}

void VGM::init() {
}

//---------------------------------------------------------------------
// vgm 再生準備
bool VGM::ready() {
  ND::canPlay = false;

  ndFile.pos = 0;

  // レジスタ初期化
  _ym2203_SSG_reg[2][16] = {0};
  _ym2203_FM_reg[2][7] = {0};

  KeyBoard.reset();

  // OKIM6258 初期化
  okim6258.reset();
  okim6258.latchAdpcmConfig();
  // Reserve block metadata upfront while playback is stopped to avoid
  // reallocations during stream parsing.
  okim6258.dataBlocks.reserve(512);
  okim6258.vgmBlockDcRemoval.reserve(512);

  //
  /*
  Serial.printf("Heap - %'d Bytes free, Min free heap %'d\n", ESP.getFreeHeap(),
                ESP.getMinFreeHeap());
  Serial.printf("PSRAM - Total %'d, Free %'d, Min free %'d\n", ESP.getPsramSize(),
                ESP.getFreePsram(), heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
  Serial.printf("Heap largest free block - %'d\n",
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  Serial.printf("PSRAM largest free block - %'d\n",
                heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
                */
  //

  // ND::fileFormat = FileFormat::Unknown;

  ND::freq[0] = SI5351_UNDEFINED;
  ND::freq[1] = SI5351_UNDEFINED;
  ND::freq[2] = SI5351_UNDEFINED;

  ND::chipNames.clear();

  _vgmLoop = 0;
  _vgmSamples = 0;
  _vgmRealSamples = 0;
  _vgmWaitUntil = 0;
  String currentPath = fileTree.getFullPath(ndFile.currentNode);

  // ヘッダキャッシュ版
  if (!ndFile.getHeaderCache(currentPath)) {
    Serial.println("ERROR: Failed to read file header.");
    ND::canPlay = false;
    return false;
  }

  // VGM ident
  if (ndFile.get_ui32_at_header(0) != 0x206d6756) {
    if (ndFile.get_ui16_at_header(0) != 0x1f8b) {
      lcd.printf("ERROR: The file is VGZ archive. Extract it and add a .vgm extension.\n");
    } else {
      lcd.printf("ERROR: File format is not VGM.\n");
      Serial.println("ERROR: VGM以外のファイルです。");
    }
    ND::canPlay = false;
    return false;
  }

  // ND::fileFormat = FileFormat::VGM;

  // version
  version = ndFile.get_ui32_at_header(8);
  // total # samples
  // totalSamples = get_ui32_at(0x18);

  Serial.printf("VGM version: %x\n", version);

  // loop offset
  loopOffset = ndFile.get_ui32_at_header(0x1c);
  // gd3 offset
  gd3Offset = ndFile.get_ui32_at_header(0x14) + 0x14;
  gd3Size = 0;

  // u32_t gd3Size = ndFile.get_ui32_at_header(gd3Offset + 0x8);

  // data offset
  dataOffset = (version >= 0x150) ? ndFile.get_ui32_at_header(0x34) + 0x34 : 0x40;
  ndFile.pos = dataOffset;

  cachePos = dataOffset;  // キャッシュ

  // Setup Clocks
  u32_t sn76489_clock = ndFile.get_ui32_at_header(0x0c);
  if (sn76489_clock) {
    if (CHIP0 == CHIP_SN76489_0) {
      ND::freq[CHIP0_CLOCK] = normalizeFreq(sn76489_clock, CHIP_SN76489_0);
    } else if (CHIP1 == CHIP_SN76489_0) {
      ND::freq[CHIP1_CLOCK] = normalizeFreq(sn76489_clock, CHIP_SN76489_0);
    }

    // デュアルSN76489
    if ((sn76489_clock & (1 << 30))) {
      // Serial.printf("DUAL, version: %x\n", version);

      if (version < 0x170) {
        ND::freq[2] = normalizeFreq(sn76489_clock, CHIP_SN76489_0);
      } else {
        u32_t headerSize = ndFile.get_ui32_at_header(0xbc);
        u32_t chpClockOffset = ndFile.get_ui32_at_header(0xbc + headerSize);
        u8_t entryCount = ndFile.get_ui8_at_header(0xbc + headerSize + chpClockOffset);
        u8_t chipID = ndFile.get_ui8_at_header(0xbc + headerSize + chpClockOffset + 1);
        u32_t clock = ndFile.get_ui32_at_header(0xbc + headerSize + chpClockOffset + 2);
        if (chipID == 0) {
          ND::freq[1] = normalizeFreq(sn76489_clock, CHIP_SN76489_0);
          ND::freq[2] = normalizeFreq(clock, CHIP_SN76489_0);
        } else if (chipID == 1) {
          ND::freq[2] = normalizeFreq(clock, CHIP_SN76489_0);
        }
      }
    }

    // SN76489 フラグ
    SN76489_Freq0is0X400 = false;
    if (version >= 0x151) {
      SN76489_Freq0is0X400 = ndFile.get_ui8_at_header(0x2b) & 0x0001;
    }
  }

  u32_t ym2413_clock = ndFile.get_ui32_at_header(0x10);
  if (ym2413_clock) {
    if (ND::clockSlot[CHIP_YM2413] != CLK_NONE) {
      si5351Freq_t tfreq = normalizeFreq(ym2413_clock, CHIP_YM2413);
      ND::freq[ND::clockSlot[CHIP_YM2413]] = tfreq;
      ND::chipNames.push_back(ND::formatChipName(tfreq, CHIP_YM2413));
    }
  }

  u32_t ym2612_clock = ndFile.get_ui32_at_header(0x2c);
  if (ym2612_clock) {
    if (CHIP0 == CHIP_YM2612) {
      ND::freq[CHIP0_CLOCK] = normalizeFreq(ym2612_clock, CHIP_YM2612);
    } else if (CHIP1 == CHIP_YM2612) {
      ND::freq[CHIP1_CLOCK] = normalizeFreq(ym2612_clock, CHIP_YM2612);
    }
  }

  u32_t ay8910_clock =
      (version >= 0x151 && dataOffset >= 0x78) ? ndFile.get_ui32_at_header(0x74) : 0;
  if (ay8910_clock) {
    if (ay8910_clock & 0x40000000) {  // Dual AY-3-8910
      Serial.printf("Dual AY8910: clock %x", ay8910_clock);
      si5351Freq_t tfreq = normalizeFreq(ay8910_clock, CHIP_AY8910);
      if (CHIP0 == CHIP_YM2203_0) {
        ND::freq[CHIP0_CLOCK] = normalizeFreq(ay8910_clock, CHIP_AY8910);
        ND::chipNames.push_back(ND::formatChipName(tfreq, CHIP_YM2203_0));
      }
      if (CHIP1 == CHIP_YM2203_1) {
        ND::freq[CHIP1_CLOCK] = normalizeFreq(ay8910_clock, CHIP_AY8910);
        ND::chipNames.push_back(ND::formatChipName(tfreq, CHIP_YM2203_1));
      }
    } else {
      if (ND::clockSlot[CHIP_AY8910] != CLK_NONE) {
        si5351Freq_t tfreq = normalizeFreq(ay8910_clock, CHIP_AY8910);
        ND::freq[ND::clockSlot[CHIP_AY8910]] = tfreq;
        ND::chipNames.push_back(ND::formatChipName(tfreq, CHIP_AY8910));
      } else if (ND::clockSlot[CHIP_YM2203_0] != CLK_NONE) {
        si5351Freq_t tfreq = normalizeFreq(ay8910_clock, CHIP_AY8910);
        ND::freq[ND::clockSlot[CHIP_YM2203_0]] = tfreq;
        ND::chipNames.push_back(ND::formatChipName(tfreq, CHIP_YM2203_0));
      }
    }
  }

  u32_t ym2203_clock =
      (version >= 0x151 && dataOffset >= 0x48) ? ndFile.get_ui32_at_header(0x44) : 0;
  if (ym2203_clock) {
    if (ym2203_clock & 0x40000000) {  // Dual YM2203
      si5351Freq_t tfreq = normalizeFreq(ym2203_clock, CHIP_YM2203_0);
      if (CHIP0 == CHIP_YM2203_0) {
        ND::freq[CHIP0_CLOCK] = normalizeFreq(ym2203_clock, CHIP_YM2203_0);
        ND::chipNames.push_back(ND::formatChipName(tfreq, CHIP_YM2203_0));
      }
      if (CHIP1 == CHIP_YM2203_1) {
        ND::freq[CHIP1_CLOCK] = normalizeFreq(ym2203_clock, CHIP_YM2203_1);
        ND::chipNames.push_back(ND::formatChipName(tfreq, CHIP_YM2203_1));
      }
    } else {
      if (ND::clockSlot[CHIP_YM2203_0] != CLK_NONE) {
        si5351Freq_t tfreq = normalizeFreq(ym2203_clock, CHIP_YM2203_0);
        ND::freq[ND::clockSlot[CHIP_YM2203_0]] = tfreq;
        ND::chipNames.push_back(ND::formatChipName(tfreq, CHIP_YM2203_0));
      } else if (ND::clockSlot[CHIP_YM2612] != CLK_NONE) {
        // Use YM2612 as YM2203
        si5351Freq_t tfreq = normalizeFreq(ym2203_clock, CHIP_YM2612);
        ND::freq[ND::clockSlot[CHIP_YM2612]] = tfreq;
        ND::chipNames.push_back(ND::formatChipName(tfreq, CHIP_YM2612));
      }
    }
  }

  u32_t ym2151_clock = ndFile.get_ui32_at_header(0x30);
  if (ym2151_clock) {
    Serial.printf("YM2151 clock: %d Hz\n", ym2151_clock);
    if (ND::clockSlot[CHIP_YM2151] != CLK_NONE) {
      si5351Freq_t tfreq = normalizeFreq(ym2151_clock, CHIP_YM2151);
      ND::freq[ND::clockSlot[CHIP_YM2151]] = tfreq;
      ND::chipNames.push_back(ND::formatChipName(tfreq, CHIP_YM2151));
    }
  }
  u32_t ym3812_clock =
      (version >= 0x151 && dataOffset >= 0x54) ? ndFile.get_ui32_at_header(0x50) : 0;  // OPL2
  if (ym3812_clock) {
    if (ND::clockSlot[CHIP_YM3812] != CLK_NONE) {
      si5351Freq_t tfreq = normalizeFreq(ym3812_clock, CHIP_YM3812);
      ND::freq[ND::clockSlot[CHIP_YM3812]] = tfreq;
      ND::chipNames.push_back(ND::formatChipName(tfreq, CHIP_YM3812));
    } else if (ND::clockSlot[CHIP_YMF262] != CLK_NONE) {  // Use YMF262 as YMF3812
      si5351Freq_t tfreq = normalizeFreq(ym3812_clock, CHIP_YMF262);
      ND::freq[ND::clockSlot[CHIP_YMF262]] = tfreq;
      ND::chipNames.push_back(ND::formatChipName(tfreq, CHIP_YMF262));
    }
  }

  u32_t ym3526_clock =
      (version >= 0x151 && dataOffset >= 0x58) ? ndFile.get_ui32_at_header(0x54) : 0;  // OPL
  if (ym3526_clock) {
    if (ND::clockSlot[CHIP_YM3526] != CLK_NONE) {  //
      si5351Freq_t tfreq = normalizeFreq(ym3526_clock, CHIP_YM3526);
      ND::freq[ND::clockSlot[CHIP_YM3526]] = tfreq;
      ND::chipNames.push_back(ND::formatChipName(tfreq, CHIP_YM3526));
    } else if (ND::clockSlot[CHIP_YM3812] != CLK_NONE) {  // Use YM3812 as YM3526
      si5351Freq_t tfreq = normalizeFreq(ym3526_clock, CHIP_YM3812);
      ND::freq[ND::clockSlot[CHIP_YM3812]] = tfreq;
      ND::chipNames.push_back(ND::formatChipName(tfreq, CHIP_YM3812));
    } else if (ND::clockSlot[CHIP_YMF262] != CLK_NONE) {  // Use YMF262 as YM3526
      si5351Freq_t tfreq = normalizeFreq(ym3526_clock, CHIP_YMF262);
      ND::freq[ND::clockSlot[CHIP_YMF262]] = tfreq;
      ND::chipNames.push_back(ND::formatChipName(tfreq, CHIP_YMF262));
    }
  }

  u32_t y8950_clock =
      (version >= 0x151 && dataOffset >= 0x5C) ? ndFile.get_ui32_at_header(0x58) : 0;  // Y8950
  if (y8950_clock) {
    if (ND::clockSlot[CHIP_YMF262] != CLK_NONE) {  // Use YMF262 as Y8950
      si5351Freq_t tfreq = normalizeFreq(y8950_clock, CHIP_YMF262);
      ND::freq[ND::clockSlot[CHIP_YMF262]] = tfreq;
      ND::chipNames.push_back(ND::formatChipName(tfreq, CHIP_YMF262));
    }
  }

  u32_t ymf262_clock =
      (version >= 0x151 && dataOffset >= 0x60) ? ndFile.get_ui32_at_header(0x5c) : 0;  // OPL3

  if (ymf262_clock) {
    if (ND::clockSlot[CHIP_YMF262] != CLK_NONE) {
      si5351Freq_t tfreq = normalizeFreq(ymf262_clock, CHIP_YMF262);
      ND::freq[ND::clockSlot[CHIP_YMF262]] = tfreq;
      ND::chipNames.push_back(ND::formatChipName(tfreq, CHIP_YMF262));
    }
  }

  u32_t okim6258_clock =
      (version >= 0x151 && dataOffset >= 0x9c) ? ndFile.get_ui32_at_header(0x90) : 0;

  if (okim6258_clock) {
    okim6258.state.vgmChipPresent = true;
    FM.setOKIM6258command(0b00000001, 1);  // ADPCM オフ
    if (ND::clockSlot[CHIP_OKIM6258] != CLK_NONE) {
      si5351Freq_t tfreq = normalizeFreq(okim6258_clock, CHIP_OKIM6258);
      // ND::freq[clockSlot[CHIP_OKIM6258]] = tfreq;
      ND::freq[1] = tfreq;
      ND::chipNames.push_back(ND::formatChipName(tfreq, CHIP_OKIM6258));
      KeyBoard.trackPan[8] = PAN_CENTER;
    }

    // OKIM6258フラグ
    u8_t okim6258_flag = ndFile.get_ui8_at_header(0x94);
    okim6258.state.divider = (okim6258_flag & 0x03) == 0   ? OKIM6258_DIV_1024
                             : (okim6258_flag & 0x03) == 1 ? OKIM6258_DIV_768
                                                           : OKIM6258_DIV_512;  // 分周
    FM.setOKIM6258divider(okim6258.state.divider);
    okim6258.state.adpcmBits = (okim6258_flag & 0x04) ? 3 : 4;  // ビット数
    portENTER_CRITICAL(&okim6258Mux);
    okim6258.state.cache.clear();
    portEXIT_CRITICAL(&okim6258Mux);
  }

  // VGM先頭に並ぶデータブロックは、再生時計を開始する前にDC偏りを検査する。
  // 再生中の0x67処理で復号すると、その遅れを取り戻すため曲が早送りになる。
  if (okim6258_clock && ndFile.accessMode == ACCESS_PSRAM) {
    u32_t dcRemovalBlocks = 0;
    u32_t scanPos = dataOffset;
    while (scanPos <= ndFile.size && ndFile.size - scanPos >= 7 && ndFile.data[scanPos] == 0x67 &&
           ndFile.data[scanPos + 1] == 0x66) {
      const u8_t dataType = ndFile.data[scanPos + 2];
      const u32_t dataSize =
          (u32_t)ndFile.data[scanPos + 3] | ((u32_t)ndFile.data[scanPos + 4] << 8) |
          ((u32_t)ndFile.data[scanPos + 5] << 16) | ((u32_t)ndFile.data[scanPos + 6] << 24);
      const u32_t blockStart = scanPos + 7;
      if (blockStart > ndFile.size || dataSize > ndFile.size - blockStart) {
        break;
      }
      if (dataType == 0x04) {
        const bool dcRemoval = okim6258.detectVgmBlockDcOffset(blockStart, dataSize);
        okim6258.vgmBlockDcRemoval.push_back(dcRemoval ? 1 : 0);
        if (dcRemoval) {
          dcRemovalBlocks++;
        }
      }
      scanPos = blockStart + dataSize;
    }
  }

  // 周波数設定
  if (ND::freq[0] != SI5351_UNDEFINED) {
    SI5351.setFreq(ND::freq[0], 0);
    // Serial.printf("freq0: %d\n", ND::freq[0]);
  }
  if (ND::freq[1] != SI5351_UNDEFINED) {
    SI5351.setFreq(ND::freq[1], 1);
    // Serial.printf("freq1: %d\n", ND::freq[1]);
  }
  /*if (ND::freq[2] != SI5351_UNDEFINED) {
    SI5351.setFreq(ND::freq[2], 0);
  }*/

  SI5351.enableOutputs(true);

  if (okim6258_clock && okim6258.beginVgmPcmReencode()) {
    // openFile()ですでに全体・左右ともミュートされている。VGM時刻開始前にDAOUTを立ち上げる。
    FM.setOKIM6258command(0b00000010, 1);
    okim6258.state.vgmPhysicalPlaying = true;
    ets_delay_us(2000);
    // メイン出力はまだmute中なので、DAOUTの立ち上がりを外へ出さずにADPCM左右を復帰できる。
    nju72342.panUnmute();
  }

  // GD3 tags
  //_parseGD3(gd3Offset);

  // GD3タグ専用キャッシュから取得
  u16_t res = ndFile.getGD3Cache(currentPath, gd3Offset);

  if (res != 0) {
    _parseGD3Cache();
  } else {
    _resetGD3();
  }

  if (ND::chipNames.size() == 0) {
    ND::chipNames.push_back("");
    ND::chipNames.push_back("");
  } else if (ND::chipNames.size() == 1) {
    ND::chipNames.push_back("");
  }

  // 表示更新
  int fileIndex = fileTree.getFileIndexInParent(ndFile.currentNode);
  u32_t n = (fileIndex >= 0) ? (u32_t)(fileIndex + 1) : 0;  // フォルダ内曲番
  Node* currentDirNode = ndFile.currentNode->parent;
  u32_t maxFiles = currentDirNode->fileCount;
  playerWindow.updateDisp({gd3.trackEn, gd3.trackJp, gd3.gameEn, gd3.gameJp, gd3.systemEn,
                           gd3.systemJp, gd3.authorEn, gd3.authorJp, gd3.date, ND::chipNames[0],
                           ND::chipNames[1], FORMAT_LABEL[(int)ND::fileFormat], 0, n, maxFiles});

  startTick = micros64() + 1000;
  ND::canPlay = true;  // VGM 開始できる

  return true;
}

// GD3タグをパース
void VGM::_parseGD3(u32_t pos) {
  _gd3p = pos + 12;

  gd3.trackEn = _digGD3();
  gd3.trackJp = _digGD3();
  gd3.gameEn = _digGD3();
  gd3.gameJp = _digGD3();
  gd3.systemEn = _digGD3();
  gd3.systemJp = _digGD3();
  gd3.authorEn = _digGD3();
  gd3.authorJp = _digGD3();
  gd3.date = _digGD3();
  gd3.converted = _digGD3();
  // gd3.notes = _digGD3();

  if (gd3.trackJp == "") gd3.trackJp = gd3.trackEn;
  if (gd3.gameJp == "") gd3.gameJp = gd3.gameEn;
  if (gd3.systemJp == "") gd3.systemJp = gd3.systemEn;
  if (gd3.authorJp == "") gd3.authorJp = gd3.authorEn;
}

String VGM::_digGD3() {
  std::wstring wst;
  wst.clear();
  while (ndFile.data[_gd3p] != 0 || ndFile.data[_gd3p + 1] != 0) {
    wst += (char16_t)((ndFile.data[_gd3p + 1] << 8) | ndFile.data[_gd3p]);
    _gd3p += 2;
  }
  _gd3p += 2;

  // 全角スペース(U+3000)を半角スペース(U+0020)に置換
  for (auto& c : wst) {
    if (c == 0x3000) c = 0x20;
  }

  std::string sst = wstringToUTF8(wst);
  return (String)sst.c_str();
}

void VGM::_parseGD3Cache() {
  _gd3p = 12;

  gd3.trackEn = _digGD3Cache();
  gd3.trackJp = _digGD3Cache();
  gd3.gameEn = _digGD3Cache();
  gd3.gameJp = _digGD3Cache();
  gd3.systemEn = _digGD3Cache();
  gd3.systemJp = _digGD3Cache();
  gd3.authorEn = _digGD3Cache();
  gd3.authorJp = _digGD3Cache();
  gd3.date = _digGD3Cache();
  gd3.converted = _digGD3Cache();
  // gd3.notes = _digGD3Cache();

  if (gd3.trackJp == "") gd3.trackJp = gd3.trackEn;
  if (gd3.gameJp == "") gd3.gameJp = gd3.gameEn;
  if (gd3.systemJp == "") gd3.systemJp = gd3.systemEn;
  if (gd3.authorJp == "") gd3.authorJp = gd3.authorEn;

  gd3Size = u32_t(ndFile.gd3Cache[0x8]) + (u32_t(ndFile.gd3Cache[0x9]) << 8) +
            (u32_t(ndFile.gd3Cache[0xa]) << 16) + (u32_t(ndFile.gd3Cache[0xb]) << 24) +
            12;  // ヘッダ分 GD3 VER SIZE 12バイト

  ndFile.gd3Cache.clear();
}

String VGM::_digGD3Cache() {
  std::wstring wst;
  wst.clear();
  while (ndFile.gd3Cache[_gd3p] != 0 || ndFile.gd3Cache[_gd3p + 1] != 0) {
    wst += (char16_t)((ndFile.gd3Cache[_gd3p + 1] << 8) | ndFile.gd3Cache[_gd3p]);
    _gd3p += 2;
  }
  _gd3p += 2;
  std::string sst = wstringToUTF8(wst);
  return (String)sst.c_str();
}

void VGM::_resetGD3() {
  gd3.trackEn = ndFile.currentNode->name;
  gd3.trackJp = ndFile.currentNode->name;
  gd3.gameEn = "(No GD3 info)";
  gd3.gameJp = "(GD3情報なし)";
  gd3.systemEn = "";
  gd3.systemJp = "";
  gd3.authorEn = "";
  gd3.authorJp = "";
  gd3.date = "";
}

//----------------------------------------------------------------------
// 周波数の実際の値を設定
si5351Freq_t VGM::normalizeFreq(u32_t freq, t_chip chip) {
  switch (chip) {
    case CHIP_AY8910: {
      switch (freq) {
        case 1250000:
          return SI5351_1250;
          break;
        case 1500000:
          return SI5351_3000;
          break;
        case 1536000:
          return SI5351_3072;
          break;
        case 1789750:
        case 1789772:
        case 1789773:
        case 1789775:
          return SI5351_3579;
          break;
        case 2000000:
          return SI5351_4000;
          break;
        case 0x4016e360:  // dual 1.5MHz
          return SI5351_3000;
          break;
        case 0x400f9b07:  // dual 1.022727MHz Apple II
          return SI5351_2045;
          break;
        case 0x401b4f4c:  // dual 1.789772MHz
          return SI5351_3579;
          break;
        default:
          return SI5351_4000;
          break;
      }
      break;
    }
    case CHIP_YM2413: {
      switch (freq) {
        case 2000000:
          return SI5351_2000;
          break;
        case 3579000 ... 3580000:  // 3.579MHz
          return SI5351_3579;
          break;
        case 4000000:
          return SI5351_4000;
          break;
        default:
          return SI5351_3579;
          break;
      }
      break;
    }
    case CHIP_YM2203_0:
    case CHIP_YM2203_1: {
      switch (freq) {
        case 1250000:  // 1.25MHz
          return SI5351_1250;
          break;
        case 1500000:     // 1.5MHz
        case 1076741824:  // デュアル 1.5MHz
        case 1075241824:  // デュアル 1.5MHz
          return SI5351_1500;
          break;
        case 3000000:  // 3MHz
          return SI5351_3000;
          break;
        case 3072000:  // 3.072MHz
          return SI5351_3072;
          break;
        case 3579000 ... 3580000:  // 3.579MHz
          return SI5351_3579;
          break;
        case 3993600:
          return SI5351_4000;
          break;
        case 4000000:
        case 1077741824:  // デュアル 4MHz
          return SI5351_4000;
          break;
        case 4500000:  // 4.5MHz
          return SI5351_4500;
          break;
        default:
          return SI5351_3579;
          break;
      }
    }
    case CHIP_YM2151: {
      switch (freq) {
        case 3375000:
          return SI5351_3375;
          break;
        case 3500000:
          return SI5351_3500;
          break;
        case 3579000 ... 3580000:  // 3.579MHz
          return SI5351_3579;
          break;
        case 4000000:
          return SI5351_4000;
          break;
        default:
          return SI5351_3579;
          break;
      }
      break;
    }
    case CHIP_YM2608: {
      switch (freq) {
        case 7987000:
          return SI5351_7987;
          break;
        case 8000000:
          return SI5351_8000;
          break;
        default:
          return SI5351_8000;
          break;
      }
      break;
    }
    case CHIP_YM2612: {
      switch (freq) {
        case 8000000:
        case 0x807a1200:
          return SI5351_8000;
          break;
        case 7670453:
          return SI5351_7670;
          break;
        case 1500000:  // YM2203 @ 1.5MHz
          return SI5351_3000;
          break;
        case 3000000:  // YM2203 @ 3MHz
          return SI5351_6000;
          break;
        case 3579000 ... 3580000:  // YM2203 @ 3.579MHz
          return SI5351_7159;
          break;
        case 3993600:
          return SI5351_8000;
          break;
        case 4000000:
        case 1077741824:  // デュアル 4MHz
          return SI5351_8000;
          break;
        default:
          return SI5351_7670;
      }
      break;
    }
    case CHIP_SN76489_0: {
      switch (freq) {
        case 1536000:
          return SI5351_1536;
          break;
        case 1789772:
        case 0x40000000 + 1789772:
          return SI5351_1789;
          break;
        case 3579580:
        case 3579545:
        case 0x40000000 + 3579580:
        case 0x40000000 + 3579545:
          return SI5351_3579;
          break;
        case 4000000:
        case 0x40000000 + 4000000:
          return SI5351_4000;
          break;
        case 2578000:
          return SI5351_2578;
          break;
        case 2000000:
        case 0x40000000 + 2000000:
          return SI5351_2000;
          break;
        default:
          return SI5351_3579;
          break;
      }
      break;
    }

    case CHIP_YM2610: {
      switch (freq) {
        case 8000000:
        case 0x807a1200:
          return SI5351_8000;
          break;
        case 7670453:
          return SI5351_7670;
          break;
        case 1500000:  // YM2203 @ 1.5MHz
          return SI5351_3000;
          break;
        case 3000000:  // YM2203 @ 3MHz
          return SI5351_6000;
          break;
        case 3579580:  // YM2203 @ 3.579MHz
        case 3579545:
          return SI5351_7159;
          break;
        case 3993600:
          return SI5351_8000;
          break;
        case 4000000:
        case 1077741824:  // デュアル 4MHz
          return SI5351_8000;
          break;
        default:
          return SI5351_7670;
      }
      break;
    }
    case CHIP_YM3526:
    case CHIP_YM3812: {
      switch (freq) {
        case 3000000:
          return SI5351_3000;
          break;
        case 3500000:
          return SI5351_3500;
          break;
        case 4000000:
        case 0x40000000 + 4000000:
          return SI5351_4000;
          break;
        case 1789772:
        case 0x40000000 + 1789772:
          return SI5351_1789;
          break;
        case 3579580:
        case 3579545:
        case 0x40000000 + 3579580:
        case 0x40000000 + 3579545:
          return SI5351_3579;
          break;
        case 2578000:
          return SI5351_2578;
          break;
        case 2000000:
        case 0x40000000 + 2000000:
          return SI5351_2000;
          break;
        default:
          return SI5351_3579;
          break;
      }
      break;
    }
    case CHIP_YMF262: {
      switch (freq) {
        case 2000000:
          return SI5351_8000;
          break;
        case 3000000:
          return SI5351_12000;
          break;
        case 3500000:
          return SI5351_14000;
          break;
        case 3579580:
        case 3579545:
          return SI5351_14318;
          break;
        case 4000000:
          return SI5351_16000;
          break;
        case 0xda7a64:
          return SI5351_14318;
          break;
        default:
          return SI5351_14318;
          break;
      }
      break;
    }
    case CHIP_OKIM6258:
      switch (freq) {
        case 3579580:
        case 3579545:
          return SI5351_7670;
          break;
        case 4000000:
          return SI5351_8000;
          break;
        case 4096000:
          return SI5351_4096;
          break;
        case 8000000:
          return SI5351_8000;
          break;
      }
      break;
  }

  return SI5351_UNDEFINED;
}

//----------------------------------------------------------------------
// VGM処理

void VGM::vgmProcess() {
  // フェードアウト完了
  if (nju72342.fadeOutStatus == FADEOUT_COMPLETED) {
    endProcedure();
    return;
  }

  // 待ち時間中は戻って入力処理
  if (_vgmWaitUntil > micros64()) {
    taskYIELD();
    return;
  }

  u8_t processedCommands = 0;
  while (_vgmSamples <= _vgmRealSamples) {
    vgmProcessMain();
    if (++processedCommands >= kVgmCommandBudgetPerLoop && _vgmSamples <= _vgmRealSamples) {
      taskYIELD();
      return;
    }
  }

  _vgmRealSamples = _vgmSamples;
  _vgmWaitUntil = startTick + (_vgmRealSamples * 1000000) / 44100;

  /*while (_vgmWaitUntil > micros64()) {
  ets_delay_us(1);
  }*/
}

void VGM::vgmProcessMain() {
  u8_t reg;
  u8_t dat;
  u8_t command = ndFile.get_ui8();
  u8_t psg_chip;  // デュアル PSG 用

  switch (command) {
#ifdef USE_AY8910
    case 0x31:  // AY8910 Stereo mask (not supported)
      ndFile.get_ui8();
      break;
    case 0xA0:  // AY8910, YM2203 PSG, YM2149, YMZ294D
      reg = ndFile.get_ui8();
      dat = ndFile.get_ui8();

      // dual chip support
      psg_chip = 0;
      if (reg & 0x80) {
        psg_chip = 1;
        reg = reg & 0x7f;
      }

      FM.setRegister(reg, dat, psg_chip);

      _ym2203_SSG_reg[psg_chip][reg] = dat;

      switch (reg) {
        case 0x00 ... 0x05: {
          int ch = reg / 2;
          if (_ym2203_SSG_reg[psg_chip][ch + 0x08] != 0) {  // 音が出てるときだけキー情報を登録
            float psgFreq = _getYM2203SSGFreq(0, ch);
            NoteInfo ni = freqToNote(psgFreq);
            /*
                        if (psg_chip == 0) {
                          KeyBoard.set(YM2203_SSG0, ch, ni);
                        } else {
                          KeyBoard.set(YM2203_SSG1, ch, ni);
                        }
            */
          }
          break;
        }
        case 0x08 ... 0x0a: {
          int ch = reg - 0x08;
          if ((dat & 0x0F) != 0) {  // 音が出てるとき
            float psgFreq = _getYM2203SSGFreq(psg_chip, ch);
            NoteInfo ni = freqToNote(psgFreq);
            /*
                        if (psg_chip == 0) {
                          KeyBoard.set(YM2203_SSG0, ch, ni);
                        } else {
                          KeyBoard.set(YM2203_SSG1, ch, ni);
                        }
                           */
          }
          break;
        }
      }

      break;
#endif

#ifdef USE_SN76489
    case 0x30:  // SN76489 CHIP 2
      if (SN76489_Freq0is0X400) {
        FM.writeRaw(ndFile.get_ui8(), 2, ND::freq[ND::chipSlot[CHIP_SN76489_1]]);
      } else {
        FM.write(ndFile.get_ui8(), 2, ND::freq[ND::chipSlot[CHIP_SN76489_0]]);
      }
      break;

    case 0x50:  // SN76489 CHIP 1
      // WORKAROUND FOR COMMAND TO UNDEFINED SN CHIP
      // Sonic & Knuckles 30th song
      if (ND::freq[ND::chipSlot[CHIP_SN76489_0]] != SI5351_UNDEFINED) {
        if (SN76489_Freq0is0X400) {
          FM.writeRaw(ndFile.get_ui8(), 1, ND::freq[ND::chipSlot[CHIP_SN76489_0]]);
        } else {
          FM.write(ndFile.get_ui8(), 1, ND::freq[ND::chipSlot[CHIP_SN76489_0]]);
        }
      }
      break;
#endif

#ifdef USE_YM2413
    case 0x51:
      reg = ndFile.get_ui8();
      dat = ndFile.get_ui8();
      FM.setRegister(reg, dat, 3);

      if (reg <= 0x38) {
        _ym2413_reg[reg] = dat;  // レジスタ保存
      }

      // キーオンオフのとき
      if ((reg >= 0x20 && reg <= 0x28) || (reg >= 0x10 && reg <= 0x18)) {
        int channel = (reg >= 0x20) ? reg - 0x20 : reg - 0x10;  // チャンネル
        float ym2413Freq = _getYM2413Freq(channel);
        NoteInfo ni = freqToNote(ym2413Freq);
        KeyBoard.set(YM2413, channel, ni);
      }

      break;
#endif

#ifdef USE_YM2612
    case 0x52:  // YM2612 port 0, write value dd to register aa
      reg = ndFile.get_ui8();
      dat = ndFile.get_ui8();
      if ((reg >= 0x30 && reg <= 0xB6) || reg == 0x22 || reg == 0x27 || reg == 0x28 ||
          reg == 0x2A || reg == 0x2B || reg == 0x2C) {  // 未ドキュメント命令
        FM.setYM2612(0, reg, dat, 0);
      }
      break;

    case 0x53:  // YM2612 port 1, write value dd to register aa
      reg = ndFile.get_ui8();
      dat = ndFile.get_ui8();
      if (reg >= 0x30 && reg <= 0xB6) {
        FM.setYM2612(1, reg, dat, 0);
      }
      break;
#endif

    case 0x54:  // YM2151
    case 0xa4:

      reg = ndFile.get_ui8();
      dat = ndFile.get_ui8();
#ifdef USE_YM2151
      if (reg != 0x10 && reg != 0x11 && reg != 0x12) {  // タイマー設定は無効

        FM.setRegisterOPM(reg, dat, 0);
        if (reg == 0x08) {  // キーオンオフ
          int ch = dat & 0x07;
          const u8_t slotMask = (u8_t)((dat >> 3) & 0x0f);
          FM.ym2151_keyOnSlots[ch] = slotMask;
          FM.ym2151_iskeyOn[ch] = (slotMask != 0);
          if (!FM.ym2151_iskeyOn[ch]) {  // bit 6 ... bit 3 ->  OP4 ... OP1
            // キーオフ処理
            KeyBoard.set(YM2151, ch, {0, 0});
          } else {
            // キーオン処理
            KeyBoard.set(YM2151, ch, freqToNote(_getYM2151Freq(ch)));
          }
          updateYm2151TrackLevel((u8_t)ch);
        } else if ((reg >= 0x28 && reg <= 0x2F) || (reg >= 0x30 && reg <= 0x37)) {
          int ch = reg & 0x07;
          if (FM.ym2151_iskeyOn[ch]) {
            KeyBoard.set(YM2151, ch, freqToNote(_getYM2151Freq(ch)));
          }
        } else if (reg >= 0x20 && reg <= 0x27) {
          int ch = reg - 0x20;
          switch (dat & 0xC0) {
            case 0x40:
              KeyBoard.trackPan[ch] = PAN_RIGHT;
              break;
            case 0x80:
              KeyBoard.trackPan[ch] = PAN_LEFT;
              break;
            case 0xC0:
              KeyBoard.trackPan[ch] = PAN_CENTER;
              break;
            default:
              KeyBoard.trackPan[ch] = PAN_MUTE;
              break;
          }
        }
      }
#endif
      break;

    case 0x55:  // YM2203_0
      reg = ndFile.get_ui8();
      dat = ndFile.get_ui8();
#ifdef USE_YM2203_0
      FM.setRegister(reg, dat, 0);

      switch (reg) {
        case 0x00 ... 0x05: {  // SSG音程
          _ym2203_SSG_reg[0][reg] = dat;
          int ch = reg / 2;

          if (_ym2203_SSG_reg[0][0x08 + ch] != 0) {  // 音が出てるとき
            float psgFreq = _getYM2203SSGFreq(0, ch);
            NoteInfo ni = freqToNote(psgFreq);

            // KeyBoard.set(YM2203_SSG0, ch, ni);
          }
          break;
        }
        case 0x08 ... 0x0a: {  // SSG音量
          _ym2203_SSG_reg[0][reg] = dat;
          int ch = reg - 0x08;
          if ((dat & 0x0F) != 0 || ((dat & 0x10) != 0)) {  // 音量セットまたはエンベロープ
            float psgFreq = _getYM2203SSGFreq(0, ch);
            NoteInfo ni = freqToNote(psgFreq);

            // KeyBoard.set(YM2203_SSG0, ch, ni);
          }
          break;
        }

        case 0xa0 ... 0xa6: {                   // FM の音程
          _ym2203_FM_reg[0][reg - 0xa0] = dat;  // レジスタ保持
          break;
        }
        case 0x28: {                // キーオンオフ
          if ((dat & 0xF0) != 0) {  // 上位 4 bitいずれかあればキーオンとする
            int channel = dat & 0x03;
            float ym2203FMFreq = _getFMFreq(0, channel);
            NoteInfo ni = freqToNote(ym2203FMFreq);
            // KeyBoard.set(YM2203_FM0, channel, ni);
          } else if ((dat & 0xF0) == 0) {  // キーオフ
            int channel = dat & 0x03;
            // KeyBoard.set(YM2203_FM0, channel, {0, 0});
          }
          break;
        }
      }

#endif
      break;
    case 0xA5:  // YM2203_1
      reg = ndFile.get_ui8();
      dat = ndFile.get_ui8();
#ifdef USE_YM2203_1
      FM.setRegister(reg, dat, 1);

      switch (reg) {
        case 0x00 ... 0x05: {  // SSG音程
          _ym2203_SSG_reg[1][reg] = dat;
          int ch = (reg - 1) / 2;

          if (_ym2203_SSG_reg[1][0x08 + ch] != 0) {  // 音が出てるとき
            float psgFreq = _getYM2203SSGFreq(1, ch);
            NoteInfo ni = freqToNote(psgFreq);
            // KeyBoard.set(YM2203_SSG1, ch, ni);
          }
          break;
        }
        case 0x08 ... 0x0a: {  // SSG音量
          _ym2203_SSG_reg[1][reg] = dat;
          int ch = reg - 0x08;
          if ((dat & 0x0F) != 0 || ((dat & 0x10) != 0)) {  // 音量セットまたはエンベロープ
            float psgFreq = _getYM2203SSGFreq(1, ch);
            NoteInfo ni = freqToNote(psgFreq);
            // KeyBoard.set(YM2203_SSG1, ch, ni);
          }
          break;
        }
        case 0xa0 ... 0xa6: {                   // FM の音程
          _ym2203_FM_reg[1][reg - 0xa0] = dat;  // レジスタ保持
          break;
        }

        case 0x28: {                // キーオンオフ
          if ((dat & 0xF0) != 0) {  // 上位 4 bitいずれかあればキーオンとする
            int channel = dat & 0x03;
            float ym2203FMFreq = _getFMFreq(1, channel);
            NoteInfo ni = freqToNote(ym2203FMFreq);

            // KeyBoard.set(YM2203_FM1, channel, ni);

            // Serial.printf("FM1 ch %d: KEY ON %s%d\n", channel, String(NOTE_NAMES[ni.note]),
            // ni.octave);
            /*Serial.printf("YM2203 FM1 - ch0: O%d-%d, ch1: O%d-%d, ch2: O%d-%d\n",
               keyInfo[YM2203_FM1][0].octave, keyInfo[YM2203_FM1][0].note,
               keyInfo[YM2203_FM1][1].octave, keyInfo[YM2203_FM1][1].note,
                          keyInfo[YM2203_FM1][2].octave, keyInfo[YM2203_FM1][2].note);
*/
          } else if ((dat & 0xF0) == 0) {  // キーオフ
            int channel = dat & 0x03;

            // KeyBoard.set(YM2203_FM1, channel, {0, 0});

          // float ym2203FMFreq = _getFMFreq(1, channel);
          // NoteInfo ni = freqToNote(ym2203FMFreq);
          // Serial.printf("FM1 ch %d: KEY OFF %s%d\n", channel, String(NOTE_NAMES[ni.note]),
          // ni.octave);
          /*Serial.printf("YM2203 FM1 - ch0: O%d-%d, ch1: O%d-%d, ch2: O%d-%d\n",
             keyInfo[YM2203_FM1][0].octave, keyInfo[YM2203_FM1][0].note,
             keyInfo[YM2203_FM1][1].octave, keyInfo[YM2203_FM1][1].note,
                        keyInfo[YM2203_FM1][2].octave, keyInfo[YM2203_FM1][2].note);
        */ }
          break;
        }
      }
#endif
      break;

#ifdef USE_YM3526
    case 0x5B:  // YM3526
      reg = ndFile.get_ui8();
      dat = ndFile.get_ui8();
      FM.setRegister(reg, dat, 1);
      break;
#endif

#ifdef USE_YM3812
    case 0x5A:  // YM3812
      reg = ndFile.get_ui8();
      dat = ndFile.get_ui8();
      FM.setRegister(reg, dat, 1);
      break;
#endif

#ifdef USE_YMF262
    case 0x5C:  // Y8950 FM
    case 0x5B:  // YM3526
    case 0x5A:  // YM3812
    case 0x5E:  // YMF262 Reg Array 0
      reg = ndFile.get_ui8();
      dat = ndFile.get_ui8();
      FM.setRegisterOPL3(0, reg, dat, 2);
      _ymf262_reg[0][reg] = dat;  // レジスタ保存

      if ((reg >= 0xA0 && reg <= 0xA8) || (reg >= 0xB0 && reg <= 0xB8)) {
        int channel = reg & 0x0F;  // 0〜8
        float freq = _getYMF262Freq(0, channel);
        NoteInfo ni = freqToNote(freq);
        KeyBoard.set(YMF262, channel, ni);
      }
      break;
    case 0x5F:  // YMF262 Reg Array 1
      reg = ndFile.get_ui8();
      dat = ndFile.get_ui8();
      FM.setRegisterOPL3(1, reg, dat, 2);
      _ymf262_reg[1][reg] = dat;  // レジスタ保存

      if ((reg >= 0xA0 && reg <= 0xA8) || (reg >= 0xB0 && reg <= 0xB8)) {
        int channel = (reg & 0x0F) + 9;  // 9〜17
        float freq = _getYMF262Freq(1, reg & 0x0F);
        NoteInfo ni = freqToNote(freq);
        KeyBoard.set(YMF262, channel, ni);
      }
      break;
#endif

    // Wait n samples, n can range from 0 to 65535 (approx 1.49 seconds)
    case 0x61: {
      u16_t w = ndFile.get_ui16();
      _vgmSamples += w;
      break;
    }

    // wait 735 samples (60th of a second)
    case 0x62:
      _vgmSamples += 735;
      break;

    // wait 882 samples (50th of a second)
    case 0x63:
      _vgmSamples += 882;
      break;

    case 0x66:
      if (!loopOffset || ndConfig.get(CFG_FADEOUT) == FO_0) {  // ループしない曲
        endProcedure();
        return;
      } else {
        _vgmLoop++;
        if (_vgmLoop == ndConfig.get(CFG_NUM_LOOP) &&
            ndConfig.get(CFG_NUM_LOOP) != LOOP_INIFITE) {  //   フェードアウトON
          nju72342.startFadeout();
        }

        ndFile.pos = loopOffset + 0x1C;  // ループする曲
      }
      break;

    case 0x67: {
      // データブロック定義
      ndFile.get_ui8();                    // 0x66
      u8_t dataType = ndFile.get_ui8();    // data type
      u32_t dataSize = ndFile.get_ui32();  // size of data, in bytes

      // データブロック追加
      if (dataType == 0x04) {
        const u32_t blockStart = ndFile.pos;
        const bool inRange =
            (blockStart <= ndFile.size) && (dataSize <= (ndFile.size - blockStart));
        if (inRange) {
          portENTER_CRITICAL(&okim6258Mux);
          if (okim6258.dataBlocks.size() < okim6258.dataBlocks.capacity()) {
            okim6258.dataBlocks.push_back({blockStart, dataSize});
          } else {
            Serial.println("WARN: OKIM6258 data block table full. Block skipped.");
          }
          portEXIT_CRITICAL(&okim6258Mux);
        } else {
          Serial.println("WARN: Invalid OKIM6258 data block range. Block skipped.");
        }
      }
      ndFile.pos += dataSize;  // skip data block
      break;
    }

    case 0x70 ... 0x7f:
      _vgmSamples += (command & 15) + 1;
      break;

    case 0x80 ... 0x8f:
      // FM.setYM2612DAC(ndFile.data[_pcmpos++], 0);
      //_vgmSamples += (command & 15);
      break;

    case 0xd0:           // ignore YM278B
      ndFile.get_ui8();  // port
      ndFile.get_ui8();  // value
      ndFile.get_ui8();  // reg
      break;

    case 0x90:
      // Setup Stream Control
      ndFile.get_ui32();
      break;

    case 0x91: {
      // Set Stream Data
      u8_t streamId = ndFile.get_ui8();    // ss: Stream ID
      u8_t dataBankId = ndFile.get_ui8();  // dd: Data Bank ID (data block index)
      u8_t stepSize = ndFile.get_ui8();    // ll: Step Size (skip count, usually 1)
      u8_t stepBase = ndFile.get_ui8();    // bb: Step Base (offset added to start, usually 0)

      // Serial.printf("0x91: Set Stream Data\n");
      // Serial.printf("  Stream ID: %u\n", streamId);
      // Serial.printf("  Data Bank ID: %u\n", dataBankId);
      // Serial.printf("  Step Size: %u\n", stepSize);
      // Serial.printf("  Step Base: %u\n", stepBase);
      break;
    }
    case 0x92: {
      // Set Stream Frequency
      u8_t streamId = ndFile.get_ui8();
      u32_t streamFreq = ndFile.get_ui32();
      // Serial.printf("0x92: Stream id %u, freq %u Hz\n", streamId, streamFreq);
      break;
    }

    case 0x93: {
      // Start Stream
      u8_t startId = ndFile.get_ui8();
      u32_t pcmPos = ndFile.get_ui32();
      ndFile.get_ui8();
      u32_t pcmLength = ndFile.get_ui32();
      // Serial.printf("0x93: Start id %u, @ 0x%x, 0x%x \n", startId, pcmPos, pcmLength);

      break;
    }

    case 0x94: {
      // nju72342.panMute();
      //   Stop Stream
      const bool reencode = okim6258.isVgmPcmReencodeActive();
      if (!reencode) {
        for (int i = 0; i < 4; ++i) {
          okim6258.state.cache.push(0x80);
        }
      }

      // 再生処理中
      if (reencode) {
        okim6258.setVgmPcmPlaying(false);
        okim6258.state.cache.clear();
      } else if (okim6258.state.isPlaying || okim6258.state.vgmPhysicalPlaying) {
        okim6258.state.isPlaying = false;
        nju72342.panMute();
        FM.setOKIM6258command(0b00000001, 1);
        okim6258.state.vgmPhysicalPlaying = false;
        ets_delay_us(260);
      }
      setTrackDisplayLevel(kVgmOkimTrackNo, 0);
      portENTER_CRITICAL(&okim6258Mux);
      okim6258.state.currentBlockId[0] = -1;
      portEXIT_CRITICAL(&okim6258Mux);
      ndFile.pos++;  // u8_t stopId = ndFile.get_ui8();
      // Serial.printf("0x94: Stop\n");
      break;
    }

    case 0x95: {
      //  Start Stream Fast
      // nju72342.panMute();
      ndFile.pos++;  // u8_t streamFId = ndFile.get_ui8();
      u16_t blockId = ndFile.get_ui16();
      ndFile.pos++;  // streamFlag
      const bool reencode = okim6258.isVgmPcmReencodeActive();
      bool preserveDecoderState = false;
      if (reencode) {
        // 旧ブロックの先行復号だけを止める。実チップをSTOPしない0x95切替では、
        // RAW経路と同じく入力側ADPCMデコーダとDCフィルタの履歴を継続する。
        portENTER_CRITICAL(&okim6258Mux);
        preserveDecoderState = okim6258.state.isPlaying;
        portEXIT_CRITICAL(&okim6258Mux);
        okim6258.setVgmPcmPlaying(false, false, preserveDecoderState);
      }
      portENTER_CRITICAL(&okim6258Mux);
      okim6258.state.posInBlock[0] = 0;
      okim6258.state.currentBlockId[0] = blockId;
      portEXIT_CRITICAL(&okim6258Mux);

      if (reencode) {
        okim6258.setVgmPcmPlaying(true, false, preserveDecoderState);
        if (!okim6258.state.vgmPhysicalPlaying) {
          nju72342.panMute();
          FM.setOKIM6258command(0b00000010, 1);
          okim6258.state.vgmPhysicalPlaying = true;
          ets_delay_us(260);
          nju72342.panUnmute();
        }
      } else if (okim6258.state.isPlaying == false) {
        okim6258.state.isPlaying = true;
        nju72342.panMute();

        FM.setOKIM6258command(0b00000010, 1);
        okim6258.state.vgmPhysicalPlaying = true;
        ets_delay_us(260);
      }

      if (!okim6258.isVgmPcmReencodeActive()) {
        nju72342.panUnmute();
      }
      setTrackDisplayLevel(kVgmOkimTrackNo, kVgmOkimLevelOn);

      // Serial.printf("0x95:(fast) block id %d\n", blockId);
      break;
    }
#ifdef USE_YM2612
    case 0xe0:
      //_pcmpos = 0x47 + ndFile.get_ui32();
      break;
#endif
#ifdef USE_OKIM6258
    case 0xb7: {
      reg = ndFile.get_ui8();
      dat = ndFile.get_ui8();

      switch (reg) {
        case 0x00: {
          // 再生制御
          switch (dat) {
            case 0b00000001:  // 停止
              if (okim6258.isVgmPcmReencodeActive()) {
                okim6258.setVgmPcmPlaying(false);
                if (okim6258.dataBlocks.size() == 0) {
                  okim6258.state.cache.clear();
                }
                setTrackDisplayLevel(kVgmOkimTrackNo, 0);
                break;
              }
              //   再生処理中
              if (okim6258.state.isPlaying || okim6258.state.vgmPhysicalPlaying) {
                okim6258.state.isPlaying = false;
                nju72342.panMute();
                FM.setOKIM6258command(0b00000001, 1);
                okim6258.state.vgmPhysicalPlaying = false;
                ets_delay_us(260);
              }
              setTrackDisplayLevel(kVgmOkimTrackNo, 0);
              if (okim6258.dataBlocks.size() == 0) {  // ブロック化されてないとき
                portENTER_CRITICAL(&okim6258Mux);
                okim6258.state.cache.clear();
                portEXIT_CRITICAL(&okim6258Mux);
              }
              // Serial.printf("OKIM6258制御: 停止\n");
              break;
            case 0b00000010:  // 開始
              if (okim6258.isVgmPcmReencodeActive()) {
                okim6258.setVgmPcmPlaying(true);
                if (!okim6258.state.vgmPhysicalPlaying) {
                  nju72342.panMute();
                  FM.setOKIM6258command(0b00000010, 1);
                  okim6258.state.vgmPhysicalPlaying = true;
                  ets_delay_us(260);
                  nju72342.panUnmute();
                }
                setTrackDisplayLevel(kVgmOkimTrackNo, kVgmOkimLevelOn);
                break;
              }
              if (okim6258.state.isPlaying == false) {
                okim6258.state.isPlaying = true;
                nju72342.panMute();

                FM.setOKIM6258command(0b00000010, 1);
                okim6258.state.vgmPhysicalPlaying = true;
                ets_delay_us(260);
                nju72342.panUnmute();
              }
              setTrackDisplayLevel(kVgmOkimTrackNo, kVgmOkimLevelOn);
              // Serial.printf("OKIM6258制御: 開始\n");
              break;
            case 0b100:
              // OKIM6258制御: 録音
              // 使わない
              break;
            default:
              // FM.setOKIM6258command(dat, 1);
              // Serial.printf("OKIM6258制御不明: 0x%x\n", dat);
              break;
          }
          break;
        }
        case 0x01: {  // ADPCM data
          if (!okim6258.state.cache.push(dat)) {
            portENTER_CRITICAL(&okim6258Mux);
            okim6258.state.vgmInputOverflows++;
            portEXIT_CRITICAL(&okim6258Mux);
          }
          okim6258.requestVgmPcmEncode();
          break;
        }

        case 0x02: {
          // パン
          okim6258.state.pan = dat;
          switch (dat) {
            case 0x00:
              // Serial.printf("OKIM6258パン: センター\n");
              nju72342.panSetPan(PAN_CENTER);
              KeyBoard.trackPan[8] = PAN_CENTER;
              break;
            case 0x02:
              // Serial.printf("OKIM6258パン: 右\n");
              nju72342.panSetPan(PAN_RIGHT);
              KeyBoard.trackPan[8] = PAN_RIGHT;
              break;
            case 0x01:
              // Serial.printf("OKIM6258パン: 左\n");
              nju72342.panSetPan(PAN_LEFT);
              KeyBoard.trackPan[8] = PAN_LEFT;
              break;
            case 0x03:
              nju72342.panSetPan(PAN_MUTE);
              KeyBoard.trackPan[8] = PAN_MUTE;
              //      Serial.printf("OKIM6258パン: ミュート\n");
              break;
            default:
              break;
          }
          break;
        }
        case 0x08 ... 0x0b: {
          // Serial.printf("OKIM6258周波数設定:%x, %x\n", reg, dat);
          //  0x08 は未使用か?

          // 4MHz: 0x09: 0x09, 0x0a: 0x3d, 0x0b: 0x00
          // 8MHz: 0x09: 0x12, 0x0a: 0x7a, 0x0b: 0x00
          if (reg == 0x0a && dat == 0x3d) {
            ND::freq[1] = SI5351_4000;
            SI5351.setFreq(ND::freq[1], 1);
            // Serial.printf("freq1: %d\n", ND::freq[1]);
          } else if (reg == 0x0a && dat == 0x7a) {
            ND::freq[1] = SI5351_8000;
            SI5351.setFreq(ND::freq[1], 1);
            // Serial.printf("freq1: %d\n", ND::freq[1]);
          }
          break;
        }
        case 0x0c: {
          // 分周
          okim6258.state.divider = (dat & 0x03) == 0   ? OKIM6258_DIV_1024
                                   : (dat & 0x03) == 1 ? OKIM6258_DIV_768
                                                       : OKIM6258_DIV_512;  // 分周
          FM.setOKIM6258divider(okim6258.state.divider);

          break;
        }
        case 0x10 ... 0x17: {
          /*
          デバッグ情報
          0x10〜0x13 … HD63450 DMAC Ch3 の MAR（Memory Address Register） をバイト分割してログ
          0x10/0x11 = MAR high の上位/下位バイト
          0x12/0x13 = MAR low の上位/下位バイト
          0x14〜0x15 … HD63450 DMAC Ch3 の MTC（Memory Transfer Counter） の上位/下位バイト
          0x17 … HD63450 DMAC Ch3 の SCR/CCR（制御レジスタ）の下位8bit（コードコメントでも
          “Control Write” としてログ）
          */
          // Serial.printf("OKIM6258 DMAログ: %x\n", dat);
          break;
        }

        default: {
          // Serial.printf("OKIM6258 未定義: 0x%x 0x%x\n", reg, dat);
          break;
        }
      }
      break;
    }
#endif
    default:
      Serial.printf("Unknown VGM Command: %0.2X at pos: 0x%X\n", command, ndFile.pos - 1);
      break;
  }
}

//---------------------------------------------------------------
// 曲終了時の処理
void VGM::endProcedure() {
  if (okim6258.isVgmPcmReencodeActive()) {
    // DAOUTがVDD/2からGNDへ戻る前に左右出力をミュートする。
    nju72342.panMute();
  }
  ND::canPlay = false;

  if (okim6258.isVgmPcmReencodeActive()) {
    FM.setOKIM6258command(0b00000001, 1);
    okim6258.state.vgmPhysicalPlaying = false;
    ets_delay_us(260);
    okim6258.endVgmPcmReencode();
  }

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

u64_t VGM::getCurrentTimeSec() {
  if (_vgmSamples >= 264600000) _vgmSamples = 0;
  return _vgmSamples / 44100;
}

u64_t VGM::getCurrentTimeSubSec() {
  if (_vgmSamples >= 264600000) _vgmSamples = 0;
  return _vgmSamples / 4410;
}

VGM vgm = VGM();
