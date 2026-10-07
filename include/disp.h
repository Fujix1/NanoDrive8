#ifndef DISP_H
#define DISP_H

#include <LovyanGFX.h>
#include <SPI.h>
#include <stdint.h>

#include "OpenFontRender.h"
#include "common.h"
#include "config.h"
#include "file.h"
#include "fonts.h"
#include "input.h"
#include "keyinfo.h"
#include "mdx.h"
#include "vgm.h"

struct Node;

#define C_BASEBG TFT_BLACK
#define C_BASEFG TFT_WHITE
#define C_RED 0xd003        // #d60019
#define C_ORANGE 0xfdc7     // #ffba3a
#define C_TANGERINE 0xec80  // #ff9d00
#define C_YELLOW 0xfee0     // #ffdf00
#define C_CYAN 0xfa9e       // #ff51f0
#define C_BLUE 0x7d7f       // #7aadff
#define C_LIME 0xd7c0       // #d6fb00
#define C_MDX 0x9cfd        // #a4a2eb
#define C_GREEN 0x67e1      // #63ff08

#define C_ACCENT_LIGHTER 0xd75e  // #d6ebf7
#define C_ACCENT_LIGHT 0x1e7e    // #1bcdfa
#define C_ACCENT_DARK 0x1475     // #007e9e
#define C_LISTBG 0xdf3d          // #e2e8ed

#define C_LIGHTGRAY 0xef7d  // #efefef
#define C_HIGHGRAY 0xd6da   // #d9d9d9
#define C_GRAY 0xad55       // #adaaad
#define C_MID 0x73ae        // #737573
#define C_DARK 0x10c4       // #101821
#define C_LV_PEAK 0x52b5    // #5255ad
#define C_MASKEDKEY 0x322f  // #324579
#define C_MDX_ON 0x843f     // #8787ff
#define C_MDX_OFF 0x212a    // #23234f

#define C_HEADER 0x2966           // #292c2e
#define C_HEADERSUB 0x5aec        // #5b5c61
#define C_BORDER 0xad55           // #adaaad
#define C_FOOTER_ACTIVE 0x530c    // #506065
#define C_FOOTER_INACTIVE 0xbe1a  // #bbc0d0

#define CFG_ITEM_HEIGHT 32  // 設定項目1つの高さ

// font icon
// 丂 - random

class LGFX : public lgfx::LGFX_Device {
 private:
  lgfx::Panel_ST7789 _panel_instance;

 public:
  lgfx::Bus_SPI _bus_instance;
  LGFX(void);
};

extern LGFX lcd;

typedef struct {
  String trackEn, trackJp, gameEn, gameJp, systemEn, systemJp, authorEn, authorJp, date;
  String chip0, chip1, type;
  int64_t time;
  u32_t no, maxFiles;
} tDispData;

// void redrawOnCore0();
bool openPNG(String path, bool AA, bool sprite);
void serialModeUpdateFooter();
void serialModeDraw();  // シリアルモード画面描画

// 現在の画面表示モード
enum class ViewMode { Player, Config, Serial, Visual, Browser };

// 画面 クラス
class Disp {
 private:
 public:
  bool init();  // 初期化
  void startTimer();  // 描画タイマー開始
  void stopTimer();   // 描画タイマー停止

  bool stopTimerDrawing;                    // タイマー描画更新停止
  ViewMode currentView = ViewMode::Player;  // デフォルトはプレーヤー画面
  ViewMode lastView = currentView;          // 前の表示がプレーヤーかビジュアルかを保持
  OpenFontRender render;                    // OpenFontRenderer
};

extern Disp disp;

// Label クラス
class Label {
 public:
  Label(const int16_t x,  // 座標
        const int16_t y,
        const int16_t w,  // 幅
        const u16_t fontColor, const u16_t bgColor, const u16_t fontSize, const float scrollSpeed,
        const Align textAlign);
  void prepareCaption(const String& newCaption);
  void drawPrepared();
  void setCaption(const String& newCaption);
  void update();
  void setEnabled(bool state);

 private:
  float _n = 0;  // label scroll offset
  int32_t _lastDrawOffset = -1;
  u32_t _x = 0, _y = 0, _labelWidth = 0, _textWidth = 0, _devWidth = 0, _startTick = 0;
  u16_t _fontColor;
  u16_t _fontSize;
  u16_t _bgColor;
  String _caption;
  LGFX_Sprite _sprite;
  Align _textAlign;
  int _scrollCount;  // スクロール済み回数
  bool _isScrolling = false;
  float _scrollSpeed;
  bool _enabled = false;
};

// 左に90度回転したラベル
class RotatedLabel {
 public:
  RotatedLabel(const int16_t x,  // 座標
               const int16_t y,
               const int16_t h,  // 高さ
               const u16_t fontColor, const u16_t bgColor, const u16_t fontSize,
               const float scrollSpeed, const Align textAlign);
  void prepareCaption(const String& newCaption);
  void drawPrepared();
  void setCaption(const String& newCaption);
  void update();
  void setEnabled(bool state);

