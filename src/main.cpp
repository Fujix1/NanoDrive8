/**
 * Nano Drive Firmware
 * 2024 - 2026 (C) Fujix
 * e2j.net
 *
 * This software uses the following libraries:
 *
 *  Open Font Render
 *  URL: https://github.com/takkaO/OpenFontRender
 *  Author: takkaO
 *  License: FreeType License
 *  Portions of this software are copyright © The FreeTypeProject (www.freetype.org).
 *  All rights reserved.
 *
 *  LovyanGFX
 *  URL: https://github.com/lovyan03/LovyanGFX
 *  Author: lovyan03
 *  License: FreeBSD
 *
 *  PNGdec
 *  URL: https://github.com/bitbank2/PNGdec
 *  Author: Larry Bank
 *  License: Apache-2.0
 *
 *  Adafruit TCA8418
 *  URL: https://github.com/adafruit/Adafruit_TCA8418
 *  Author: Adafruit
 *  License: BSD-3-Clause
 *
 *  Adafruit AW9523
 *  URL: https://github.com/adafruit/Adafruit_AW9523
 *  Author: Adafruit
 *  License: BSD-3-Clause
 *
 *  Adafruit BusIO
 *  URL: https://github.com/adafruit/Adafruit_BusIO
 *  Author: Adafruit
 *  License: MIT
 *
 *  LZEXE
 *  URL: https://bellard.org/lzexe/
 *  Author: Fabrice Bellard
 *  License: MIT
 *
 *  BIZ UDPGothic
 *  URL:https://fonts.google.com/specimen/BIZ+UDPGothic/license
 *  License: SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007
 *  Copyright 2022 The BIZ UDGothic Project Authors
 *  (https://github.com/googlefonts/morisawa-biz-ud-mincho) This Font Software is
 *  licensed under the SIL Open Font License, Version 1.1 . This license is
 *  copied below, and is also available with a FAQ at:
 *  https://openfontlicense.org
 *
 */

#include "config.h"
//--
#include "NJU72342.h"
#include "SI5351.hpp"
#include "common.h"
#include "disp.h"
#include "file.h"
#include "fm.h"
#include "input.h"
#include "leds.h"
#include "mdx.h"
#include "okim6258.h"
#include "vgm.h"

