#include "./config.h"

#include <Arduino.h>
#include <FS.h>
#include <Preferences.h>

#include <vector>

#include "./file.h"
#include "NJU72342.h"
#include "fm.h"
#include "input.h"
#include "okim6258.h"

Preferences preferences;
static constexpr const char* kLastViewKey = "last_view";
static portMUX_TYPE lastViewMux = portMUX_INITIALIZER_UNLOCKED;

static tNJU7234X_GAIN amplifyToInputGain(int amplify) {
  switch (amplify) {
    case AMP_0:
      return GAIN0;
    case AMP_6:
      return GAIN6;
    case AMP_9:
      return GAIN9;
    case AMP_3:
    default:
      return GAIN3;
  }
}

static const char* configSlug(tConfig item) {
  switch (item) {
    case CFG_LANG:
      return "lang";
    case CFG_SHUFFLE:
      return "shuffle";
    case CFG_NUM_LOOP:
      return "loop";
    case CFG_REPEAT:
      return "repeat";
    case CFG_SCROLL:
      return "scroll";
    case CFG_HISTORY:
      return "resume";
    case CFG_FADEOUT:
      return "fadeout";
    case CFG_YM2151_PAN:
      return "ym2151pan";
    case CFG_M6258_PAN:
      return "m6258pan";
    case CFG_AMPLIFY:
      return "amplify";
    case CFG_ADPCM:
      return "adpcm";
    case CFG_LED:
      return "led";
    case CFG_KEYON:
      return "keyon";
    case CFG_PAUSE:
      return "pause";
    case CFG_CONTROL:
      return "control";
    case CFG_VWROTATE:
      return "vwrotate";
    default:
      return "";
  }
}

static tConfig configFromSlug(const String& slug) {
  for (int i = CFG_LANG; i < CFG_UNKNOWN; i++) {
    const tConfig item = static_cast<tConfig>(i);
    if (slug == configSlug(item)) {
      return item;
    }
  }
  return CFG_UNKNOWN;
}
void _saveCFGonCore0(void* param) {
  for (int i = 0; i < ndConfig.items.size(); i++) {
    preferences.putUChar(ndConfig.items[i].slug.c_str(), ndConfig.items[i].index);
  }
  vTaskDelete(NULL);
}

void NDConfig::init() {
  items.push_back({"lang",
                   LANG_JA,
                   "Language",
                   "Language",
                   {"日本語", "英語"},
                   {"Japanese", "English"},
                   {LANG_JA, LANG_EN}});

  items.push_back({"shuffle",
                   0,  // 初期値idx
                   "シャッフル",
                   "Shuffle",
                   {"なし", "フォルダ内", "全曲"},
                   {"No", "Folder", "All"},
                   {TRANDOM_NO, TRANDOM_FOLDER, TRANDOM_ALL}});
  items.push_back({"adpcm",
                   0,  // 初期値idx
                   "PCM",
                   "PCM",
                   {"RAWデータ", "再サンプル", "実機風フィルタ"},
                   {"Raw", "Resample", "Band-pass"},
                   {ADPCM_THROUGH, ADPCM_RESAMPLE, ADPCM_LPF}});
  items.push_back({
      "loop",
      0,  // 初期値idx
      "曲ループ",
      "Song Loop",
      {"1回", "2回", "3回", "4回", "5回", "無制限"},
      {"1", "2", "3", "4", "5", "Infinite"},
      {LOOP_1, LOOP_2, LOOP_3, LOOP_4, LOOP_5, LOOP_INIFITE},
  });
  items.push_back({"repeat",
                   0,  // 初期値idx
                   "リピート",
                   "Repeat",
                   {"全曲", "フォルダ", "1曲"},
                   {"All", "Folder", "One Song"},
                   {REPEAT_ALL, REPEAT_FOLDER, REPEAT_ONE}});
  items.push_back({"scroll",
                   3,  // 初期値idx
                   "スクロール",
                   "Text scroll",
                   {"なし", "1回", "2回", "無制限"},
                   {"None", "1", "2", "Infinite"},
                   {SCROLL_0, SCROLL_1, SCROLL_2, SCROLL_INFINITE}});
  items.push_back({"resume",
                   2,  // 初期値idx
                   "起動時",
                   "Resume",
                   {"初めから再生", "最終フォルダ", "最後の曲"},
                   {"No", "Last Folder", "Last Song"},
                   {HISTORY_NONE, HISTORY_FOLDER, HISTORY_FILE}});
  items.push_back({"fadeout",
                   4,  // 初期値idx
                   "フェードアウト",
                   "Fadeout",
                   {"なし", "2秒", "5秒", "8秒", "10秒", "12秒", "15秒"},
                   {"None", "2 sec.", "5 sec.", "8 sec.", "10 sec.", "12 sec.", "15 sec."},
                   {FO_0, FO_2, FO_5, FO_8, FO_10, FO_12, FO_15}});
  items.push_back({"pause",
                   0,  // 初期値idx
                   "再生ホールド",
                   "Play Hold",
                   {"オフ", "オン", "3秒前"},
                   {"Off", "On", "3 sec."},
                   {HOLD_NONE, HOLD_YES, HOLD_3SEC}});
  items.push_back({"amplify",
                   1,  // 初期値idx
                   "出力増幅",
                   "Output Gain",
                   {"0dB", "3dB", "6dB", "9dB"},
                   {"0dB", "3dB", "6dB", "9dB"},
                   {AMP_0, AMP_3, AMP_6, AMP_9}});

  items.push_back({"ym2151pan",
                   0,  // 初期値idx
                   "YM2151パン",
                   "YM2151 Pan",
                   {"ふつう", "反転"},
                   {"Normal", "Invert"},
                   {TPAN_NORMAL, TPAN_INVERT}});
  items.push_back({"m6258pan",
                   0,  // 初期値idx
                   "M6258パン",
                   "M6258 Pan",
                   {"ふつう", "反転"},
                   {"Normal", "Invert"},
                   {TPAN_NORMAL, TPAN_INVERT}});

  items.push_back({"led",
                   1,  // 初期値idx
                   "LED明度",
                   "LED",
                   {"消灯", "暗め", "ふつう", "明るめ"},
                   {"Off", "Dimmed", "Normal", "Bright"},
                   {BRIGHTNESS_0, BRIGHTNESS_1, BRIGHTNESS_2, BRIGHTNESS_3}});
  items.push_back({"keyon",
                   0,  // 初期値idx
                   "キーオン色",
                   "Keyon Color",
                   {"赤", "緑", "青"},
                   {"Red", "Green", "Blue"},
                   {KEYON_RED, KEYON_GREEN, KEYON_BLUE}});

  items.push_back({"control",
                   0,  // 初期値idx
                   "キー割り当て",
                   "Key Assign",
                   {"セット1", "セット2"},
                   {"Set 1", "Set 2"},
                   {CTRL_1, CTRL_2}});

  items.push_back({"vwrotate",
                   0,  // 初期値idx
                   "画面方向*",
                   "Visual Rotate*",
                   {"左向き", "右向き"},
                   {"Left", "Right"},
                   {VW_ROTATE_OFF, VW_ROTATE_ON}});

  preferences.begin("NanoDrive");
}