 private:
  float _n = 0;  // label scroll offset
  int32_t _lastDrawOffset = -1;
  u32_t _x = 0, _y = 0, _labelHeight = 0, _textWidth = 0, _devWidth = 0, _startTick = 0;
  u16_t _fontColor;
  u16_t _fontSize;
  u16_t _bgColor;
  String _caption;
  LGFX_Sprite _sprite;
  Align _textAlign;
  int _scrollCount = 0;  // スクロール済み回数
  bool _isScrolling = false;
  float _scrollSpeed = 0;
  bool _enabled = false;
};

//------------------------------------------------------------------
// Panel クラス
static constexpr u8_t CURRENTDIRHEIGHT = 28;
static constexpr u8_t NODEITEMHEIGHT = 28;
void printWithEllipsis(OpenFontRender& render, const char* text, int x, int y, int maxWidth,
                       int dotsWidth = -1);

// パネル描画インタフェース
class IPanelRenderer {
 public:
  virtual ~IPanelRenderer() = default;
  virtual void onDrawItem(LGFX_Sprite& target, int itemIndex, int x, int y, int width,
                          bool selected) = 0;
  virtual void onClick(int itemIndex) = 0;
};

// ファイルブラウザの描画クラス
class BrowserPanelRenderer : public IPanelRenderer {
 public:
  void init();
  void setBrowseDirNode(Node* browseDirNode);
  bool hasParentEntry() const;
  Node* getNodeByDisplayIndex(int itemIndex) const;
  void onDrawItem(LGFX_Sprite& target, int itemIndex, int x, int y, int width,
                  bool selected) override;
  void onClick(int index) override;

 private:
  Node* _browseDirNode = nullptr;
  int _dotsWidth = 0;
};

// 設定画面の描画クラス
class ConfigPanelRenderer : public IPanelRenderer {
 public:
  void onDrawItem(LGFX_Sprite& target, int itemIndex, int x, int y, int width,
                  bool selected) override;
  void onClick(int itemIndex) override;
};

class Panel {
 public:
  Panel(u16_t x,  // 座標
        u16_t y,
        u16_t w,  // 幅高
        u16_t h, u8_t itemHeight, IPanelRenderer* renderer)
      : x(x), y(y), width(w), height(h), _itemHeight(itemHeight), _renderer(renderer) {
    scrollTop = 0;

    _maxBarHeight = height - INDICATORHEIGHT * 2;  // スクロールバー最大となる高さ
    _borderRadius = (SCROLLBARWIDTH - PADDING - PADDING) / 2;

    // スクロールバー背景初期化
    //_spriteScrollBar.setPsram(true);
    _spriteScrollBar.createSprite(SCROLLBARWIDTH, height);

    //_spriteScrollBarBG.setPsram(true);
    _spriteScrollBarBG.createSprite(SCROLLBARWIDTH, height);
    _spriteScrollBarBG.fillSprite(C_HIGHGRAY);
    //_spriteScrollBar.drawFastVLine(0, 0, height, C_BORDER);
    _spriteScrollBarBG.fillTriangle(SCROLLBARWIDTH / 2, 3, SCROLLBARWIDTH - 2, SCROLLBARWIDTH - 3,
                                    2, SCROLLBARWIDTH - 3, C_MID);
    _spriteScrollBarBG.fillTriangle(SCROLLBARWIDTH / 2, height - 3, SCROLLBARWIDTH - 2,
                                    height - SCROLLBARWIDTH + 3, 2, height - SCROLLBARWIDTH + 3,
                                    C_MID);
  }

  int scrollTop;
  void update(LGFX_Sprite& frameBuffer);  // 表示更新
  void drawScrollbar();                   // スクロールバーバッファに描画
  u16_t x, y, width, height;
  int currentIndex = 0;

  void setItemCount(int newCount);
  void ensureVisible();
  void invalidate();
  void resetScroll();
  void clickCurrentItem();
  u16_t itemWidth() const {
    return width - SCROLLBARWIDTH;
  }

 private:
  static constexpr u8_t SCROLLBARWIDTH = 12;   // スクロールバー幅
  static constexpr u8_t PADDING = 3;           // スクロールバー PADDING
  static constexpr u8_t INDICATORHEIGHT = 14;  // ▲ の高さ

  u16_t _maxBarHeight;
  u8_t _borderRadius;

  u16_t _innerHeight = 0;
  int _itemCount = 0;
  int _itemHeight = 0;  // 項目の高さ
  int _prevCurrentIndex = -1;
  int _prevScrollTop = 0;
  int _prevItemCount = -1;
  bool _needsFullRedraw = true;
  LGFX_Sprite _spriteScrollBarBG;       //
  LGFX_Sprite _spriteScrollBar;         // スクロールバーのスプライト
  IPanelRenderer* _renderer = nullptr;  // 描画更新処理