void setup() {
  disableCore0WDT();  // ウォッチドッグ0無効化
  digitalWrite(D0, HIGH);

  pinMode(D0, OUTPUT);
  pinMode(WR, OUTPUT);
  pinMode(CS0, OUTPUT);  // 起動時出力に設定必須
  pinMode(CS1, OUTPUT);
  pinMode(CS2, OUTPUT);
  pinMode(A0, OUTPUT);
  pinMode(A1, OUTPUT);

  Serial.begin(115200);
  Serial.printf("Heap - %'d Bytes free\n", ESP.getFreeHeap());
  Serial.printf("Flash - %'d Bytes at %'d\n", ESP.getFlashChipSize(), ESP.getFlashChipSpeed());
  Serial.printf("PSRAM - Total %'d, Free %'d\n", ESP.getPsramSize(), ESP.getFreePsram());

  delay(100);

  Wire.begin(I2C_SDA, I2C_SCL, I2C_CLOCK);

  // ディスプレイ初期化
  if (!disp.init()) {
    Serial.println("disp.init() failed.");
  }

  lcd.setFont(&fonts::Font2);
  lcd.println("NANO DRIVE 8");
  lcd.println("2024 -2026 Fujix@e2j.net");
  lcd.printf("Firmware: 1.0b6\n\n");

  delay(100);  // 安定用必須

  // ﾆｭｳﾘｮｸ
  if (input.init()) {
    lcd.printf("Input initialized.\n");
  } else {
    lcd.printf("[ERROR] Input IC TCA8418 failed.\n");
  }
  delay(100);

  // PSRAM 初期化確認
  if (psramInit()) {
    lcd.printf("PSRAM initialized.\n");
  } else {
    lcd.printf("[ERROR] PSRAM not available.\n");
    exit;
  }

  // ユーザ設定
  ndConfig.init();
  ndConfig.loadCfg();
  const tLastView savedLastView = ndConfig.loadLastView();

  // NJU72342 初期化
  nju72342.init(I2C_SDA, I2C_SCL, I2C_CLOCK, ndConfig.get(CFG_FADEOUT), true);
  ndConfig.applyCfg();

  // SI5351 初期化
  SI5351.begin(I2C_SDA, I2C_SCL, I2C_CLOCK);
  SI5351.setFreq(SI5351_4000, 0);
  SI5351.setFreq(SI5351_4000, 1);
  SI5351.enableOutputs(true);

  // VGM用GPIO初期化
  // Lovyanの初期化で上書きされるので、initDisp();の後に呼び出す
  FM.begin();
  FM.reset();

  // ビジュアル初期化
  visualWindow.init();

  // SD初期化
  if (!ndFile.init()) {
    ESP.restart();
  }

  Serial.printf("Heap - %'d Bytes free, Min free heap %'d\n", ESP.getFreeHeap(),
                ESP.getMinFreeHeap());
  Serial.printf("Flash - %'d Bytes at %'d\n", ESP.getFlashChipSize(), ESP.getFlashChipSpeed());
  Serial.printf("PSRAM - Total %'d, Free %'d\n", ESP.getPsramSize(), ESP.getFreePsram());
  Serial.printf("Heap largest free block - %'d\n",
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  //

  if (fileTree.begin("/")) {
    // lcd.printf("Files OK. %d\n", fileTree.getTotalFiles());
  } else {
    lcd.printf("[ERROR] No music file.\n");
    exit;
  }

  if (Leds.init()) {
    Leds.startupTest();
    Leds.setAll(0);
    FM.refreshChannelMaskLeds();
  }

  MDX.init();

  // OKIM6258割り込み初期化
  okim6258.init();

  vgm.init();
  cfgWindow.init();
  browserWindow.init();

  // 入力有効化
  input.setEnabled(true);

  // 読み込み履歴復元新処理
  String savedNodePath = ndConfig.loadNodePath();
  Node* savedNode = fileTree.findNodeByPath(savedNodePath);
  Node* firstDir = fileTree.getNextDirNode(fileTree.getRoot());
  Node* firstFile = firstDir ? fileTree.getNextFileNode(firstDir, false) : nullptr;
  ndFile.currentNode = firstFile;

  switch (ndConfig.get(CFG_HISTORY)) {
    case HISTORY_FOLDER: {
      if (savedNode) {
        Node* savedDir = (savedNode->type == NODE_TYPE_DIR) ? savedNode : savedNode->parent;
        if (savedDir && savedDir != fileTree.getRoot()) {
          Node* savedFirstFile = fileTree.getNextFileNode(savedDir, false);
          if (savedFirstFile) {
            ndFile.currentNode = savedFirstFile;
          }
        }
      }
      break;
    }
    case HISTORY_FILE: {
      if (savedNode) {
        if (savedNode->type == NODE_TYPE_FILE) {
          ndFile.currentNode = savedNode;
        } else {
          Node* firstFile = fileTree.getNextFileNode(savedNode, false);
          if (firstFile) {
            ndFile.currentNode = firstFile;
          }
        }
      }
      break;
    }
    default:
      break;
  }

  if (!ndFile.currentNode) {
    lcd.printf("[ERROR] No playable file.\n");
    exit;
  }

  if (savedLastView == LAST_VIEW_VISUAL) {
    disp.currentView = ViewMode::Visual;
    disp.lastView = ViewMode::Visual;
  } else {
    disp.currentView = ViewMode::Player;
    disp.lastView = ViewMode::Player;
  }

  ndFile.openFile(fileTree.getFullPath(ndFile.currentNode));

  if (savedLastView == LAST_VIEW_VISUAL) {
    visualWindow.show();
  } else {
    playerWindow.show();
  }

  vgm.startTick = micros64();
}

void loop() {
  while (1) {
    FM.applyPendingChannelMask();

    if ((ND::fileFormat == FileFormat::VGM || ND::fileFormat == FileFormat::VGZ) && ND::canPlay &&
        !ND::isPaused) {
      vgm.vgmProcess();
    }
#ifdef USE_MDX
    else if (ND::fileFormat == FileFormat::MDX && ND::canPlay && !ND::isPaused) {
      MDX.process();
    }
#endif
    ndFile.processPendingPlayRequest();
    input.inputHandler();
  }
}