void NDConfig::applyCfg() {
  nju72342.setFadeoutDuration(get(CFG_FADEOUT));
  const tNJU7234X_GAIN inputGain = amplifyToInputGain(get(CFG_AMPLIFY));
  nju72342.setInputGain(3, inputGain);
  nju72342.setInputGain(4, inputGain);
  FM.refreshChannelMaskLeds();
  // Play Hold設定だけは、現在ホールド中の再生にも即時反映する。
  syncPlayHoldConfig();
  // ADPCM処理を現在の再生へ反映する。
  okim6258.applyAdpcmConfigRealtime();
}

// 設定保存
void NDConfig::saveCfg() {
  xTaskCreateUniversal(_saveCFGonCore0, "saveCFG", 10000, NULL, 1, NULL, PRO_CPU_NUM);
  applyCfg();
  return;
}

// 最後に開いたノード保存
void NDConfig::saveNodePath(const String& nodePath) {
  preferences.putString("node_path", nodePath);
  // Serial.printf("save: node_path %s\n", nodePath.c_str());
}

// 表示ウインドウ保存
void NDConfig::saveLastView(tLastView view) {
  portENTER_CRITICAL(&lastViewMux);
  if (_lastView != view) {
    _lastView = view;
    _lastViewDirty = true;
  }
  portEXIT_CRITICAL(&lastViewMux);
}

// 再生中のFlash書き込みを避け、曲切り替え時に最後の表示ウインドウを保存
void NDConfig::flushLastView() {
  tLastView pendingView;

  portENTER_CRITICAL(&lastViewMux);
  if (!_lastViewDirty) {
    portEXIT_CRITICAL(&lastViewMux);
    return;
  }
  pendingView = _lastView;
  _lastViewDirty = false;
  portEXIT_CRITICAL(&lastViewMux);

  if (preferences.putUChar(kLastViewKey, static_cast<u8_t>(pendingView)) == 0) {
    portENTER_CRITICAL(&lastViewMux);
    _lastViewDirty = true;
    portEXIT_CRITICAL(&lastViewMux);
  }
}

// 設定ロード
void NDConfig::loadCfg() {
  for (int i = 0; i < ndConfig.items.size(); i++) {
    u8_t idx = preferences.getUChar(ndConfig.items[i].slug.c_str(), ndConfig.items[i].index);
    // 範囲チェック
    if (ndConfig.items[i].optionValues.size() > idx) {
      ndConfig.items[i].index = idx;
    }
  }
}

// 最後に開いたノード取得
String NDConfig::loadNodePath() {
  return preferences.getString("node_path");
}

tLastView NDConfig::loadLastView() {
  const u8_t rawView = preferences.getUChar(kLastViewKey, static_cast<u8_t>(LAST_VIEW_PLAYER));
  portENTER_CRITICAL(&lastViewMux);
  _lastView =
      (rawView == static_cast<u8_t>(LAST_VIEW_VISUAL)) ? LAST_VIEW_VISUAL : LAST_VIEW_PLAYER;
  _lastViewDirty = false;
  portEXIT_CRITICAL(&lastViewMux);
  return _lastView;
}

void NDConfig::remove() {
  preferences.clear();
}

int NDConfig::get(tConfig item) {
  const int itemIndex = indexOf(item);
  if (itemIndex < 0) {
    return 0;
  }

  const sConfig& config = items[itemIndex];
  if (config.index >= config.optionValues.size()) {
    return 0;
  }
  return config.optionValues[config.index];
}

int NDConfig::indexOf(tConfig item) {
  const char* slug = configSlug(item);
  if (slug[0] == '\0') {
    return -1;
  }
  for (int i = 0; i < items.size(); i++) {
    if (items[i].slug == slug) {
      return i;
    }
  }
  return -1;
}

tConfig NDConfig::configAt(int index) {
  if (index < 0 || index >= items.size()) {
    return CFG_UNKNOWN;
  }
  return configFromSlug(items[index].slug);
}

NDConfig ndConfig = NDConfig();
