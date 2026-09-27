#include "./disp.h"

#include <cstdio>
#include <cstring>

#include "pics.h"
#include "png_renderer.h"

#ifndef ND_DEBUG_VISUAL_UPDATE
#define ND_DEBUG_VISUAL_UPDATE 0
#endif

class ScopedEncoderDisable {
 public:
  ScopedEncoderDisable() : _restore(input.isEncoderEnabled()) {
    input.setEncoderEnabled(false);
  }
  ~ScopedEncoderDisable() {
    if (_restore) {
      input.setEncoderEnabled(true);
    }
  }

 private:
  bool _restore;
};

LGFX::LGFX(void) {
  {                                     // バス制御の設定を行います。
    auto cfg = _bus_instance.config();  // バス設定用の構造体を取得します。

    // SPIバスの設定
    cfg.spi_host = SPI2_HOST;  // 使用するSPIを選択  ESP32-S2,C3 : SPI2_HOST or
                               // SPI3_HOST / ESP32 : VSPI_HOST or HSPI_HOST
    // ※ ESP-IDFバージョンアップに伴い、VSPI_HOST ,
    // HSPI_HOSTの記述は非推奨になるため、エラーが出る場合は代わりにSPI2_HOST ,
    // SPI3_HOSTを使用してください。
    cfg.spi_mode = 3;                   // SPI通信モードを設定 (0 ~ 3)
    cfg.freq_write = 80000000;          // 送信時のSPIクロック (最大80MHz,
                                        // 80MHzを整数で割った値に丸められます)
    cfg.freq_read = 40000000;           // 受信時のSPIクロック
    cfg.spi_3wire = true;               // 受信をMOSIピンで行う場合はtrueを設定
    cfg.use_lock = true;                // トランザクションロックを使用する場合はtrueを設定
    cfg.dma_channel = SPI_DMA_CH_AUTO;  // 使用するDMAチャンネルを設定 (0=DMA不使用 / 1=1ch /
                                        // 2=ch / SPI_DMA_CH_AUTO=自動設定)
    // ※
    // ESP-IDFバージョンアップに伴い、DMAチャンネルはSPI_DMA_CH_AUTO(自動設定)が推奨になりました。1ch,2chの指定は非推奨になります。
    cfg.pin_sclk = LCD_CLK;                  // SPIのSCLKピン番号を設定
    cfg.pin_mosi = LCD_MOSI;                 // SPIのMOSIピン番号を設定
    cfg.pin_miso = -1;                       // SPIのMISOピン番号を設定 (-1 = disable)
    cfg.pin_dc = LCD_DC;                     // SPIのD/Cピン番号を設定  (-1 = disable)
    _bus_instance.config(cfg);               // 設定値をバスに反映します。
    _panel_instance.setBus(&_bus_instance);  // バスをパネルにセットします。
  }

  {                                       // 表示パネル制御の設定を行います。
    auto cfg = _panel_instance.config();  // 表示パネル設定用の構造体を取得します。

    cfg.pin_cs = -1;        // CSが接続されているピン番号   (-1 = disable)
    cfg.pin_rst = LCD_RST;  // LCD_RST;  // RSTが接続されているピン番号  (-1 = disable)
    cfg.pin_busy = -1;      // BUSYが接続されているピン番号 (-1 = disable)

    cfg.panel_width = 170;     // 実際に表示可能な幅
    cfg.panel_height = 320;    // 実際に表示可能な高さ
    cfg.offset_x = 35;         // パネルのX方向オフセット量
    cfg.offset_y = 0;          // パネルのY方向オフセット量
    cfg.offset_rotation = 2;   // 回転方向の値のオフセット 0~7 (4~7は上下反転)
    cfg.dummy_read_pixel = 8;  // ピクセル読出し前のダミーリードのビット数
    cfg.dummy_read_bits = 1;   // ピクセル以外のデータ読出し前のダミーリードのビット数
    cfg.readable = true;       // データ読出しが可能な場合 trueに設定
    cfg.invert = true;         // パネルの明暗が反転してしまう場合 trueに設定
    cfg.rgb_order = false;     // パネルの赤と青が入れ替わってしまう場合 trueに設定
    cfg.dlen_16bit = false;    // 16bitパラレルやSPIでデータ長を16bit単位で送信するパネルの場合
                               // trueに設定
    cfg.bus_shared = false;    // SDカードとバスを共有している場合
                               // trueに設定(drawJpgFile等でバス制御を行います)

    _panel_instance.config(cfg);
  }
  setPanel(&_panel_instance);  // 使用するパネルをセットします。
}

LGFX lcd;

static LGFX_Sprite frameBuffer(&lcd);
static LGFX_Sprite keyboardBuffer(&lcd);     // キーボード用
static LGFX_Sprite keyboardBufferSub(&lcd);  // キーボード用
static LGFX_Sprite panMarkerCenter(&lcd);
static LGFX_Sprite panMarkerLeft(&lcd);
static LGFX_Sprite panMarkerRight(&lcd);
static LGFX_Sprite panMarkerMute(&lcd);

static bool kVisualRotate180 = false;
static constexpr u8_t kLevelSpriteCount = 16;  // level 0 .. 15
static constexpr u16_t kLevelSpriteWidth = levelsWidth;
static constexpr u16_t kLevelSpriteHeight = 17;
static constexpr u8_t kNumberSpriteCount = 11;  // "--", 9 .. 0
static constexpr u16_t kNumberSpriteWidth = numbersWidth;
static constexpr u16_t kNumberSpriteHeight = 8;
static constexpr u8_t kLabelSpriteCount = 3;  // PCM8, MDX, VGM

// 負の秒数を -M:SS として表示できるようにする。
static void formatTimestamp(char* buffer, size_t size, int64_t sec) {
  const bool isNegative = sec < 0;
  const uint64_t absSec =
      isNegative ? static_cast<uint64_t>(-(sec + 1)) + 1 : static_cast<uint64_t>(sec);
  snprintf(buffer, size, "%s%llu:%02u", isNegative ? "-" : "",
           static_cast<unsigned long long>(absSec / 60), static_cast<unsigned int>(absSec % 60));
}
static constexpr u16_t kLabelSpriteWidth = labelsWidth;
static constexpr u16_t kLabelSpriteHeight = 41;
static constexpr u16_t kLabelPcm8X = 155;
static constexpr u16_t kLabelPcm8Y = 135;
static constexpr u16_t kLabelMdxX = 155;
static constexpr u16_t kLabelMdxY = 179;
static constexpr u16_t kLabelVgmX = 155;
static constexpr u16_t kLabelVgmY = 223;
static constexpr u16_t kLevelWaterfallAccelQ8 = 16;         // 落下加速度
static constexpr u16_t kLevelWaterfallInitialSpeedQ8 = 32;  // 落下開始初速
static constexpr u8_t kPeakHoldDelayFrames = 10;            // ピークホールドフレーム数
static constexpr u8_t kPeakLineX0 = 29;
static constexpr u8_t kPeakLineYTop = 2;
static constexpr u8_t kPeakLineYBottom = 14;
static constexpr u16_t kNoteDrawX = 115;        // キー表示 X
static constexpr u16_t kNoteDrawBottomY = 263;  // キー表示 Y 下端
static u16_t levelSprite[kLevelSpriteCount]
                        [kLevelSpriteWidth * kLevelSpriteHeight];      // レベルメータの各スプライト
static u16_t levelWorkBuffer[kLevelSpriteWidth * kLevelSpriteHeight];  // レベルメータ作業用バッファ
static u16_t numberSprite[kNumberSpriteCount]
                         [kNumberSpriteWidth * kNumberSpriteHeight];  // 数字表示の各スプライト
static u16_t numberWorkBuffer[kNumberSpriteWidth * (kNumberSpriteHeight * 2)];  // 2文字表示作業用
static u16_t labelSprite[kLabelSpriteCount]
                        [kLabelSpriteWidth * kLabelSpriteHeight];  // ラベル表示の各スプライト