  void redrawScrollEdgeItems(LGFX_Sprite& targetBuffer, int scrollDelta);
  void redrawItem(LGFX_Sprite& targetBuffer, int index);
  void redrawVisibleItems(LGFX_Sprite& targetBuffer);
};

// プレイヤー画面クラス
class PlayerWindow {
 public:
  PlayerWindow() : _sprHeader(&lcd) {
  }
  void init();
  void drawBG();                    // 背景描画
  void redraw();                    // プレーヤー描画
  void updateDisp(tDispData data);  // 表示内容更新
  void updateHeader(int64_t sec) {  // ヘッダ更新
    updateHeader(sec, true);
  }
  void updateHeader(int64_t sec, bool visible) {
    updateHeader(sec, visible, 0);
  }
  void updateHeaderBlocking(int64_t sec) {
    updateHeader(sec, true, portMAX_DELAY);
  }
  tDispData dispData;               // 各種表示テキストなど
  void eventHandler(event event);
  void show();  // 表示

 private:
  void updateHeader(int64_t sec, bool visible, uint32_t ticksToWait);
  LGFX_Sprite _sprHeader;  // ヘッダ部分のスプライト
};

extern PlayerWindow playerWindow;

// 設定画面クラス
class CFGWindow {
 private:
  LGFX_Sprite _sprite;
  LGFX_Sprite _sprFooter;
  bool _isChanged = false;
  ViewMode _returnView = ViewMode::Visual;
  // 言語別ヘッダー用スプライト
  LGFX_Sprite _sprHeaderJP;
  LGFX_Sprite _sprHeaderEN;

  // ヘッダースプライトの初期化
  void initHeaders();
  void drawPanelView();
  void refreshPanel();
  void refreshCurrentItem();
  int getItemCount() const;
  void selectItem(int index);
  void moveSelection(int delta);

 public:
  bool _isOptionMode;  // オプション変更モード
  int currentItemIndex = 0;

  void init();
  void drawItem(LGFX_Sprite& target, int index, int x, int y, int width, bool selected);
  void drawFooter(bool toFrameBuffer);
  void eventHandler(event event);
  void show();  // 表示
  void close();
};

extern CFGWindow cfgWindow;

// ビジュアル画面クラス
class VisualWindow {
 private:
  boolean drawKeyboard(LGFX_Sprite& sprite, t_device device, const NoteInfo* notes);
  boolean drawPan(u8_t trackNo, tPan pan);
  boolean drawLevel(u8_t trackNo, u8_t level, u8_t peakLevel);
  boolean drawNote(u8_t trackNo, u8_t noteNo);
  LGFX_Sprite _sprShuffleOn;
  LGFX_Sprite _sprShuffleOff;
  LGFX_Sprite _sprTime;
  int64_t _lastTimestampSec = INT64_MAX;
  int _lastTempoBpm = -1;
  bool _lastTimestampVisible = false;
  void drawTimestamp(int64_t sec, bool toFrameBuffer, bool visible);

 public:
  VisualWindow() : _sprShuffleOn(&lcd), _sprShuffleOff(&lcd), _sprTime(&lcd) {
  }
  void init();
  void draw();
  void update();
  void updateLabels();
  void drawTimestamp(int64_t sec);
  void drawTimestamp(int64_t sec, bool visible);
  void show();
  void close();
  void eventHandler(event ev);
  boolean visible;
};
extern VisualWindow visualWindow;

// ファイルブラウザ画面クラス
class BrowserWindow {
 private:
  LGFX_Sprite _sprFooter;
  // 言語別ヘッダー用スプライト
  LGFX_Sprite _sprHeaderJP;
  LGFX_Sprite _sprHeaderEN;
  LGFX_Sprite _sprCurrentDir;
  Node* _lastCurrentDirNode = nullptr;
  Node* _browseDirNode = nullptr;
  Node* _selectedNode = nullptr;

  void initHeaders();     // ヘッダースプライトの初期化
  void drawCurrentDir();  // カレントディレクトリを描画
  void drawFooterStatic();
  void drawFooterState(bool toFrameBuffer);
  bool isFooterUpEnabled() const;

 public:
  void init();
  void draw();
  void drawFooter(bool toFrameBuffer);
  int getItemCount() const;       // 表示中ディレクトリの項目数を返す
  void selectItem(int index);     // 指定した表示インデックスを選択する
  void moveSelection(int delta);  // 選択項目を前後に移動する
  void openDirectory(Node* dirNode,
                     Node* selectedNode = nullptr);  // 指定ディレクトリをブラウズする
  bool openParentDirectory();                        // 上位ディレクトリへ移動する
  void onCurrentNodeChanged(Node* prevNode,
                            Node* currentNode);  // 再生中項目の表示を更新する

  void close();
  void eventHandler(event ev);
  void show();
  boolean visible;
};
extern BrowserWindow browserWindow;

#endif