static u16_t visualRowBuffer[LCD_W];                               // 低頻度画像の180度反転用
static constexpr int kLevelDrawX = 67;                             // レベルメータX
static constexpr int kLevelDrawBottomY = 262;                      // レベルメータY下端
#if ND_DEBUG_VISUAL_UPDATE
static u32_t visualUpdateCount = 0;
static u32_t visualUpdateSkipCount = 0;
static uint64_t visualUpdateTimeUs = 0;
#endif
static int8_t lastTrackPan[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
static int8_t lastTrackLevel[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
static int8_t lastTrackPeak[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
static int16_t lastTrackNote[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
static u8_t heldTrackNote[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static u16_t trackLevelDisplayQ8[16] = {0};
static u16_t trackLevelFallSpeedQ8[16] = {0};
static u16_t trackPeakDisplayQ8[16] = {0};
static u16_t trackPeakFallSpeedQ8[16] = {0};
static u8_t trackPeakHoldFrames[16] = {0};
static u8_t keyboardDrawChannelMask = 0;

static int visualX(int x, int w) {
  return kVisualRotate180 ? LCD_W - x - w : x;
}

static int visualY(int y, int h) {
  return kVisualRotate180 ? LCD_H - y - h : y;
}

static void copyVisualImage(u16_t* dst, const u16_t* src, int w, int h) {
  if (!kVisualRotate180) {
    memcpy(dst, src, sizeof(u16_t) * w * h);
    return;
  }

  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      dst[y * w + x] = src[(h - 1 - y) * w + (w - 1 - x)];
    }
  }
}

static void pushVisualImage(LGFX_Sprite& target, int x, int y, int w, int h, const u16_t* src) {
  if (!kVisualRotate180) {
    target.pushImage(x, y, w, h, src);
    return;
  }

  const int dstX = visualX(x, w);
  const int dstY = visualY(y, h);
  for (int row = 0; row < h; row++) {
    for (int col = 0; col < w; col++) {
      visualRowBuffer[col] = src[(h - 1 - row) * w + (w - 1 - col)];
    }
    target.pushImage(dstX, dstY + row, w, 1, visualRowBuffer);
  }
}

static void pushPreparedVisualImage(LGFX_Sprite& target, int x, int y, int w, int h,
                                    const u16_t* src) {
  if (!kVisualRotate180) {
    target.pushImage(x, y, w, h, src);
    return;
  }

  for (int row = 0; row < h; row++) {
    for (int col = 0; col < w; col++) {
      visualRowBuffer[col] = src[(h - 1 - row) * w + (w - 1 - col)];
    }
    target.pushImage(x, y + row, w, 1, visualRowBuffer);
  }
}

static void pushVisualSpriteToFrameBuffer(LGFX_Sprite& sprite, int x, int y) {
  sprite.pushSprite(&frameBuffer, visualX(x, sprite.width()), visualY(y, sprite.height()));
}

static void pushVisualSpriteToLcd(LGFX_Sprite& sprite, int x, int y) {
  sprite.pushSprite(visualX(x, sprite.width()), visualY(y, sprite.height()));
}

static void pushVisualBufferToLcd(int x, int y, int w, int h, const u16_t* data) {
  lcd.pushImage(visualX(x, w), visualY(y, h), w, h, data);
}

static void fillVisualRect(LGFX_Sprite& sprite, int x, int y, int w, int h) {
  if (kVisualRotate180) {
    sprite.fillRect(sprite.width() - x - w, sprite.height() - y - h, w, h);
  } else {
    sprite.fillRect(x, y, w, h);
  }
}

static void createVisualShuffleIconSprite(LGFX_Sprite& target, u16_t color) {
  static constexpr int kShuffleIconSize = 18;

  LGFX_Sprite source(&lcd);
  source.setPsram(false);
  source.createSprite(kShuffleIconSize, kShuffleIconSize);
  if (source.width() == 0) {
    return;
  }

  target.deleteSprite();
  target.setPsram(false);
  target.createSprite(kShuffleIconSize, kShuffleIconSize);
  if (target.width() == 0) {
    source.deleteSprite();
    return;
  }

  source.fillSprite(TFT_BLACK);
  disp.render.setDrawer(source);
  disp.render.loadFont(fontMain, sizeof(fontMain));
  disp.render.setFontSize(15);
  disp.render.setAlignment(Align::TopCenter);
  disp.render.setFontColor(color, TFT_BLACK);
  disp.render.setCursor(kShuffleIconSize / 2, 1);
  disp.render.printf("丂");
  disp.render.unloadFont();

  target.fillSprite(TFT_BLACK);
  source.setPivot(kShuffleIconSize / 2.0f, kShuffleIconSize / 2.0f);
  const float angle = kVisualRotate180 ? 90.0f : -90.0f;
  source.pushRotateZoom(&target, kShuffleIconSize / 2.0f, kShuffleIconSize / 2.0f, angle, 1.0f,
                        1.0f, TFT_BLACK);
  source.deleteSprite();
}

// パンマーカー切り出し
static void cutPanMarkerSprite(LGFX_Sprite& sprite, int markerIndex) {
  const int markerHeight = panmarkersHeight / 4;
  sprite.setPsram(false);
  sprite.createSprite(panmarkersWidth, markerHeight);

  if (kVisualRotate180) {
    for (int y = 0; y < markerHeight; y++) {
      const int srcY = markerIndex * markerHeight + (markerHeight - 1 - y);
      for (int x = 0; x < panmarkersWidth; x++) {
        visualRowBuffer[x] = panmarkers[srcY * panmarkersWidth + (panmarkersWidth - 1 - x)];
      }
      sprite.pushImage(0, y, panmarkersWidth, 1, visualRowBuffer);
    }
  } else {
    for (int y = 0; y < markerHeight; y++) {
      const int srcY = markerIndex * markerHeight + y;
      const u16_t* src = &panmarkers[srcY * panmarkersWidth];
      sprite.pushImage(0, y, panmarkersWidth, 1, src);
    }
  }
}

// レベルメータ画像切り出し
static void cutLevelSprite(u8_t levelIndex) {
  if (levelIndex >= kLevelSpriteCount) {
    return;
  }

  const u16_t srcY = (u16_t)(levelIndex * kLevelSpriteHeight);
  u16_t* dst = levelSprite[levelIndex];

  if (kVisualRotate180) {
    for (u16_t y = 0; y < kLevelSpriteHeight; y++) {
      for (u16_t x = 0; x < kLevelSpriteWidth; x++) {
        dst[y * kLevelSpriteWidth + x] =
            levels[(srcY + (kLevelSpriteHeight - 1 - y)) * levelsWidth +
                   (kLevelSpriteWidth - 1 - x)];
      }
    }
  } else {
    for (u16_t y = 0; y < kLevelSpriteHeight; y++) {
      const u16_t* src = &levels[(srcY + y) * levelsWidth];
      memcpy(&dst[y * kLevelSpriteWidth], src, sizeof(u16_t) * kLevelSpriteWidth);
    }
  }
}

// 数字画像切り出し
static void cutNumberSprite(u8_t glyphIndex) {
  if (glyphIndex >= kNumberSpriteCount) {
    return;
  }

  const u16_t srcY = (u16_t)(glyphIndex * kNumberSpriteHeight);
  u16_t* dst = numberSprite[glyphIndex];

  if (kVisualRotate180) {
    for (u16_t y = 0; y < kNumberSpriteHeight; y++) {
      for (u16_t x = 0; x < kNumberSpriteWidth; x++) {
        dst[y * kNumberSpriteWidth + x] =
            numbers[(srcY + (kNumberSpriteHeight - 1 - y)) * numbersWidth +
                    (kNumberSpriteWidth - 1 - x)];
      }
    }
  } else {
    for (u16_t y = 0; y < kNumberSpriteHeight; y++) {
      const u16_t* src = &numbers[(srcY + y) * numbersWidth];
      memcpy(&dst[y * kNumberSpriteWidth], src, sizeof(u16_t) * kNumberSpriteWidth);
    }
  }
}

// ラベル画像切り出し
static void cutLabelSprite(u8_t labelIndex) {
  if (labelIndex >= kLabelSpriteCount) {
    return;
  }

  const u16_t* src = &labels[labelIndex * kLabelSpriteWidth * kLabelSpriteHeight];
  copyVisualImage(labelSprite[labelIndex], src, kLabelSpriteWidth, kLabelSpriteHeight);
}

// ラベルの描画
static void drawLabelClip(LGFX_Sprite& target, u8_t labelIndex, int x, int y) {
  if (labelIndex >= kLabelSpriteCount) {
    return;
  }

  target.pushImage(visualX(x, kLabelSpriteWidth), visualY(y, kLabelSpriteHeight), kLabelSpriteWidth,
                   kLabelSpriteHeight, labelSprite[labelIndex]);
}

static u8_t getNumberGlyphIndex(char c) {
  if (c == '-') {
    return 0;
  }
  if (c >= '0' && c <= '9') {
    return (u8_t)(10 - (c - '0'));
  }
  return 0;
}

static u8_t noteInfoToDisplayNoteNo(t_device device, const NoteInfo& ni) {
  if (ni.octave < 0 || ni.octave > 8 || ni.note < 0 || ni.note > 11) {
    return 0xff;
  }

  int noteNo = -1;
  switch (device) {
    case YM2151:
      noteNo = ni.octave * 12 + ni.note - 3;
      break;
    case OKIM6258_KEY:
      noteNo = ni.octave * 12 + ni.note - 9;
      break;
    default:
      return 0xff;
  }

  if (noteNo < 0 || noteNo > 96) {
    return 0xff;
  }
  return (u8_t)noteNo;
}

static u8_t updateTrackLevelWaterfall(u8_t trackNo, u8_t targetLevel) {
  if (trackNo >= 16) {
    return 0;
  }
  if (targetLevel >= kLevelSpriteCount) {
    targetLevel = kLevelSpriteCount - 1;
  }

  const u16_t targetQ8 = (u16_t)(targetLevel << 8);
  u16_t& displayQ8 = trackLevelDisplayQ8[trackNo];
  u16_t& fallSpeedQ8 = trackLevelFallSpeedQ8[trackNo];

  if (displayQ8 <= targetQ8) {
    displayQ8 = targetQ8;
    fallSpeedQ8 = 0;
  } else {
    if (fallSpeedQ8 == 0) {
      fallSpeedQ8 = kLevelWaterfallInitialSpeedQ8;
    }
    fallSpeedQ8 = (u16_t)(fallSpeedQ8 + kLevelWaterfallAccelQ8);
    if (displayQ8 > fallSpeedQ8) {
      displayQ8 = (u16_t)(displayQ8 - fallSpeedQ8);
    } else {
      displayQ8 = 0;
    }
    if (displayQ8 < targetQ8) {
      displayQ8 = targetQ8;
      fallSpeedQ8 = 0;
    }
  }

  return (u8_t)(displayQ8 >> 8);
}

static u8_t updateTrackPeakHold(u8_t trackNo) {
  if (trackNo >= 16) {
    return 0;
  }

  const u16_t currentQ8 = trackLevelDisplayQ8[trackNo];
  u16_t& peakQ8 = trackPeakDisplayQ8[trackNo];
  u16_t& fallSpeedQ8 = trackPeakFallSpeedQ8[trackNo];
  u8_t& holdFrames = trackPeakHoldFrames[trackNo];

  if (peakQ8 <= currentQ8) {
    peakQ8 = currentQ8;
    fallSpeedQ8 = 0;
    holdFrames = kPeakHoldDelayFrames;
  } else if (holdFrames > 0) {
    holdFrames--;
  } else {
    if (fallSpeedQ8 == 0) {
      fallSpeedQ8 = kLevelWaterfallInitialSpeedQ8;
    }
    fallSpeedQ8 = (u16_t)(fallSpeedQ8 + kLevelWaterfallAccelQ8);
    if (peakQ8 > fallSpeedQ8) {
      peakQ8 = (u16_t)(peakQ8 - fallSpeedQ8);
    } else {
      peakQ8 = 0;
    }
    if (peakQ8 < currentQ8) {
      peakQ8 = currentQ8;
      fallSpeedQ8 = 0;
    }
  }

  return (u8_t)(peakQ8 >> 8);
}

static TimerHandle_t hDispTimer;
static TaskHandle_t hDispUpdateTask;

static Label lblTitle =
    Label(0, 28, LCD_W, C_ACCENT_LIGHT, C_BASEBG, 21, SCROLL_SPEED_TITLE, Align::TopCenter);
static Label lblGame =
    Label(0, 53, LCD_W, C_LIGHTGRAY, C_BASEBG, 16, SCROLL_SPEED_GAME, Align::TopCenter);
static Label lblAuthor =
    Label(28, 232, LCD_W - 28, C_GRAY, C_BASEBG, 17, SCROLL_SPEED_AUTHOR, Align::TopLeft);
static Label lblSystem =
    Label(28, 210, LCD_W - 28, C_GRAY, C_BASEBG, 17, SCROLL_SPEED_AUTHOR, Align::TopLeft);
static RotatedLabel lblSongTitle =
    RotatedLabel(133, 0, 268, TFT_WHITE, C_BASEBG, 17, SCROLL_SPEED_TITLE, Align::TopLeft);

static void preparePlayerLabels(const tDispData& data) {
  if (ndConfig.get(CFG_LANG) == LANG_JA) {
    lblTitle.prepareCaption(data.trackJp);
    lblGame.prepareCaption(data.gameJp);
    lblAuthor.prepareCaption(data.authorJp);
    lblSystem.prepareCaption(data.systemJp);
  } else {
    lblTitle.prepareCaption(data.trackEn);
    lblGame.prepareCaption(data.gameEn);
    lblAuthor.prepareCaption(data.authorEn);
    lblSystem.prepareCaption(data.systemEn);
  }
}

static void drawPreparedPlayerLabels() {
  lblTitle.drawPrepared();
  lblGame.drawPrepared();
  lblAuthor.drawPrepared();
  lblSystem.drawPrepared();
}

static String getVisualSongTitle(const tDispData& data) {
  String songTitle;
  if (ndConfig.get(CFG_LANG) == LANG_JA) {
    songTitle = data.trackJp;
    if ((ND::fileFormat == FileFormat::VGM || ND::fileFormat == FileFormat::VGZ) &&
        data.gameJp != "") {
      songTitle += " / " + data.gameJp;
    }
  } else {
    songTitle = data.trackEn;
    if ((ND::fileFormat == FileFormat::VGM || ND::fileFormat == FileFormat::VGZ) &&
        data.gameEn != "") {
      songTitle += " / " + data.gameEn;
    }
  }
  return songTitle;
}

static void prepareVisualSongTitle(const tDispData& data) {
  lblSongTitle.prepareCaption(getVisualSongTitle(data));
}

static SemaphoreHandle_t spFrameBuffer;  // 描画用セマフォ

//---------------------------------------------------------------------------
// 長い文字を ... 省略する文字描画
void printWithEllipsis(OpenFontRender& render, const char* text, int x, int y, int maxWidth,
                       int dotsWidth) {
  if (!text || maxWidth <= 0) return;

  static constexpr char dots[] = "...";
  if (dotsWidth < 0) dotsWidth = render.getTextWidth(dots);

  if (dotsWidth > maxWidth) return;

  u16_t fitLen = render.getUtf8BytesForWidth(text, maxWidth, dotsWidth);
  if (text[fitLen] == '\0') {
    render.drawString(text, x, y, render.getFontColor(), render.getBackgroundColor(),
                      render.getLayout());
    return;
  }

  if (fitLen == 0) {
    render.drawString(dots, x, y, render.getFontColor(), render.getBackgroundColor(),
                      render.getLayout());
    return;
  }

  char buf[256];
  if (fitLen > sizeof(buf) - 4) fitLen = sizeof(buf) - 4;
  memcpy(buf, text, fitLen);
  memcpy(buf + fitLen, dots, 4);
  render.drawString(buf, x, y, render.getFontColor(), render.getBackgroundColor(),
                    render.getLayout());
}

//---------------------------------------------------------------------------
// Panel class

// スクロールバー描画
void Panel::drawScrollbar() {
  // スクロールバー背景描画
  _spriteScrollBarBG.pushSprite(&_spriteScrollBar, 0, 0);
  if (_innerHeight > height) {
    u16_t barHeight = _maxBarHeight * height / _innerHeight;
    u16_t barTop = (_maxBarHeight * scrollTop) / _innerHeight;
    _spriteScrollBar.fillRoundRect(PADDING, INDICATORHEIGHT + barTop, SCROLLBARWIDTH - PADDING * 2,
                                   barHeight, _borderRadius, C_MID);
  }
}

void Panel::redrawItem(LGFX_Sprite& targetBuffer, int index) {
  if (!_renderer || index < 0 || index >= _itemCount) return;

  int firstIndex = scrollTop / _itemHeight;
  int lastIndex = (scrollTop + height - 1) / _itemHeight;

  if (firstIndex < 0) firstIndex = 0;
  if (lastIndex >= _itemCount) lastIndex = _itemCount - 1;
  if (index < firstIndex || index > lastIndex) return;

  int drawY = y + index * _itemHeight - scrollTop;
  bool selected = (index == currentIndex);
  _renderer->onDrawItem(targetBuffer, index, x, drawY, width - SCROLLBARWIDTH, selected);
}

void Panel::redrawScrollEdgeItems(LGFX_Sprite& targetBuffer, int scrollDelta) {
  if (scrollDelta == 0 || _itemCount <= 0) return;

  int firstIndex = scrollTop / _itemHeight;
  int lastIndex = (scrollTop + height - 1) / _itemHeight;
  int redrawCount = (abs(scrollDelta) + _itemHeight - 1) / _itemHeight + 1;

  if (firstIndex < 0) firstIndex = 0;
  if (lastIndex >= _itemCount) lastIndex = _itemCount - 1;

  if (scrollDelta > 0) {
    for (int i = 0; i < redrawCount; i++) {
      redrawItem(targetBuffer, lastIndex - i);
    }
    return;
  }

  for (int i = 0; i < redrawCount; i++) {
    redrawItem(targetBuffer, firstIndex + i);
  }
}

void Panel::redrawVisibleItems(LGFX_Sprite& targetBuffer) {
  if (!_renderer) return;

  int firstIndex = scrollTop / _itemHeight;
  int lastIndex = (scrollTop + height - 1) / _itemHeight;

  if (firstIndex < 0) firstIndex = 0;
  if (lastIndex >= _itemCount) lastIndex = _itemCount - 1;

  for (int index = firstIndex; index <= lastIndex; index++) {
    int drawY = y + index * _itemHeight - scrollTop;
    bool selected = (index == currentIndex);
    _renderer->onDrawItem(targetBuffer, index, x, drawY, width - SCROLLBARWIDTH, selected);
  }
}

// フレームバッファに内容を描画
void Panel::update(LGFX_Sprite& targetBuffer) {
  int scrollDelta = scrollTop - _prevScrollTop;
  bool reuseScrolledContent = !_needsFullRedraw && _itemCount == _prevItemCount &&
                              scrollDelta != 0 && abs(scrollDelta) < height;
  bool fullRedraw = _needsFullRedraw || _itemCount != _prevItemCount ||
                    (scrollTop != _prevScrollTop && !reuseScrolledContent);
  int itemWidth = width - SCROLLBARWIDTH;

  targetBuffer.setClipRect(x, y, itemWidth, height);

  if (fullRedraw) {
    targetBuffer.fillRect(x, y, itemWidth, height, TFT_WHITE);
  }

  if (_renderer) {
    if (fullRedraw) {
      redrawVisibleItems(targetBuffer);
    } else if (reuseScrolledContent) {
      if (scrollDelta > 0) {
        targetBuffer.copyRect(x, y, itemWidth, height - scrollDelta, x, y + scrollDelta);
        targetBuffer.fillRect(x, y + height - scrollDelta, itemWidth, scrollDelta, TFT_WHITE);
      } else {
        int shift = -scrollDelta;
        targetBuffer.copyRect(x, y + shift, itemWidth, height - shift, x, y);
        targetBuffer.fillRect(x, y, itemWidth, shift, TFT_WHITE);
      }

      redrawScrollEdgeItems(targetBuffer, scrollDelta);
      if (_prevCurrentIndex != currentIndex) {
        redrawItem(targetBuffer, _prevCurrentIndex);
        redrawItem(targetBuffer, currentIndex);
      }
    } else if (currentIndex != _prevCurrentIndex) {
      redrawItem(targetBuffer, _prevCurrentIndex);
      redrawItem(targetBuffer, currentIndex);
    }
  }

  targetBuffer.clearClipRect();

  // スクロールバーをバッファに描画
  drawScrollbar();

  // スクロールバースプライト転送
  _spriteScrollBar.pushSprite(&targetBuffer, x + width - SCROLLBARWIDTH, y);

  _prevCurrentIndex = currentIndex;
  _prevScrollTop = scrollTop;
  _prevItemCount = _itemCount;
  _needsFullRedraw = false;
}

// アイテムカウントを更新
void Panel::setItemCount(int newItemCount) {
  if (_itemCount != newItemCount) {
    _needsFullRedraw = true;
  }
  _itemCount = newItemCount;
  _innerHeight = _itemCount * _itemHeight;
}

void Panel::invalidate() {
  _needsFullRedraw = true;
}

void Panel::resetScroll() {
  scrollTop = 0;
  _prevScrollTop = 0;
  _needsFullRedraw = true;
}

// アイテムを見える場所に持ってくる
void Panel::ensureVisible() {
  int itemTop = currentIndex * _itemHeight;
  int itemBottom = itemTop + _itemHeight;

  if (itemTop < scrollTop) {
    scrollTop = itemTop;
  } else if (itemBottom > scrollTop + height) {
    scrollTop = itemBottom - height;
  }

  int maxScroll = _innerHeight - height;
  if (maxScroll < 0) maxScroll = 0;
  if (scrollTop < 0) scrollTop = 0;
  if (scrollTop > maxScroll) scrollTop = maxScroll;

  drawScrollbar();
}

// クリック処理呼び出し
void Panel::clickCurrentItem() {
  if (_renderer == nullptr) return;
  _renderer->onClick(currentIndex);
}

//---------------------------------------------------------------------------
// Scrolling label class
Label::Label(const int16_t x, const int16_t y, const int16_t w, const u16_t fontColor,
             const u16_t bgColor, const u16_t fontSize, const float scrollSpeed,
             const Align textAlign) {
  _x = x;
  _y = y;
  _labelWidth = w;
  _fontColor = fontColor;
  _bgColor = bgColor;
  _fontSize = fontSize;
  _scrollSpeed = scrollSpeed;
  _textAlign = textAlign;
}

void Label::prepareCaption(const String& newCaption) {
  if (_caption != newCaption || _sprite.width() == 0) {
    _enabled = false;
    _caption = newCaption;

    OpenFontRender ofr;

    ofr.setUseRenderTask(false);

    ofr.setDrawer(_sprite);
    ofr.loadFont(fontMain, sizeof(fontMain));

    ofr.setFontSize(_fontSize);
    ofr.setFontColor(_fontColor, _bgColor);
    _textWidth = ofr.getTextWidth(_caption.c_str());
    _devWidth = ofr.getTextWidth(TITLE_DEVIDER) + ofr.getTextWidth("/");
    _sprite.deleteSprite();
    _sprite.setPsram(true);

    String renderCaption = _caption;
    if (_textWidth > _labelWidth) {
      renderCaption += TITLE_DEVIDER;
      _isScrolling = true;
      _sprite.createSprite(_textWidth + _devWidth, _fontSize);

      ofr.setAlignment(Align::TopLeft);
      ofr.setCursor(0, 0);
    } else {
      _isScrolling = false;
      _sprite.createSprite(_labelWidth, _fontSize);
      ofr.setAlignment(_textAlign);
      if (_textAlign == Align::TopCenter) {
        ofr.setCursor(_labelWidth / 2, 0);
      } else {
        ofr.setCursor(0, 0);
      }
    }

    _sprite.fillSprite(_bgColor);
    ofr.printf(renderCaption.c_str());
    ofr.unloadFont();
  }
}

void Label::drawPrepared() {
  if (_sprite.width() == 0) return;
  _sprite.pushSprite(&frameBuffer, _x, _y);
  _n = 0;
  _lastDrawOffset = -1;
  _scrollCount = 0;
  _startTick = millis();
  _enabled = true;
}

void Label::setCaption(const String& newCaption) {
  prepareCaption(newCaption);
  drawPrepared();
}

void Label::update() {
  if (!_enabled || !_isScrolling) return;

  // スクロール回数チェック
  const int32_t scrollLimit = ndConfig.get(CFG_SCROLL);
  if (scrollLimit != SCROLL_INFINITE && _scrollCount >= scrollLimit) return;

  u32_t now = millis();
  if (now - _startTick < SCROLL_DELAY) return;

  const int32_t loopWidth = _textWidth + _devWidth;
  const int32_t drawOffset = (int32_t)_n;
  if (drawOffset != _lastDrawOffset) {
    lcd.setClipRect(_x, _y, _labelWidth, _sprite.height());
    _sprite.pushSprite(&lcd, _x - drawOffset, _y);

    // 先頭側が見えるラップ境界のときだけ2枚目を描画する
    if (drawOffset + (int32_t)_labelWidth > loopWidth) {
      _sprite.pushSprite(&lcd, _x + loopWidth - drawOffset, _y);
    }
    _lastDrawOffset = drawOffset;
  }

  if (_n < loopWidth - _scrollSpeed) {
    _n += _scrollSpeed;
  } else {
    _n = 0;
    _lastDrawOffset = -1;
    _startTick = now;
    _scrollCount++;
  }
}

void Label::setEnabled(bool state) {
  _enabled = state;
}

//---------------------------------------------------------------------------
// Rotated scrolling label class
RotatedLabel::RotatedLabel(const int16_t x, const int16_t y, const int16_t h, const u16_t fontColor,
                           const u16_t bgColor, const u16_t fontSize, const float scrollSpeed,
                           const Align textAlign) {
  _x = x;
  _y = y;
  _labelHeight = h;
  _fontColor = fontColor;
  _bgColor = bgColor;
  _fontSize = fontSize;
  _scrollSpeed = scrollSpeed;
  _textAlign = textAlign;
}

void RotatedLabel::prepareCaption(const String& newCaption) {
  if (_caption != newCaption || _sprite.width() == 0) {
    _enabled = false;
    _caption = newCaption;

    OpenFontRender ofr;
    LGFX_Sprite source(&lcd);
    String renderCaption = _caption;

    ofr.setUseRenderTask(false);
    ofr.loadFont(fontMain, sizeof(fontMain));
    ofr.setFontSize(_fontSize);
    _textWidth = ofr.getTextWidth(_caption.c_str());
    _devWidth = ofr.getTextWidth(TITLE_DEVIDER) + ofr.getTextWidth("/");

    _sprite.deleteSprite();
    _sprite.setPsram(true);
    source.setPsram(true);

    if (_textWidth > _labelHeight) {
      renderCaption += TITLE_DEVIDER;
      _isScrolling = true;
      source.createSprite(_textWidth + _devWidth, _fontSize);
      _sprite.createSprite(_fontSize, _textWidth + _devWidth);
    } else {
      _isScrolling = false;
      source.createSprite(_textWidth > 0 ? _textWidth : 1, _fontSize);
      _sprite.createSprite(_fontSize, _labelHeight > 0 ? _labelHeight : 1);
    }

    source.fillSprite(_bgColor);
    _sprite.fillSprite(_bgColor);

    ofr.setDrawer(source);
    ofr.setFontColor(_fontColor, _bgColor);
    ofr.setAlignment(Align::TopLeft);
    ofr.setCursor(0, 0);
    ofr.printf(renderCaption.c_str());
    ofr.unloadFont();

    const int32_t rotatedHeight = source.width();
    int32_t dstY = 0;
    if (!_isScrolling && _labelHeight > (u32_t)rotatedHeight) {
      dstY = _labelHeight - rotatedHeight;
    }

    for (int32_t srcY = 0; srcY < source.height(); srcY++) {
      for (int32_t srcX = 0; srcX < source.width(); srcX++) {
        const int32_t dstX = srcY;
        const int32_t dstYPos = dstY + source.width() - 1 - srcX;
        if (dstX >= 0 && dstX < _sprite.width() && dstYPos >= 0 && dstYPos < _sprite.height()) {
          const u16_t color = source.readPixel(srcX, srcY);
          if (kVisualRotate180) {
            _sprite.drawPixel(_sprite.width() - 1 - dstX, _sprite.height() - 1 - dstYPos, color);
          } else {
            _sprite.drawPixel(dstX, dstYPos, color);
          }
        }
      }
    }

    source.deleteSprite();
  }
}

void RotatedLabel::drawPrepared() {
  if (_sprite.width() == 0) return;
  const int32_t initialY = kVisualRotate180
                               ? visualY(_y, _labelHeight)
                               : (int32_t)_y + (int32_t)_labelHeight - _sprite.height();
  const int32_t drawX = visualX(_x, _sprite.width());
  const int32_t clipY = visualY(_y, _labelHeight);
  frameBuffer.setClipRect(drawX, clipY, _sprite.width(), _labelHeight);
  _sprite.pushSprite(&frameBuffer, drawX, initialY);
  frameBuffer.clearClipRect();
  _n = 0;
  _lastDrawOffset = -1;
  _scrollCount = 0;
  _startTick = millis();
  _enabled = true;
}

void RotatedLabel::setCaption(const String& newCaption) {
  prepareCaption(newCaption);
  drawPrepared();
}

void RotatedLabel::update() {
  if (!_enabled || !_isScrolling) return;

  const int32_t scrollLimit = ndConfig.get(CFG_SCROLL);
  if (scrollLimit != SCROLL_INFINITE && _scrollCount >= scrollLimit) return;

  u32_t now = millis();
  if (now - _startTick < SCROLL_DELAY) return;

  const int32_t loopHeight = _sprite.height();
  const int32_t drawOffset = (int32_t)_n;
  const int32_t baseY = kVisualRotate180 ? visualY(_y, _labelHeight)
                                         : (int32_t)_y + (int32_t)_labelHeight - _sprite.height();
  const int32_t drawX = visualX(_x, _sprite.width());
  const int32_t clipY = visualY(_y, _labelHeight);
  if (drawOffset != _lastDrawOffset) {
    lcd.setClipRect(drawX, clipY, _sprite.width(), _labelHeight);
    const int32_t firstY = kVisualRotate180 ? baseY - drawOffset : baseY + drawOffset;
    _sprite.pushSprite(&lcd, drawX, firstY);

    if (drawOffset > loopHeight - (int32_t)_labelHeight) {
      const int32_t secondY =
          kVisualRotate180 ? firstY + loopHeight : baseY + drawOffset - loopHeight;
      _sprite.pushSprite(&lcd, drawX, secondY);
    }
    lcd.clearClipRect();
    _lastDrawOffset = drawOffset;
  }

  if (_n < loopHeight - _scrollSpeed) {
    _n += _scrollSpeed;
  } else {
    _n = 0;
    _lastDrawOffset = -1;
    _startTick = now;
    _scrollCount++;
  }
}

void RotatedLabel::setEnabled(bool state) {
  _enabled = state;
}

static void dispUpdateWorker() {
  switch (disp.currentView) {
    case ViewMode::Player: {  // プレイヤーのとき
      if (xSemaphoreTake(spFrameBuffer, 0) == pdTRUE) {
        lcd.startWrite();
        lblTitle.update();
        lblGame.update();
        lblAuthor.update();
        lblSystem.update();
        lcd.setClipRect(0, 0, LCD_W, LCD_H);
        lcd.endWrite();
        xSemaphoreGive(spFrameBuffer);
      }
      int64_t sec = playerWindow.dispData.time;
      if (ND::canPlay == false || ND::isPaused) {
        if (ND::isPaused) {
          // カウントダウン中だけは点滅させず、残り秒数を常時表示する。
          const bool visible = isPlayHoldCountdownActive() || ((millis() / 500) & 1) == 0;
          playerWindow.updateHeader(sec, visible);
        }
        return;
      }
      if (ND::fileFormat == FileFormat::VGM || ND::fileFormat == FileFormat::VGZ) {
        sec = vgm.getCurrentTimeSec();
      } else if (ND::fileFormat == FileFormat::MDX) {
        sec = MDX.getCurrentTimeSec();
      } else {
        return;
      }

      if (playerWindow.dispData.time != sec) {
        playerWindow.updateHeader(sec);
        playerWindow.dispData.time = sec;
      }
      break;
    }
    case ViewMode::Visual: {  // ビジュアルモードのとき
      int64_t sec = playerWindow.dispData.time;
      if (ND::canPlay == false || ND::isPaused) {
        if (ND::isPaused) {
          visualWindow.updateLabels();
          // カウントダウン中だけは点滅させず、残り秒数を常時表示する。
          const bool visible = isPlayHoldCountdownActive() || ((millis() / 500) & 1) == 0;
          visualWindow.drawTimestamp(sec, visible);
        }
        return;
      }
      visualWindow.update();
      if (ND::fileFormat == FileFormat::VGM || ND::fileFormat == FileFormat::VGZ) {
        sec = vgm.getCurrentTimeSec();
      } else if (ND::fileFormat == FileFormat::MDX) {
        sec = MDX.getCurrentTimeSec();
      } else {
        return;
      }

      // 秒だけでなく、MDXのテンポ変更も即座に表示へ反映する。
      visualWindow.drawTimestamp(sec);
      if (playerWindow.dispData.time != sec) {
        playerWindow.dispData.time = sec;
      }
      break;
    }
  }
}

static void dispUpdateTask(void* param) {
  while (1) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (ulTaskNotifyTake(pdTRUE, 0) > 0) {
      // queue collapse: 最新状態だけ描画
    }
    if (disp.stopTimerDrawing) {
      continue;
    }
    dispUpdateWorker();
  }
}

//---------------------------------------------------------------------------
// 描画更新タイマー処理
void IRAM_ATTR dispTimerHandler(void* param) {
  if (hDispUpdateTask != NULL) {
    xTaskNotifyGive(hDispUpdateTask);
  }
}

/**
 * @brief PNGファイルを開いて配置する
 *
 * @param path      パス
 * @param AA        アンチエイリアス
 * @param toSprite  フレームバッファに配置か直接描画
 * @return true
 * @return false
 */

bool openPNG(String path, bool AA = false, bool toSprite = true) {
  if (!loadPNG(path, AA)) {
    const String& error = getPNGErrorMessage();
    if (error != "") {
      frameBuffer.setFont(&fonts::Font2);
      frameBuffer.setCursor(0, 77);
      frameBuffer.printf("%s", error.c_str());
    }
    return false;
  }

  LGFX_Sprite& pngSprite = getPNGSprite();
  if (toSprite) {
    pngSprite.pushSprite(&frameBuffer, 0, 75);
  } else {
    pngSprite.pushSprite(&lcd, 0, 75);
  }

  return true;
}

// --------------------------------------------------------------------------
// 画面クラス
bool Disp::init() {
  if (!lcd.init()) {
    return false;
  }

  lcd.setRotation(0);
  lcd.fillScreen(C_BASEBG);
  lcd.endWrite();

  // 再描画用セマフォ
  spFrameBuffer = xSemaphoreCreateBinary();
  xSemaphoreGive(spFrameBuffer);

  // フレームバッファスプライト作成
  // frameBuffer.setPsram(true);
  frameBuffer.createSprite(LCD_W, LCD_H);

  if (!initPNGRenderer()) {
    return false;
  }

  this->stopTimerDrawing = true;
  hDispUpdateTask = NULL;

  BaseType_t taskOk =
      xTaskCreatePinnedToCore(dispUpdateTask, "dispUpdate", DISP_UPDATE_TASK_STACK, NULL,
                              DISP_UPDATE_TASK_PRIORITY, &hDispUpdateTask, DISP_UPDATE_TASK_CORE);
  if (taskOk != pdPASS || hDispUpdateTask == NULL) {
    Serial.println("ERROR: dispUpdateTask create failed.");
    return false;
  }

  // タイマー生成
  hDispTimer = xTimerCreate("DISP_TIMER", DISP_TIMER_INTERVAL, pdTRUE, NULL, dispTimerHandler);
  xTimerStart(hDispTimer, 0);

  return true;
}

void Disp::startTimer() {
  xTimerStart(hDispTimer, 0);
}

void Disp::stopTimer() {
  xTimerStop(hDispTimer, 0);
}

bool tryLockDrawing() {
  return xSemaphoreTake(spFrameBuffer, 0) == pdTRUE;
}

void lockDrawing() {
  xSemaphoreTake(spFrameBuffer, portMAX_DELAY);
}

void unlockDrawing() {
  xSemaphoreGive(spFrameBuffer);
}

Disp disp = Disp();

//---------------------------------------------------------------------------
// プレイヤー画面クラス
void PlayerWindow::init() {
}

void PlayerWindow::drawBG() {  // 背景描画
  frameBuffer.fillSprite(C_BASEBG);
  frameBuffer.fillRect(0, 0, LCD_W, 19, C_HEADER);
  // frameBuffer.fillRect(0, 75, LCD_W, 125, C_DARK);
  frameBuffer.fillRoundRect(1, 279, LCD_W - 2, 40, 2, C_DARK);
  if (ND::fileFormat == FileFormat::MDX) {
    frameBuffer.pushImage(7, 210, ICONS_WIDTH, ICONS_HEIGHT, icons_mdx);
  } else {
    frameBuffer.pushImage(7, 210, ICONS_WIDTH, ICONS_HEIGHT, icons_vgm);
  }
  frameBuffer.fillRoundRect(6, 283, 17, 14, 2, C_FOOTER_ACTIVE);
  frameBuffer.fillRoundRect(6, 301, 17, 14, 2, C_FOOTER_ACTIVE);
}

void PlayerWindow::redraw() {  // プレーヤー描画
  xSemaphoreTake(spFrameBuffer, portMAX_DELAY);

  disp.stopTimerDrawing = true;
  drawBG();
  disp.render.setUseRenderTask(false);
  disp.render.setDrawer(frameBuffer);
  disp.render.setAlignment(Align::TopLeft);

  disp.render.loadFont(fontMain, sizeof(fontMain));
  disp.render.setFontSize(16);
  disp.render.setFontColor(C_GRAY, C_BASEBG);
  disp.render.setCursor(27, 255);
  disp.render.printf(dispData.date.c_str());

  // シャッフルアイコン
  if (ndConfig.get(CFG_SHUFFLE) != TRANDOM_NO) {
    disp.render.setCursor(3, 2);
    disp.render.setFontSize(16);
    disp.render.setFontColor(C_LIGHTGRAY, C_HEADER);
    disp.render.printf("丂");
  }

  disp.render.unloadFont();

  disp.render.loadFont(nimbusBold, sizeof(nimbusBold));
  disp.render.setFontSize(13);
  disp.render.setFontColor(C_YELLOW, C_DARK);
  disp.render.setCursor(27, 284);
  disp.render.printf(dispData.chip0.c_str());
  disp.render.setCursor(27, 303);
  disp.render.printf(dispData.chip1.c_str());

  disp.render.setFontColor(C_LIGHTGRAY, C_FOOTER_INACTIVE);
  disp.render.setCursor(11, 284);
  disp.render.printf("1");
  disp.render.setCursor(11, 303);
  disp.render.printf("2");

  disp.render.setFontSize(14);
  if (dispData.no != 0 && dispData.maxFiles != 0) {
    disp.render.setFontColor(C_GRAY, C_HEADER);
    disp.render.setCursor(167, 4);
    disp.render.setAlignment(Align::TopRight);
    disp.render.printf("%02d/%02d", dispData.no, dispData.maxFiles);
  }

  disp.render.setAlignment(Align::TopCenter);
  disp.render.setFontColor(C_LIGHTGRAY, C_HEADER);
  disp.render.setCursor(LCD_W / 2, 4);
  char timestamp[24];
  formatTimestamp(timestamp, sizeof(timestamp), dispData.time);
  disp.render.printf("%s", timestamp);

  disp.render.setFontSize(13);
  if (dispData.type == "MDX") {
    disp.render.setFontColor(MDX.isLZX ? C_LIME : C_MDX, C_HEADER);  // 圧縮時の文字色
  } else if (ndFile.accessMode == ACCESS_CACHE) {
    disp.render.setFontColor(C_CYAN, C_HEADER);
  } else {
    disp.render.setFontColor(C_ORANGE, C_HEADER);
  }
  // シャッフルアイコン分ずらす
  if (ndConfig.get(CFG_SHUFFLE) != TRANDOM_NO) {
    disp.render.setCursor(22, 4);
  } else {
    disp.render.setCursor(4, 4);
  }

  disp.render.setAlignment(Align::TopLeft);
  disp.render.printf(dispData.type.c_str());

  disp.render.unloadFont();

  // ready中に生成済みなら、ここではPSRAMスプライトの転送だけで済む。
  // 言語設定が変わっていた場合に限り、この場で作り直す。
  preparePlayerLabels(dispData);
  prepareVisualSongTitle(dispData);
  drawPreparedPlayerLabels();

  // Snapshot
  // 1) snap/[finemame].png
  // 2) snap/[songno].png
  // 3) ***.png

  String currentDir = fileTree.getFullPath(ndFile.currentNode->parent);
  String filePngName = ndFile.currentNode->pngName ? String(ndFile.currentNode->pngName) : "";
  String pngName =
      ndFile.currentNode->parent->pngName ? String(ndFile.currentNode->parent->pngName) : "";
  String filePngPath =
      (currentDir == "/") ? "/snap/" + filePngName : currentDir + "/snap/" + filePngName;
  String folderPngPath = (currentDir == "/") ? "/" + pngName : currentDir + "/" + pngName;

  if (filePngName != "") {
    openPNG(filePngPath, true, true);
  } else if (pngName != "") {
    openPNG(folderPngPath, true, true);
  }

  frameBuffer.pushSprite(0, 0);
  disp.stopTimerDrawing = false;
  xSemaphoreGive(spFrameBuffer);
}

void PlayerWindow::updateDisp(tDispData data) {
  dispData.authorEn = data.authorEn;
  dispData.authorJp = data.authorJp;
  dispData.chip0 = data.chip0;
  dispData.chip1 = data.chip1;
  dispData.date = data.date;
  dispData.gameEn = data.gameEn;
  dispData.gameJp = data.gameJp;
  dispData.no = data.no;
  dispData.maxFiles = data.maxFiles;
  dispData.systemEn = data.systemEn;
  dispData.systemJp = data.systemJp;
  dispData.trackEn = data.trackEn;
  dispData.trackJp = data.trackJp;
  dispData.type = data.type;
  dispData.time = 0;

  if (disp.currentView == ViewMode::Player) {
    playerWindow.redraw();
  } else if (disp.currentView == ViewMode::Visual) {
    visualWindow.draw();
  }
}

// ヘッダ更新。ticksToWait が 0 の通常更新は、描画中なら捨てて次回更新へ回す。
void PlayerWindow::updateHeader(int64_t sec, bool visible, uint32_t ticksToWait) {
  if (xSemaphoreTake(spFrameBuffer, ticksToWait) != pdTRUE) {
    return;
  }

  if (_sprHeader.width() == 0) {
    _sprHeader.setPsram(false);
    _sprHeader.createSprite(50, 14);
    if (_sprHeader.width() == 0) {
      xSemaphoreGive(spFrameBuffer);
      return;
    }
  }

  _sprHeader.fillSprite(C_HEADER);
  disp.render.setDrawer(_sprHeader);
  disp.render.setAlignment(Align::TopCenter);
  disp.render.loadFont(nimbusBold, sizeof(nimbusBold));
  disp.render.setFontSize(14);
  disp.render.setFontColor(TFT_WHITE);
  if (visible) {
    char timestamp[24];
    formatTimestamp(timestamp, sizeof(timestamp), sec);
    disp.render.setCursor(25, 0);
    disp.render.printf("%s", timestamp);
  }
  disp.render.unloadFont();
  _sprHeader.pushSprite(60, 4);
  xSemaphoreGive(spFrameBuffer);
}

// イベント処理
void PlayerWindow::eventHandler(event event) {
  switch (event) {
    case event::Up: {
      break;
    }
    case event::Down: {
      break;
    }
    case event::Left: {
      visualWindow.show();
      break;
    }
    case event::Right: {
      break;
    }
    case event::Option: {
      cfgWindow.show();
      break;
    }
    case event::Menu: {
      browserWindow.show();
      break;
    }
    case event::EncClick: {
      browserWindow.show();
      break;
    }
  }
}

void PlayerWindow::show() {
  ScopedEncoderDisable encoderGuard;
  disp.currentView = ViewMode::Player;
  disp.lastView = ViewMode::Player;
  ndConfig.saveLastView(LAST_VIEW_PLAYER);
  redraw();
  disp.stopTimerDrawing = false;  // タイマー描画更新再開
}

PlayerWindow playerWindow = PlayerWindow();

//---------------------------------------------------------------------------
// 設定画面クラス
// 項目描画用
ConfigPanelRenderer configRenderer;
static Panel pnlConfig = Panel(0, 26, LCD_W, 264, CFG_ITEM_HEIGHT, &configRenderer);

void ConfigPanelRenderer::onDrawItem(LGFX_Sprite& target, int itemIndex, int x, int y, int width,
                                     bool selected) {
  cfgWindow.drawItem(target, itemIndex, x, y, width, selected);
}

void ConfigPanelRenderer::onClick(int itemIndex) {
}

void CFGWindow::init() {
  _sprite.createSprite(LCD_W, CFG_ITEM_HEIGHT);
  _sprFooter.createSprite(120, 23);

  // ヘッダーの初期化
  initHeaders();
}

// ヘッダ部初期化
void CFGWindow::initHeaders() {
  // スプライトが既に初期化されているかチェック
  if (_sprHeaderJP.width() > 0 && _sprHeaderEN.width() > 0) return;

  // 日本語ヘッダー作成
  _sprHeaderJP.setPsram(true);
  _sprHeaderJP.createSprite(LCD_W, 26);
  _sprHeaderJP.fillSprite(C_HEADER);
  _sprHeaderJP.pushImage(146, 3, CFG_ICON_WIDTH, CFG_ICON_HEIGHT, cfgIcon);

  disp.render.setDrawer(_sprHeaderJP);
  disp.render.setAlignment(Align::TopLeft);
  disp.render.loadFont(fontMain, sizeof(fontMain));
  disp.render.setFontSize(17);
  disp.render.setFontColor(C_LIGHTGRAY, C_HEADER);
  disp.render.setCursor(6, 4);
  disp.render.printf("設定");
  disp.render.unloadFont();

  // 英語ヘッダー作成
  _sprHeaderEN.setPsram(true);
  _sprHeaderEN.createSprite(LCD_W, 26);
  _sprHeaderEN.fillSprite(C_HEADER);
  _sprHeaderEN.pushImage(146, 3, CFG_ICON_WIDTH, CFG_ICON_HEIGHT, cfgIcon);

  disp.render.setDrawer(_sprHeaderEN);
  disp.render.setAlignment(Align::TopLeft);
  // disp.render.loadFont(nimbusBold, sizeof(nimbusBold));
  disp.render.loadFont(fontMain, sizeof(fontMain));
  disp.render.setFontSize(16);
  disp.render.setFontColor(C_LIGHTGRAY, C_HEADER);
  disp.render.setCursor(6, 4);
  disp.render.printf("Settings");
  disp.render.unloadFont();
}

void CFGWindow::eventHandler(event event) {
  if (disp.currentView == ViewMode::Config) {
    // オプションモードはイベント振り直し
    if (_isOptionMode) {
      if (event == event::Up) {
        event = event::Left;
      } else if (event == event::Down) {
        event = event::Right;
      }
    }

    switch (event) {
      case event::Option: {
        this->show();
        break;
      }
      case event::Close: {
        // Serial.printf("CFG Window::Close event.\n");
        this->close();
        if (ndConfig.get(CFG_CONTROL) == CTRL_2) {
          visualWindow.show();
          break;
        }
        switch (_returnView) {
          case ViewMode::Player:
            playerWindow.show();
            break;
          case ViewMode::Visual:
            visualWindow.show();
            break;
          case ViewMode::Browser:
            browserWindow.show();
            break;
          default:
            visualWindow.show();
            break;
        }
        break;
      }
      case event::Up: {
        moveSelection(-1);
        break;
      }
      case event::Down: {
        moveSelection(1);
        break;
      }
      case event::Left: {
        if (ndConfig.items[this->currentItemIndex].index != 0) {
          int prevRandom = ndConfig.get(CFG_SHUFFLE);
          ndConfig.items[this->currentItemIndex].index--;

          // シャッフル設定の切り替え時は、シャッフルステートをリセット
          const tConfig changedItem = ndConfig.configAt(this->currentItemIndex);
          if (changedItem == CFG_SHUFFLE && prevRandom != ndConfig.get(CFG_SHUFFLE)) {
            ndFile.resetRandomSession();
          }

          // 言語はすぐ再描画
          if (changedItem == CFG_LANG) {
            drawPanelView();
          } else {
            refreshCurrentItem();
          }
          ndConfig.applyCfg();
          _isChanged = true;
        }
        break;
      }
      case event::Right: {
        if (ndConfig.items[this->currentItemIndex].index !=
            ndConfig.items[this->currentItemIndex].optionValues.size() - 1) {
          int prevRandom = ndConfig.get(CFG_SHUFFLE);
          ndConfig.items[this->currentItemIndex].index++;

          // シャッフル設定の切り替え時は、シャッフルステートをリセット
          const tConfig changedItem = ndConfig.configAt(this->currentItemIndex);
          if (changedItem == CFG_SHUFFLE && prevRandom != ndConfig.get(CFG_SHUFFLE)) {
            ndFile.resetRandomSession();
          }

          // 言語はすぐ再描画
          if (changedItem == CFG_LANG) {
            drawPanelView();
          } else {
            refreshCurrentItem();
          }
          ndConfig.applyCfg();
          _isChanged = true;
        }
        break;
      }
      case event::Menu: {
        this->close();
        browserWindow.show();
        break;
      }
      case event::EncClick: {
        _isOptionMode = !_isOptionMode;
        /*
        if (_isOptionMode) {
          Serial.printf("オプションモード ON\n");
        } else {
          Serial.printf("オプションモード OFF\n");
        }
        */
        refreshCurrentItem();
        break;
      }
    }
  }
}

int CFGWindow::getItemCount() const {
  return ndConfig.items.size();
}

void CFGWindow::drawPanelView() {
  int itemCount = getItemCount();
  if (itemCount <= 0) return;

  if (currentItemIndex < 0) {
    currentItemIndex = 0;
  } else if (currentItemIndex >= itemCount) {
    currentItemIndex = itemCount - 1;
  }

  xSemaphoreTake(spFrameBuffer, portMAX_DELAY);

  frameBuffer.fillSprite(TFT_WHITE);

  if (ndConfig.get(CFG_LANG) == LANG_JA) {
    _sprHeaderJP.pushSprite(&frameBuffer, 0, 0);
  } else {
    _sprHeaderEN.pushSprite(&frameBuffer, 0, 0);
  }

  frameBuffer.fillRoundRect(124, 293, 42, 23, 2, C_FOOTER_ACTIVE);

  disp.render.setDrawer(frameBuffer);
  disp.render.setAlignment(Align::TopCenter);
  disp.render.setFontColor(C_LIGHTGRAY, C_FOOTER_ACTIVE);

  disp.render.loadFont(fontMain, sizeof(fontMain));
  disp.render.setFontSize(17);
  disp.render.setCursor(124 + 42 / 2, 297);
  disp.render.printf("OK");

  disp.render.setAlignment(Align::TopLeft);
  pnlConfig.setItemCount(itemCount);
  pnlConfig.currentIndex = currentItemIndex;
  pnlConfig.ensureVisible();
  pnlConfig.invalidate();
  pnlConfig.update(frameBuffer);
  currentItemIndex = pnlConfig.currentIndex;

  drawFooter(true);

  disp.render.unloadFont();
  frameBuffer.pushSprite(0, 0);
  xSemaphoreGive(spFrameBuffer);
}

void CFGWindow::refreshPanel() {
  int itemCount = getItemCount();
  if (itemCount <= 0) return;

  if (currentItemIndex < 0) {
    currentItemIndex = 0;
  } else if (currentItemIndex >= itemCount) {
    currentItemIndex = itemCount - 1;
  }

  xSemaphoreTake(spFrameBuffer, portMAX_DELAY);
  pnlConfig.setItemCount(itemCount);
  pnlConfig.currentIndex = currentItemIndex;
  pnlConfig.invalidate();
  pnlConfig.update(frameBuffer);
  currentItemIndex = pnlConfig.currentIndex;
  drawFooter(true);
  frameBuffer.pushSprite(0, 0);
  xSemaphoreGive(spFrameBuffer);
}

void CFGWindow::refreshCurrentItem() {
  int itemCount = getItemCount();
  if (itemCount <= 0) return;

  if (currentItemIndex < 0) {
    currentItemIndex = 0;
  } else if (currentItemIndex >= itemCount) {
    currentItemIndex = itemCount - 1;
  }

  xSemaphoreTake(spFrameBuffer, portMAX_DELAY);
  pnlConfig.setItemCount(itemCount);
  pnlConfig.currentIndex = currentItemIndex;
  pnlConfig.ensureVisible();
  currentItemIndex = pnlConfig.currentIndex;

  const int itemY = pnlConfig.y + currentItemIndex * CFG_ITEM_HEIGHT - pnlConfig.scrollTop;
  const bool fullyVisible =
      itemY >= pnlConfig.y && itemY + CFG_ITEM_HEIGHT <= pnlConfig.y + pnlConfig.height;

  if (fullyVisible) {
    drawItem(frameBuffer, currentItemIndex, pnlConfig.x, itemY, pnlConfig.itemWidth(), true);
    _sprite.pushSprite(&lcd, pnlConfig.x, itemY);

    drawFooter(true);
    _sprFooter.pushSprite(&lcd, 0, 293);
  } else {
    pnlConfig.invalidate();
    pnlConfig.update(frameBuffer);
    drawFooter(true);
    frameBuffer.pushSprite(0, 0);
  }

  xSemaphoreGive(spFrameBuffer);
}

void CFGWindow::selectItem(int index) {
  int itemCount = getItemCount();
  if (itemCount <= 0) return;

  if (index < 0) index = 0;
  if (index >= itemCount) index = itemCount - 1;

  xSemaphoreTake(spFrameBuffer, portMAX_DELAY);
  currentItemIndex = index;
  pnlConfig.setItemCount(itemCount);
  pnlConfig.currentIndex = currentItemIndex;
  pnlConfig.ensureVisible();
  pnlConfig.update(frameBuffer);
  currentItemIndex = pnlConfig.currentIndex;
  drawFooter(true);
  frameBuffer.pushSprite(0, 0);
  xSemaphoreGive(spFrameBuffer);
}

void CFGWindow::moveSelection(int delta) {
  int itemCount = getItemCount();
  if (itemCount <= 0) return;

  int nextIndex = currentItemIndex + delta;
  if (nextIndex < 0) {
    nextIndex = 0;
  } else if (nextIndex >= itemCount) {
    nextIndex = itemCount - 1;
  }

  if (nextIndex == currentItemIndex) return;
  selectItem(nextIndex);
}

void CFGWindow::drawItem(LGFX_Sprite& target, int index, int x, int y, int width, bool selected) {
  if (index < 0 || index >= ndConfig.items.size() || width <= 0) return;

  if (_sprite.width() != width || _sprite.height() != CFG_ITEM_HEIGHT) {
    _sprite.createSprite(width, CFG_ITEM_HEIGHT);
  }

  int titleTextColor = C_DARK;
  int optionTextColor = C_MID;
  int backgroundColor = TFT_WHITE;
  int borderColor = C_BORDER;

  if (selected) {
    titleTextColor = TFT_WHITE;
    optionTextColor = C_YELLOW;
    backgroundColor = C_LV_PEAK;  // C_ACCENT_DARK;
    if (_isOptionMode) {
      titleTextColor = TFT_WHITE;
      optionTextColor = C_YELLOW;
      backgroundColor = C_HEADERSUB;
      borderColor = C_ACCENT_DARK;
    }
  }

  OpenFontRender ofr;
  ofr.setDrawer(_sprite);

  String label;
  String option;
  int fontSize;
  if (ndConfig.get(CFG_LANG) == LANG_JA) {
    ofr.loadFont(fontMain, sizeof(fontMain));
    fontSize = 17;
    label = ndConfig.items[index].labelJp;
    option = ndConfig.items[index].optionsJp[ndConfig.items[index].index];
  } else {
    ofr.loadFont(fontMain, sizeof(fontMain));
    fontSize = 16;
    label = ndConfig.items[index].labelEn;
    option = ndConfig.items[index].optionsEn[ndConfig.items[index].index];
  }

  _sprite.fillSprite(backgroundColor);
  _sprite.drawLine(0, CFG_ITEM_HEIGHT - 1, width - 1, CFG_ITEM_HEIGHT - 1, borderColor);
  /*if (selected && _isOptionMode) {
    _sprite.drawRect(0, 0, width, CFG_ITEM_HEIGHT, borderColor);
    _sprite.drawRect(1, 1, width - 2, CFG_ITEM_HEIGHT - 2, borderColor);
  }*/
  ofr.setFontSize(fontSize);
  ofr.setAlignment(Align::TopLeft);
  ofr.setFontColor(titleTextColor, backgroundColor);
  ofr.setCursor(5, (CFG_ITEM_HEIGHT - fontSize) / 2);
  ofr.printf(label.c_str());

  ofr.setFontColor(optionTextColor, backgroundColor);
  ofr.setAlignment(Align::TopRight);
  ofr.setCursor(width - 5, (CFG_ITEM_HEIGHT - fontSize) / 2);
  ofr.printf(option.c_str());

  ofr.unloadFont();
  _sprite.pushSprite(&target, x, y);
}

void CFGWindow::drawFooter(bool toFrameBuffer) {
  bool up, down, left, right;
  int color;

  _sprFooter.fillSprite(TFT_WHITE);

  up = !_isOptionMode && (this->currentItemIndex != 0);
  down = !_isOptionMode && (this->currentItemIndex != ndConfig.items.size() - 1);
  left = (ndConfig.items[this->currentItemIndex].index != 0);
  right = (ndConfig.items[this->currentItemIndex].index !=
           ndConfig.items[this->currentItemIndex].optionValues.size() - 1);

  _sprFooter.fillRoundRect(4, 0, 27, 23, 2, left ? C_FOOTER_ACTIVE : C_FOOTER_INACTIVE);
  _sprFooter.fillRoundRect(33, 0, 27, 23, 2, color = up ? C_FOOTER_ACTIVE : C_FOOTER_INACTIVE);
  _sprFooter.fillRoundRect(64, 0, 27, 23, 2, color = down ? C_FOOTER_ACTIVE : C_FOOTER_INACTIVE);
  _sprFooter.fillRoundRect(93, 0, 27, 23, 2, color = right ? C_FOOTER_ACTIVE : C_FOOTER_INACTIVE);
  if (left) {
    _sprFooter.pushImage(12, 6, CFG_ICON_ARROR_WIDTH, CFG_ICON_ARROR_HEIGHT, cfgLEFT);
  }
  if (up) {
    _sprFooter.pushImage(41, 6, CFG_ICON_ARROR_WIDTH, CFG_ICON_ARROR_HEIGHT, cfgUP);
  }
  if (down) {
    _sprFooter.pushImage(72, 6, CFG_ICON_ARROR_WIDTH, CFG_ICON_ARROR_HEIGHT, cfgDOWN);
  }
  if (right) {
    _sprFooter.pushImage(101, 6, CFG_ICON_ARROR_WIDTH, CFG_ICON_ARROR_HEIGHT, cfgRIGHT);
  }

  if (toFrameBuffer) {
    _sprFooter.pushSprite(&frameBuffer, 0, 293);
  } else {
    _sprFooter.pushSprite(&lcd, 0, 293);
  }
}

// 表示
void CFGWindow::show() {
  const bool isOpening = disp.currentView != ViewMode::Config;
  if (isOpening) _returnView = disp.currentView;
  _isOptionMode = false;  // オプションモードオフ
  if (isOpening) _isChanged = false;
  ScopedEncoderDisable encoderGuard;
  //
  disp.currentView = ViewMode::Config;
  drawPanelView();
}

void CFGWindow::close() {
  if (_isChanged) {
    ndConfig.saveCfg();
    _isChanged = false;
  }
}

CFGWindow cfgWindow = CFGWindow();

// ビジュアルウィンドウ
void VisualWindow::init() {
  kVisualRotate180 = ndConfig.get(CFG_VWROTATE) == VW_ROTATE_ON;

  // フレームバッファスプライト作成
  // keyboardBuffer.setPsram(true);
  keyboardBuffer.createSprite(keyboard2Width, keyboard2Height);
  pushPreparedVisualImage(keyboardBuffer, 0, 0, keyboard2Width, keyboard2Height, keyboard2);
  // keyboardBufferSub.setPsram(true);
  keyboardBufferSub.createSprite(keyboard2Width, keyboard2Height);
  cutPanMarkerSprite(panMarkerCenter, 0);
  cutPanMarkerSprite(panMarkerLeft, 1);
  cutPanMarkerSprite(panMarkerRight, 2);
  cutPanMarkerSprite(panMarkerMute, 3);
  for (u8_t i = 0; i < kLevelSpriteCount; i++) {
    cutLevelSprite(i);
  }
  for (u8_t i = 0; i < kNumberSpriteCount; i++) {
    cutNumberSprite(i);
  }
  for (u8_t i = 0; i < kLabelSpriteCount; i++) {
    cutLabelSprite(i);
  }

  createVisualShuffleIconSprite(_sprShuffleOn, C_MDX_ON);
  createVisualShuffleIconSprite(_sprShuffleOff, C_MDX_OFF);

  _sprTime.setPsram(false);
  _sprTime.createSprite(14, 104);
}

void VisualWindow::drawTimestamp(int64_t sec) {
  lockDrawing();
  drawTimestamp(sec, false, true);
  unlockDrawing();
}

void VisualWindow::drawTimestamp(int64_t sec, bool visible) {
  lockDrawing();
  drawTimestamp(sec, false, visible);
  unlockDrawing();
}

void VisualWindow::drawTimestamp(int64_t sec, bool toFrameBuffer, bool visible) {
  if (_sprTime.width() == 0) {
    return;
  }
  static constexpr int kTempoHidden = -1;
  static constexpr int kTempoNoteOnly = -2;
  const int tempoBpm = ND::fileFormat != FileFormat::MDX
                           ? kTempoHidden
                           : (MDX.hasExplicitTempo() ? MDX.getTempoBpm() : kTempoNoteOnly);
  if (_lastTimestampSec == sec && _lastTempoBpm == tempoBpm &&
      _lastTimestampVisible == visible) {
    return;
  }

  static constexpr int kTimestampSourceW = 104;
  static constexpr int kTimestampSourceH = 14;
  LGFX_Sprite source(&lcd);
  source.setPsram(false);
  source.createSprite(kTimestampSourceW, kTimestampSourceH);
  if (source.width() == 0) {
    return;
  }

  source.fillSprite(TFT_BLACK);
  disp.render.setDrawer(source);
  disp.render.setAlignment(Align::TopRight);
  disp.render.loadFont(nimbusBold, sizeof(nimbusBold));
  disp.render.setFontSize(14);
  disp.render.setFontColor(C_MDX_ON, TFT_BLACK);
  if (visible) {
    if (tempoBpm != kTempoHidden) {
      disp.render.unloadFont();
      disp.render.loadFont(fontMain, sizeof(fontMain));
      disp.render.setAlignment(Align::TopLeft);
      disp.render.setCursor(0, 0);
      if (tempoBpm == kTempoNoteOnly) {
        disp.render.printf("♪");
      } else {
        disp.render.printf("♪%d", tempoBpm);
      }
      disp.render.unloadFont();
      disp.render.loadFont(nimbusBold, sizeof(nimbusBold));
      disp.render.setAlignment(Align::TopRight);
    }
    char timestamp[24];
    formatTimestamp(timestamp, sizeof(timestamp), sec);
    disp.render.setCursor(source.width(), 0);
    disp.render.printf("%s", timestamp);
  }
  disp.render.unloadFont();

  _sprTime.fillSprite(TFT_BLACK);
  source.setPivot(source.width() / 2.0f, source.height() / 2.0f);
  const float angle = kVisualRotate180 ? 90.0f : -90.0f;
  source.pushRotateZoom(&_sprTime, _sprTime.width() / 2.0f, _sprTime.height() / 2.0f, angle, 1.0f,
                        1.0f, TFT_BLACK);
  source.deleteSprite();

  const int x = LCD_W - _sprTime.width();
  const int y = 0;
  if (toFrameBuffer) {
    pushVisualSpriteToFrameBuffer(_sprTime, x, y);
  } else {
    pushVisualSpriteToLcd(_sprTime, x, y);
  }
  _lastTimestampSec = sec;
  _lastTempoBpm = tempoBpm;
  _lastTimestampVisible = visible;
}

void VisualWindow::draw() {
  // u32_t t0 = millis();

  xSemaphoreTake(spFrameBuffer, portMAX_DELAY);
  // 曲ロード時に開始された描画が画面遷移後まで待機していた場合は、
  // 新しい画面を古いVisual画面で上書きしない。
  if (disp.currentView != ViewMode::Visual) {
    xSemaphoreGive(spFrameBuffer);
    return;
  }

  // OpenFontRenderは内部のFreeType状態を全インスタンスで共有するため、
  // 他画面の描画と競合しない描画ロック内でPlayer用Labelも先行生成する。
  preparePlayerLabels(playerWindow.dispData);
  pushVisualImage(frameBuffer, 0, 0, keyboardWidth, keyboardHeight, keyboard);
  if (ND::fileFormat == FileFormat::MDX) {
    drawLabelClip(frameBuffer, 1, kLabelMdxX, kLabelMdxY);
    if (MDX.pcm8) {
      drawLabelClip(frameBuffer, 0, kLabelPcm8X, kLabelPcm8Y);
    }
  } else if (ND::fileFormat == FileFormat::VGM || ND::fileFormat == FileFormat::VGZ) {
    drawLabelClip(frameBuffer, 2, kLabelVgmX, kLabelVgmY);
  }
  // ready中に生成済みなら、ここでは縦向きスプライトの転送だけで済む。
  prepareVisualSongTitle(playerWindow.dispData);
  lblSongTitle.drawPrepared();

  // シャッフル再生のアイコン表示
  if (ndConfig.get(CFG_SHUFFLE) != TRANDOM_NO) {
    pushVisualSpriteToFrameBuffer(_sprShuffleOn, 154, 113);
  } else {
    pushVisualSpriteToFrameBuffer(_sprShuffleOff, 154, 113);
  }

  _lastTimestampSec = INT64_MAX;
  _lastTempoBpm = -1;
  _lastTimestampVisible = false;
  drawTimestamp(playerWindow.dispData.time, true, true);
  frameBuffer.pushSprite(0, 0);
  xSemaphoreGive(spFrameBuffer);

  _lastTimestampSec = INT64_MAX;
  drawTimestamp(playerWindow.dispData.time);

  for (int i = 0; i < 16; i++) {
    lastTrackPan[i] = -1;
    lastTrackLevel[i] = -1;
    lastTrackPeak[i] = -1;
    lastTrackNote[i] = -1;
    heldTrackNote[i] = 0xff;
    trackLevelDisplayQ8[i] = 0;
    trackLevelFallSpeedQ8[i] = 0;
    trackPeakDisplayQ8[i] = 0;
    trackPeakFallSpeedQ8[i] = 0;
    trackPeakHoldFrames[i] = 0;
  }
  // u32_t t1 = millis();
  //  Serial.printf("VisualWindow::draw time: %d ms\n", t1 - t0);
  disp.stopTimerDrawing = false;  // タイマー更新再開, タイマー側のupdateイベントが先に起こるので
}

// 更新処理
void VisualWindow::update() {
#if ND_DEBUG_VISUAL_UPDATE
  u32_t t0 = micros();
#endif

  // キー情報更新
  if (disp.currentView == ViewMode::Visual) {
    // キーボードのスプライト配置
    keyboardBufferSub.setColor(static_cast<uint16_t>(ndConfig.get(CFG_KEYON)));

    NoteInfo keySnapshot[DEVICE_COUNT][MAX_CHANNELS];
    tPan panSnapshot[16];
    u8_t levelSnapshot[16];
    u8_t noteSnapshot[16];
    u8_t ym2151ChannelMaskSnapshot;
    if (xSemaphoreTake(KeyBoard.keyinfoMutex, 0) != pdTRUE) {
#if ND_DEBUG_VISUAL_UPDATE
      visualUpdateSkipCount++;
#endif
      return;
    }
    memcpy(keySnapshot, KeyBoard.keyInfo, sizeof(keySnapshot));
    memcpy(panSnapshot, KeyBoard.trackPan, sizeof(panSnapshot));
    memcpy(levelSnapshot, KeyBoard.trackLevel, sizeof(levelSnapshot));
    ym2151ChannelMaskSnapshot = KeyBoard.ym2151ChannelMask;
    // Peak meters are latched one-shot values. Clear the sampled source and let the waterfall
    // decay.
    memset(KeyBoard.trackLevel, 0, sizeof(KeyBoard.trackLevel));
    xSemaphoreGive(KeyBoard.keyinfoMutex);

    for (int i = 0; i < 8; i++) {
      u8_t ymNote = noteInfoToDisplayNoteNo(YM2151, keySnapshot[YM2151][i]);
      if (ymNote != 0xff) {
        heldTrackNote[i] = ymNote;
      }
      noteSnapshot[i] = heldTrackNote[i];

      u8_t okiNote = noteInfoToDisplayNoteNo(OKIM6258_KEY, keySnapshot[OKIM6258_KEY][i]);
      if (okiNote != 0xff) {
        heldTrackNote[i + 8] = okiNote;
      }
      noteSnapshot[i + 8] = heldTrackNote[i + 8];
    }

    if (!tryLockDrawing()) {
#if ND_DEBUG_VISUAL_UPDATE
      visualUpdateSkipCount++;
#endif
      return;
    }

    // YM2151
    keyboardBuffer.pushSprite(&keyboardBufferSub, 0, 0);
    keyboardDrawChannelMask = ym2151ChannelMaskSnapshot;
    drawKeyboard(keyboardBufferSub, YM2151, keySnapshot[YM2151]);
    keyboardDrawChannelMask = 0;
    pushVisualSpriteToLcd(keyboardBufferSub, 2, 0);

    // OKIM6258
    keyboardBuffer.pushSprite(&keyboardBufferSub, 0, 0);
    drawKeyboard(keyboardBufferSub, OKIM6258_KEY, keySnapshot[OKIM6258_KEY]);
    pushVisualSpriteToLcd(keyboardBufferSub, 30, 0);

    for (int i = 0; i < 16; i++) {
      u8_t displayLevel = updateTrackLevelWaterfall(i, levelSnapshot[i]);
      u8_t peakLevel = updateTrackPeakHold(i);
      if (lastTrackLevel[i] != displayLevel || lastTrackPeak[i] != peakLevel) {
        drawLevel(i, displayLevel, peakLevel);
        lastTrackLevel[i] = displayLevel;
        lastTrackPeak[i] = peakLevel;
      }
      if (lastTrackPan[i] != panSnapshot[i]) {
        drawPan(i, panSnapshot[i]);
        lastTrackPan[i] = panSnapshot[i];
      }
      if (lastTrackNote[i] != noteSnapshot[i]) {
        drawNote(i, noteSnapshot[i]);
        lastTrackNote[i] = noteSnapshot[i];
      }
    }
    lblSongTitle.update();
    unlockDrawing();
  }

#if ND_DEBUG_VISUAL_UPDATE
  visualUpdateTimeUs += (u32_t)(micros() - t0);
  visualUpdateCount++;
  if (visualUpdateCount >= 500) {
    Serial.printf("VisualWindow::update avg=%lu us skip=%lu\n",
                  (u32_t)(visualUpdateTimeUs / visualUpdateCount), visualUpdateSkipCount);
    visualUpdateCount = 0;
    visualUpdateSkipCount = 0;
    visualUpdateTimeUs = 0;
  }
#endif
}

void VisualWindow::updateLabels() {
  if (!tryLockDrawing()) {
    return;
  }

  // 再生ホールド中も Visual 画面の曲名スクロールだけは止めない。
  lblSongTitle.update();
  unlockDrawing();
}

// 個別のキーボードを描画する
// 戻り値: true 描画更新した
boolean VisualWindow::drawKeyboard(LGFX_Sprite& sprite, t_device device, const NoteInfo* notes) {
  // 鍵盤描画位置用定数
  const int notePos[12] = {5, 7, 10, 12, 15, 20, 22, 25, 27, 30, 32, 35};
  const int noteWidth[12] = {4, 3, 4, 3, 4, 4, 3, 4, 3, 4, 3, 4};

  bool touched = false;
  const uint16_t keyOnColor = static_cast<uint16_t>(ndConfig.get(CFG_KEYON));

  for (int i = 0; i < device_channels[device]; i++) {
    int oct = notes[i].octave;
    int note = notes[i].note;
    if (oct > 0 || (oct == 0 && note > 8)) {  // オクターブ0は A以上
      if (note < 0 || note > 11) {
        continue;
      }

      touched = true;
      const bool masked = device == YM2151 && (keyboardDrawChannelMask & (u8_t)(1u << i));
      sprite.setColor(masked ? C_MASKEDKEY : keyOnColor);
      int y = keyboard2Height - ((oct - 1) * 35 + notePos[note]) - 10;
      if (y < 0 || y >= keyboard2Height) {
        continue;
      }

      if (noteWidth[note] == 3) {
        fillVisualRect(sprite, 0, y, 15, 3);  // 黒鍵
      } else {
        // keyboard2Width=25 に対して x=15 の白鍵側は 9px 幅が上限。
        fillVisualRect(sprite, 15, y, 10, 4);
        switch (note) {
          case 0:
          case 5:
            fillVisualRect(sprite, 0, y + 1, 15, 3);
            break;
          case 4:
          case 11:
            fillVisualRect(sprite, 0, y, 15, 3);
            break;
          case 2:
          case 7:
          case 9:
            fillVisualRect(sprite, 0, y + 1, 15, 2);
            break;
        }
      }
    }
  }
  return touched;
}

// イベント処理
void VisualWindow::eventHandler(event ev) {
  switch (ev) {
    case event::Option: {
      cfgWindow.show();
      break;
    }
    case event::Close: {
      this->close();
      playerWindow.show();
      break;
    }
    case event::Up: {
      break;
    }
    case event::Down: {
      break;
    }
    case event::Left: {
      break;
    }
    case event::Right: {
      break;
    }
    case event::Menu: {
      this->close();
      browserWindow.show();
      break;
    }
  }
}

boolean VisualWindow::drawPan(u8_t trackNo, tPan pan) {
  LGFX_Sprite* marker = &panMarkerCenter;
  if (pan == PAN_LEFT) {
    marker = &panMarkerLeft;
  } else if (pan == PAN_RIGHT) {
    marker = &panMarkerRight;
  } else if (pan == PAN_MUTE) {
    marker = &panMarkerMute;
  }

  marker->pushSprite(visualX(99, marker->width()), visualY(264 - trackNo * 17, marker->height()));
  return true;
}

boolean VisualWindow::drawLevel(u8_t trackNo, u8_t level, u8_t peakLevel) {
  if (trackNo >= 16) {
    return false;
  }
  if (level >= kLevelSpriteCount) {
    level = kLevelSpriteCount - 1;
  }
  if (peakLevel >= kLevelSpriteCount) {
    peakLevel = kLevelSpriteCount - 1;
  }

  const int y = kLevelDrawBottomY - trackNo * kLevelSpriteHeight;
  const u8_t spriteIndex = (u8_t)((kLevelSpriteCount - 1) - level);
  memcpy(levelWorkBuffer, levelSprite[spriteIndex], sizeof(levelWorkBuffer));

  if (peakLevel > 0) {
    const int peakX = max(1, (int)kPeakLineX0 - ((int)peakLevel - 1) * 2);
    for (int peakY = kPeakLineYTop; peakY <= kPeakLineYBottom; peakY++) {
      const int drawX = kVisualRotate180 ? kLevelSpriteWidth - 1 - peakX : peakX;
      const int drawY = kVisualRotate180 ? kLevelSpriteHeight - 1 - peakY : peakY;
      levelWorkBuffer[drawY * kLevelSpriteWidth + drawX] = C_LV_PEAK;
    }
  }

  pushVisualBufferToLcd(kLevelDrawX, y, kLevelSpriteWidth, kLevelSpriteHeight, levelWorkBuffer);
  return true;
}

boolean VisualWindow::drawNote(u8_t trackNo, u8_t noteNo) {
  if (trackNo >= 16) {
    return false;
  }

  char upperChar = '-';
  char lowerChar = '-';
  if (noteNo <= 96) {
    upperChar = (char)('0' + (noteNo / 10));
    lowerChar = (char)('0' + (noteNo % 10));
  }

  const u8_t upperIndex = getNumberGlyphIndex(upperChar);
  const u8_t lowerIndex = getNumberGlyphIndex(lowerChar);
  const int y = kNoteDrawBottomY - trackNo * kLevelSpriteHeight;

  memcpy(&numberWorkBuffer[0], numberSprite[upperIndex],
         sizeof(u16_t) * kNumberSpriteWidth * kNumberSpriteHeight);
  memcpy(&numberWorkBuffer[kNumberSpriteWidth * kNumberSpriteHeight], numberSprite[lowerIndex],
         sizeof(u16_t) * kNumberSpriteWidth * kNumberSpriteHeight);

  pushVisualBufferToLcd(kNoteDrawX, y, kNumberSpriteWidth, kNumberSpriteHeight * 2,
                        numberWorkBuffer);
  return true;
}

void VisualWindow::show() {
  ScopedEncoderDisable encoderGuard;
  visible = true;
  disp.stopTimerDrawing = true;
  disp.currentView = ViewMode::Visual;
  disp.lastView = ViewMode::Visual;
  ndConfig.saveLastView(LAST_VIEW_VISUAL);
  draw();
  disp.stopTimerDrawing = false;
}

void VisualWindow::close() {
  visible = false;
  disp.stopTimerDrawing = true;
}

VisualWindow visualWindow = VisualWindow();

//------------------------------------------------------------------
// フィイルブラウザ画面

// リスト項目描画
BrowserPanelRenderer browserRenderer;
static Panel pnlFiles = Panel(0, 26 + CURRENTDIRHEIGHT, LCD_W, 264 - CURRENTDIRHEIGHT,
                              NODEITEMHEIGHT, &browserRenderer);

void BrowserPanelRenderer::setBrowseDirNode(Node* browseDirNode) {
  _browseDirNode = browseDirNode;
}

bool BrowserPanelRenderer::hasParentEntry() const {
  return _browseDirNode != nullptr && _browseDirNode != fileTree.getRoot() &&
         _browseDirNode->parent != nullptr;
}

Node* BrowserPanelRenderer::getNodeByDisplayIndex(int itemIndex) const {
  int nodeIndex = itemIndex;
  if (hasParentEntry()) {
    if (itemIndex == 0) return nullptr;
    nodeIndex--;
  }

  if (_browseDirNode == nullptr) return nullptr;

  Node* node = _browseDirNode->firstChild;
  for (int i = 0; node && i < nodeIndex; i++) {
    node = node->next;
  }
  return node;
}

// 項目描画処理　
void BrowserPanelRenderer::onDrawItem(LGFX_Sprite& target, int itemIndex, int x, int y, int width,
                                      bool selected) {
  int bg = (itemIndex % 2 != 0) ? C_LISTBG : TFT_WHITE;
  int fg = C_DARK;
  const unsigned short* icon = (itemIndex % 2 != 0) ? icoFolderOdd : icoFolderWhite;
  bool isParentEntry = hasParentEntry() && itemIndex == 0;
  Node* workNode = isParentEntry ? nullptr : getNodeByDisplayIndex(itemIndex);
  Node* playingNode = ndFile.currentNode;
  Node* playingParent = (ndFile.currentNode != nullptr) ? ndFile.currentNode->parent : nullptr;
  bool playing = (workNode == playingNode || workNode == playingParent);

  if (!isParentEntry && !workNode) return;

  if (selected) {
    bg = C_ACCENT_DARK;
    fg = TFT_WHITE;
    icon = icoFolderSelect;
  } else if (playing) {
    bg = C_MID;
    icon = icoFolderGray;
  }
  if (playing) {
    fg = C_YELLOW;
  }

  target.fillRect(x, y, width, NODEITEMHEIGHT, bg);

  if (isParentEntry || workNode->type == NODE_TYPE_DIR) {
    target.pushImage(x + 2, y + 5, 16, 16, icon);
  }

  disp.render.setDrawer(target);
  disp.render.setCursor(x + 20, y + 7);
  disp.render.setFontColor(fg, bg);
  disp.render.setFontSize(17);

  if (isParentEntry) {
    disp.render.printf("../");
  } else if (workNode == fileTree.getRoot()) {  // ルート
    disp.render.printf("/");
  } else {
    printWithEllipsis(disp.render, workNode->name, x + 20, y + 7, width - 20 - 3, _dotsWidth);
  }
}

// クリック処理
void BrowserPanelRenderer::onClick(int index) {
  if (_browseDirNode == nullptr) return;

  if (hasParentEntry() && index == 0) {
    browserWindow.openParentDirectory();
    return;
  }

  Node* workNode = getNodeByDisplayIndex(index);
  if (workNode == nullptr) return;

  if (workNode->type == NODE_TYPE_DIR) {
    browserWindow.openDirectory(workNode);
    return;
  }

  ndFile.requestPlay(workNode);
}

void BrowserPanelRenderer::init() {
  disp.render.unloadFont();
  disp.render.setUseRenderTask(false);
  disp.render.loadFont(fontMain, sizeof(fontMain));
  disp.render.setAlignment(Align::TopLeft);
  disp.render.setFontSize(17);
  _dotsWidth = disp.render.getTextWidth("...");
}

////

void BrowserWindow::init() {
  // ヘッダーの初期化
  initHeaders();

  if (_sprFooter.width() == 0) {
    _sprFooter.createSprite(LCD_W, 23);
  }

  if (_sprCurrentDir.width() == 0) {
    _sprCurrentDir.setPsram(true);
    _sprCurrentDir.createSprite(LCD_W, CURRENTDIRHEIGHT);
  }

  _lastCurrentDirNode = nullptr;
  _browseDirNode = nullptr;
  _selectedNode = nullptr;
}

// ヘッダ部初期化
void BrowserWindow::initHeaders() {
  // スプライトが既に初期化されているかチェック
  if (_sprHeaderJP.width() > 0 && _sprHeaderEN.width() > 0) return;

  // 日本語ヘッダー作成
  _sprHeaderJP.setPsram(true);
  _sprHeaderJP.createSprite(LCD_W, 26);
  _sprHeaderJP.fillSprite(C_HEADER);

  disp.render.setDrawer(_sprHeaderJP);
  disp.render.setAlignment(Align::TopLeft);
  disp.render.loadFont(fontMain, sizeof(fontMain));
  disp.render.setFontSize(17);
  disp.render.setFontColor(C_LIGHTGRAY, C_HEADER);
  disp.render.setCursor(6, 4);
  disp.render.printf("ファイルブラウザ");
  disp.render.unloadFont();

  // 英語ヘッダー作成
  _sprHeaderEN.setPsram(true);
  _sprHeaderEN.createSprite(LCD_W, 26);
  _sprHeaderEN.fillSprite(C_HEADER);
  //_sprHeaderEN.pushImage(146, 3, CFG_ICON_WIDTH, CFG_ICON_HEIGHT,
  // browserIcon);

  disp.render.setDrawer(_sprHeaderEN);
  disp.render.setAlignment(Align::TopLeft);
  // disp.render.loadFont(nimbusBold, sizeof(nimbusBold));
  disp.render.loadFont(fontMain, sizeof(fontMain));
  disp.render.setFontSize(16);
  disp.render.setFontColor(C_LIGHTGRAY, C_HEADER);
  disp.render.setCursor(6, 4);
  disp.render.printf("File Browser");
  disp.render.unloadFont();
}

void BrowserWindow::draw() {
  xSemaphoreTake(spFrameBuffer, portMAX_DELAY);
  frameBuffer.fillSprite(TFT_WHITE);

  // 事前作成したヘッダー部
  if (ndConfig.get(CFG_LANG) == LANG_JA) {
    _sprHeaderJP.pushSprite(&frameBuffer, 0, 0);
  } else {
    _sprHeaderEN.pushSprite(&frameBuffer, 0, 0);
  }

  // カレントディレクトリ部
  drawCurrentDir();

  // パネル部
  pnlFiles.setItemCount(_browseDirNode->fileCount + _browseDirNode->dirCount +
                        (browserRenderer.hasParentEntry() ? 1 : 0));

  // ブラウザ内の選択ノードを初期選択にする
  Node* selectedNode = _selectedNode;
  if (selectedNode == nullptr || selectedNode->parent != _browseDirNode) {
    selectedNode = (ndFile.currentNode && ndFile.currentNode->parent == _browseDirNode)
                       ? ndFile.currentNode
                       : nullptr;
  }

  Node* workNode = _browseDirNode->firstChild;
  pnlFiles.currentIndex = browserRenderer.hasParentEntry() ? 1 : 0;
  for (int n = 0; n < _browseDirNode->fileCount + _browseDirNode->dirCount; n++) {
    if (workNode == selectedNode) {
      pnlFiles.currentIndex = browserRenderer.hasParentEntry() ? n + 1 : n;
      break;
    }
    workNode = workNode->next;
  }

  pnlFiles.ensureVisible();
  pnlFiles.update(frameBuffer);

  // フッタ部
  drawFooter(true);

  //
  frameBuffer.pushSprite(0, 0);
  xSemaphoreGive(spFrameBuffer);  // 描画完了
}

void BrowserWindow::drawFooterStatic() {
  _sprFooter.fillSprite(TFT_WHITE);
  _sprFooter.fillRoundRect(4, 0, 52, 23, 2, C_FOOTER_ACTIVE);
  _sprFooter.fillRoundRect(124, 0, 42, 23, 2, C_FOOTER_ACTIVE);

  disp.render.setDrawer(_sprFooter);
  disp.render.setFontColor(C_LIGHTGRAY, C_FOOTER_ACTIVE);

  disp.render.setAlignment(Align::TopCenter);
  disp.render.setFontSize(17);
  disp.render.setCursor(4 + 52 / 2, 4);
  if (ndConfig.get(CFG_LANG) == LANG_JA) {
    disp.render.printf("閉じる");
  } else {
    disp.render.printf("Close");
  }
  disp.render.setCursor(124 + 42 / 2, 4);
  disp.render.printf("OK");

  disp.render.setAlignment(Align::TopLeft);
  disp.render.setFontSize(17);
}

void BrowserWindow::drawFooter(bool toFrameBuffer) {
  if (toFrameBuffer) {
    _sprFooter.pushSprite(&frameBuffer, 0, 293);
  } else {
    _sprFooter.pushSprite(&lcd, 0, 293);
  }

  drawFooterState(toFrameBuffer);
}

bool BrowserWindow::isFooterUpEnabled() const {
  Node* selectedNode = browserRenderer.getNodeByDisplayIndex(pnlFiles.currentIndex);
  return (browserRenderer.hasParentEntry() && pnlFiles.currentIndex == 0) ||
         (selectedNode != nullptr && selectedNode->parent != nullptr &&
          selectedNode->parent != fileTree.getRoot());
}

void BrowserWindow::drawFooterState(bool toFrameBuffer) {
  bool up = isFooterUpEnabled();

  if (toFrameBuffer) {
    frameBuffer.fillRoundRect(93, 293, 27, 23, 2, up ? C_FOOTER_ACTIVE : C_FOOTER_INACTIVE);
    if (up) {
      disp.render.setDrawer(frameBuffer);
      disp.render.setFontColor(C_LIGHTGRAY, C_FOOTER_ACTIVE);
      disp.render.setAlignment(Align::TopCenter);
      disp.render.setFontSize(17);
      disp.render.setCursor(93 + 27 / 2, 297);
      disp.render.printf("../");
      disp.render.setAlignment(Align::TopLeft);
      disp.render.setFontSize(17);
    }
  } else {
    lcd.fillRoundRect(93, 293, 27, 23, 2, up ? C_FOOTER_ACTIVE : C_FOOTER_INACTIVE);
    if (up) {
      disp.render.setDrawer(lcd);
      disp.render.setFontColor(C_LIGHTGRAY, C_FOOTER_ACTIVE);
      disp.render.setAlignment(Align::TopCenter);
      disp.render.setFontSize(17);
      disp.render.setCursor(93 + 27 / 2, 297);
      disp.render.printf("../");
      disp.render.setAlignment(Align::TopLeft);
      disp.render.setFontSize(17);
    }
  }
}

int BrowserWindow::getItemCount() const {
  // 親移動項目を含めた表示件数を返す
  if (_browseDirNode == nullptr) return 0;
  return _browseDirNode->fileCount + _browseDirNode->dirCount +
         (browserRenderer.hasParentEntry() ? 1 : 0);
}

void BrowserWindow::selectItem(int index) {
  // 表示インデックスを更新して一覧を再描画する
  int itemCount = getItemCount();
  if (itemCount <= 0) return;

  if (index < 0) index = 0;
  if (index >= itemCount) index = itemCount - 1;

  xSemaphoreTake(spFrameBuffer, portMAX_DELAY);
  pnlFiles.currentIndex = index;
  pnlFiles.ensureVisible();
  pnlFiles.update(frameBuffer);
  drawFooterState(true);
  frameBuffer.pushSprite(0, 0);
  xSemaphoreGive(spFrameBuffer);
}

void BrowserWindow::moveSelection(int delta) {
  // 先頭と末尾でループする選択移動を行う
  int itemCount = getItemCount();
  if (itemCount <= 0) return;

  int nextIndex = pnlFiles.currentIndex + delta;
  if (nextIndex < 0) {
    nextIndex = itemCount - 1;
  } else if (nextIndex >= itemCount) {
    nextIndex = 0;
  }

  selectItem(nextIndex);
}

void BrowserWindow::openDirectory(Node* dirNode, Node* selectedNode) {
  ScopedEncoderDisable encoderGuard;
  _browseDirNode = (dirNode != nullptr) ? dirNode : fileTree.getRoot();
  _selectedNode = selectedNode;
  browserRenderer.setBrowseDirNode(_browseDirNode);
  pnlFiles.resetScroll();
  draw();
}

bool BrowserWindow::openParentDirectory() {
  if (_browseDirNode == nullptr || _browseDirNode == fileTree.getRoot() ||
      _browseDirNode->parent == nullptr) {
    return false;
  }

  openDirectory(_browseDirNode->parent, _browseDirNode);
  return true;
}

void BrowserWindow::onCurrentNodeChanged(Node* prevNode, Node* currentNode) {
  // ブラウザ表示中に再生中項目とその親ディレクトリの灰色表示を更新する
  if (disp.currentView != ViewMode::Browser || _browseDirNode == nullptr) return;

  bool prevInBrowse = prevNode != nullptr && prevNode->parent == _browseDirNode;
  bool currentInBrowse = currentNode != nullptr && currentNode->parent == _browseDirNode;
  Node* prevParent = (prevNode != nullptr) ? prevNode->parent : nullptr;
  Node* currentParent = (currentNode != nullptr) ? currentNode->parent : nullptr;
  bool prevParentInBrowse = prevParent != nullptr && prevParent->parent == _browseDirNode;
  bool currentParentInBrowse = currentParent != nullptr && currentParent->parent == _browseDirNode;

  if (!prevInBrowse && !currentInBrowse && !prevParentInBrowse && !currentParentInBrowse) return;

  if (xSemaphoreTake(spFrameBuffer, 0) != pdTRUE) return;
  pnlFiles.invalidate();  // 全体再描画指示
  pnlFiles.update(frameBuffer);
  frameBuffer.pushSprite(0, 0);
  xSemaphoreGive(spFrameBuffer);
}

// カレントディレクトリ部をフレームバッファに描画します
void BrowserWindow::drawCurrentDir() {
  Node* currentDirNode = _browseDirNode;

  if (_lastCurrentDirNode != currentDirNode) {
    _sprCurrentDir.fillSprite(C_HIGHGRAY);
    _sprCurrentDir.pushImage(2, 5, 16, 16, icoFolderOpen);

    if (currentDirNode != nullptr) {
      disp.render.setDrawer(_sprCurrentDir);
      disp.render.setAlignment(Align::TopLeft);
      disp.render.setFontColor(C_DARK, C_HIGHGRAY);
      disp.render.setCursor(20, 7);
      if (currentDirNode == fileTree.getRoot()) {  // ルート
        disp.render.printf("/");
      } else {
        printWithEllipsis(disp.render, currentDirNode->name, 20, 6, LCD_W - 20);
      }
    }

    _lastCurrentDirNode = currentDirNode;
  }

  _sprCurrentDir.pushSprite(&frameBuffer, 0, 26);
}

void BrowserWindow::close() {
  visible = false;
}
void BrowserWindow::eventHandler(event ev) {
  switch (ev) {
    case event::Up: {
      moveSelection(-1);
      break;
    }
    case event::Down: {
      moveSelection(1);
      break;
    }
    case event::EncClick: {
      pnlFiles.clickCurrentItem();
      break;
    }
    case event::Right: {
      break;
    }
    case event::Menu: {
      this->show();
      break;
    }
    case event::Option: {
      break;
    }
    case event::UpDir: {
      openParentDirectory();
      break;
    }
    case event::Close: {
      this->close();
      if (disp.lastView == ViewMode::Visual) {
        visualWindow.show();
      } else {
        playerWindow.show();
      }
      break;
    }
  }
}

void BrowserWindow::show() {
  ScopedEncoderDisable encoderGuard;
  visible = true;
  disp.currentView = ViewMode::Browser;

  // OpenFontRenderは内部のFreeType状態を全インスタンスで共有するため、
  // 曲切り替え直後のPlayer/Visual用ラベル生成と競合させない。
  lockDrawing();
  browserRenderer.init();
  drawFooterStatic();
  unlockDrawing();

  openDirectory((ndFile.currentNode && ndFile.currentNode->parent != nullptr)
                    ? ndFile.currentNode->parent
                    : fileTree.getRoot(),
                ndFile.currentNode);
}

BrowserWindow browserWindow = BrowserWindow();
