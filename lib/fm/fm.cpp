#include "fm.h"

#include <driver/dedic_gpio.h>

#include "../../include/common.h"
#include "../../include/config.h"
#include "../../include/input.h"
#include "../../include/leds.h"

void setKeyboardYM2151ChannelMask(u8_t mask);

dedic_gpio_bundle_handle_t dataBus = NULL;  // GPIOバンドル用ハンドラ
static SemaphoreHandle_t spGPIO = NULL;     // GPIOアクセス用セマフォ
// クリティカルセクション
portMUX_TYPE gpioMux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE ym2151ChmaskMux = portMUX_INITIALIZER_UNLOCKED;

// YM2151 chマスク
constexpr u8_t YM2151_CHMASK_LED_BASE = 0;
constexpr u8_t YM2151_CHMASK_LED_MAX_BRIGHTNESS = 3;
constexpr u16_t YM2151_CHMASK_LED_OFF_FEEDBACK_MS = 300;
static u32_t ym2151ChmaskLedPulseUntil[8] = {0};

static inline bool isYm2151PanRegister(byte addr) {
  return addr >= 0x20 && addr <= 0x27;
}

static inline byte invertYm2151Pan(byte data) {
  const byte bit6 = (data >> 6) & 0x01;
  const byte bit7 = (data >> 7) & 0x01;
  return (data & 0x3F) | (bit6 << 7) | (bit7 << 6);
}

static inline u8_t getChannelMaskLedBrightness() {
  const int brightness = ndConfig.get(CFG_LED);
  if (brightness <= 0) {
    return 0;
  }
  if (brightness > YM2151_CHMASK_LED_MAX_BRIGHTNESS) {
    return YM2151_CHMASK_LED_MAX_BRIGHTNESS;
  }
  return static_cast<u8_t>(brightness);
}

static bool isChannelMaskLedPulseActive(u8_t ch, u32_t now) {
  return ym2151ChmaskLedPulseUntil[ch] != 0 && (int32_t)(now - ym2151ChmaskLedPulseUntil[ch]) < 0;
}

static void clearChannelMaskLedPulses() {
  for (u8_t ch = 0; ch < 8; ch++) {
    ym2151ChmaskLedPulseUntil[ch] = 0;
  }
}

// ------------------------------------------------------------------------------
// FM音源クラス
//    表記の違い
//    YM**** -> SN76489AN
//    WR -> WE
//    CS -> CE(OE)
//    READY -> No connect

static void syncChannelMaskLeds(u8_t chmask) {
  if (!Leds.ready()) {
    return;
  }

  const u8_t litBrightness = getChannelMaskLedBrightness();
  const u32_t now = millis();
  if (litBrightness != 0) {
    clearChannelMaskLedPulses();
  }

  for (u8_t ch = 0; ch < 8; ch++) {
    const u8_t brightness = (chmask & (u8_t)(1u << ch)) ? 0 : litBrightness;
    Leds.set(YM2151_CHMASK_LED_BASE + ch, brightness);
  }

  if (litBrightness != 0) {
    return;
  }

  for (u8_t ch = 0; ch < 8; ch++) {
    if ((chmask & (u8_t)(1u << ch)) && isChannelMaskLedPulseActive(ch, now)) {
      Leds.set(YM2151_CHMASK_LED_BASE + ch, YM2151_CHMASK_LED_MAX_BRIGHTNESS);
    } else {
      ym2151ChmaskLedPulseUntil[ch] = 0;
    }
  }
}

static void updateChannelMaskLedPulses() {
  if (!Leds.ready() || getChannelMaskLedBrightness() != 0) {
    return;
  }

  const u32_t now = millis();
  for (u8_t ch = 0; ch < 8; ch++) {
    if (ym2151ChmaskLedPulseUntil[ch] == 0) {
      continue;
    }
    if (isChannelMaskLedPulseActive(ch, now)) {
      continue;
    }

    ym2151ChmaskLedPulseUntil[ch] = 0;
    Leds.set(YM2151_CHMASK_LED_BASE + ch, 0);
  }
}

static void pulseChannelMaskLed(u8_t ch) {
  if (!Leds.ready() || ch >= 8 || getChannelMaskLedBrightness() != 0) {
    return;
  }

  ym2151ChmaskLedPulseUntil[ch] = millis() + YM2151_CHMASK_LED_OFF_FEEDBACK_MS;
  Leds.set(YM2151_CHMASK_LED_BASE + ch, YM2151_CHMASK_LED_MAX_BRIGHTNESS);
}

// 初期化
// FMChip::FMChip() : _io(PCA95XX_PCA9537) {}

void FMChip::begin() {
  // GPIO 用セマフォ作成
  spGPIO = xSemaphoreCreateBinary();
  xSemaphoreGive(spGPIO);

  // データバス用 GPIO バンドル
  const int bundleA_gpios[] = {D0, D1, D2, D3, D4, D5, D6, D7};
  gpio_config_t io_conf = {
      .mode = GPIO_MODE_OUTPUT,
  };
  for (int i = 0; i < sizeof(bundleA_gpios) / sizeof(bundleA_gpios[0]); i++) {
    io_conf.pin_bit_mask = 1ULL << bundleA_gpios[i];
    gpio_config(&io_conf);
  }

  // gpio バンドル decic config
  dedic_gpio_bundle_config_t bundle_config = {
      .gpio_array = bundleA_gpios,
      .array_size = sizeof(bundleA_gpios) / sizeof(bundleA_gpios[0]),
      .flags =
          {
              .out_en = 1,
          },
  };
  ESP_ERROR_CHECK(dedic_gpio_new_bundle(&bundle_config, &dataBus));

  // その他の GPIO
  pinMode(WR, OUTPUT);
  pinMode(CS0, OUTPUT);
  pinMode(CS1, OUTPUT);
  // pinMode(CS2, OUTPUT);

  pinMode(A0, OUTPUT);
  pinMode(A1, OUTPUT);
  // pinMode(IC, OUTPUT);

  WR_HIGH;
  A0_LOW;
  // A1_LOW;
  // IC_LOW;

  // IC,AC は GPIO エクスパンダで処理
  input.digitalWrite(KP_IC, HIGH);
  input.digitalWrite(KP_AC, LOW);

  CS0_HIGH;
  CS1_HIGH;
  // CS2_HIGH;

  syncChannelMaskLeds(ym2151_chmask);
}

void FMChip::reset() {
  CS0_LOW;
  CS1_LOW;
  // CS2_LOW;
  WR_HIGH;
  A0_LOW;
  // A1_LOW;
  //  IC_LOW;
  //   _io.digitalWrite(2, LOW);
  //   _io.digitalWrite(3, HIGH);
  input.digitalWrite(KP_IC, LOW);
  input.digitalWrite(KP_AC, HIGH);

  ets_delay_us(16);

  //  _io.digitalWrite(2, HIGH);
  //  _io.digitalWrite(3, LOW);
  input.digitalWrite(KP_IC, HIGH);
  input.digitalWrite(KP_AC, LOW);

  // IC_HIGH;

  CS0_HIGH;
  CS1_HIGH;
  // CS2_HIGH;

  _currentOKIDiv = OKIM6258_DIV_1024;
  setOKIM6258divider(OKIM6258_DIV_512);
  ets_delay_us(16);

  // CS2_HIGH;

  //_psgFrqLowByte = 0;
  for (u16_t i = 0; i < sizeof(ym2151_reg); i++) {
    ym2151_reg[i] = 0;
  }
  for (u8_t i = 0; i < sizeof(ym2151_iskeyOn); i++) {
    ym2151_iskeyOn[i] = false;
  }
  for (u8_t i = 0; i < sizeof(ym2151_keyOnSlots); i++) {
    ym2151_keyOnSlots[i] = 0;
  }

  // D0-D7 を LOW にしておく
  dedic_gpio_bundle_write(dataBus, 0xff, 0x00);
}

// SN76489
void FMChip::write(byte data, byte chipno, si5351Freq_t freq) {
  //

  if ((data & 0x90) == 0x80 && (data & 0x60) >> 5 != 3) {
    // Low byte 周波数 0x8n, 0xan, 0xcn
    _psgFrqLowByte = data;

  } else if ((data & 0x80) == 0) {  // High byte
    if ((_psgFrqLowByte & 0x0F) == 0) {
      if ((data & 0x3F) == 0) _psgFrqLowByte |= 1;
    }
    writeRaw(_psgFrqLowByte, chipno, freq);
    writeRaw(data, chipno, freq);

  } else {
    writeRaw(data, chipno, freq);
  }
}

void FMChip::writeRaw(byte data, byte chipno, si5351Freq_t freq) {
  switch (chipno) {
    case 0:
      CS0_LOW;
      break;
    case 1:
      CS1_LOW;
      break;
    case 2:
      // CS2_LOW;
      break;
  }
  WR_HIGH;
  dedic_gpio_bundle_write(dataBus, 0xff, data);

  // コントロールレジスタに登録するには WR_LOW → WR_HIGH 最低32クロック
  // 4MHz     :　0.25us   * 32 = 8 us
  // 3.579MHz :  0.2794us * 32 = 8.94 us
  // 1.5MHz   :  0.66us   * 32 = 21.3 us
  WR_LOW;

  ets_delay_us((32000000 / freq) + 1);

  WR_HIGH;
  switch (chipno) {
    case 0:
      CS0_HIGH;
      break;
    case 1:
      CS1_HIGH;
      break;
    case 2:
      // CS2_HIGH;
      break;
  }
}

byte lastAddr = 0;
void FMChip::setYM2612(byte bank, byte addr, byte data, u8_t chipno) {
  switch (chipno) {
    case 0:
      CS0_LOW;
      break;
    case 1:
      CS1_LOW;
    case 2:
      // CS2_LOW;
      break;
  }

  if (bank == 1) {
    A1_HIGH;
  } else {
    A1_LOW;
  }

  lastAddr = addr;

  // Address
  A0_LOW;
  dedic_gpio_bundle_write(dataBus, 0xff, addr);
  WR_LOW;
  WR_HIGH;
  A0_HIGH;

  // アドレスライト後の待ちサイクル
  // アドレス＄21-＄B6 待ちサイクル 17 = 2.21us
  ets_delay_us(5);  // 3 は一部足りない

  // data
  dedic_gpio_bundle_write(dataBus, 0xff, data);

  WR_LOW;
  WR_HIGH;
  switch (chipno) {
    case 0:
      CS0_HIGH;
      break;
    case 1:
      CS1_HIGH;
      break;
    case 2:
      // CS2_HIGH;
      break;
  }

  if (bank == 1) {
    A1_LOW;
  }
  // unsigned long deltaTime = micros() - startTime;
  // Serial.printf("%x%d\n", addr, deltaTime);
  if (addr == 0x2a) {
  } else if (addr >= 0x21 && addr <= 0x9e) {
    ets_delay_us(12);  // 83 cycles = 10.79us,
  } else if (addr >= 0xa0 && addr <= 0xb6) {
    ets_delay_us(8);  // 47 cycles = 6.11us
  }

  // YM3438 Twww マニュアルより
  // WR_LOW -> WR_HIGH: Tww 200 ns
  // Dn -> WR_HIGH: Twds 100 ns

  // データ-アドレスライト間, データデータ間 ($21 - $9E) 83サイクル = 10.79 us
  // データ-アドレスライト間  データデータ間 ($A0 - $B6) 47サイクル = 6.11 us
}

// YM2612 の DAC データ送信専用
void FMChip::setYM2612DAC(byte data, u8_t chipno) {
  switch (chipno) {
    case 0:
      CS0_LOW;
      break;
    case 1:
      CS1_LOW;
      break;
  }

  if (lastAddr != 0x2a) {
    lastAddr = 0x2a;
    // Address
    A0_LOW;
    dedic_gpio_bundle_write(dataBus, 0xff, 0x2a);
    WR_LOW;
    WR_HIGH;
    A0_HIGH;
    // アドレスライト後の待ちサイクル
    // アドレス＄21-＄B6 待ちサイクル 17 = 2.21us
    ets_delay_us(4);
  }

  // data
  dedic_gpio_bundle_write(dataBus, 0xff, data);
  WR_LOW;
  WR_HIGH;
  switch (chipno) {
    case 0:
      CS0_HIGH;
      break;
    case 1:
      CS1_HIGH;
      break;
  }
}

// 汎用レジスタ設定：YM2203, AY-8910, YM2413
void FMChip::setRegister(byte addr, byte data, int chipno = 0) {
  // Address
  dedic_gpio_bundle_write(dataBus, 0xff, addr);
  A0_LOW;  // 375ns
  switch (chipno) {
    case 0:
      CS0_LOW;
      break;
    case 1:
      CS1_LOW;
      break;
    case 2:
      // CS2_LOW;
      break;
  }
  ets_delay_us(2);
  WR_LOW;
  ets_delay_us(2);
  WR_HIGH;
  A0_HIGH;

  ets_delay_us(3);

  // data
  dedic_gpio_bundle_write(dataBus, 0xff, data);
  ets_delay_us(2);
  WR_LOW;
  ets_delay_us(2);
  WR_HIGH;
  switch (chipno) {
    case 0:
      CS0_HIGH;
      break;
    case 1:
      CS1_HIGH;
      break;
    case 2:
      // CS2_HIGH;
      break;
  }
  ets_delay_us(19);  // 最低17
}

// YM2151用レジスタ設定
void FMChip::setRegisterOPM(byte addr, byte data, u8_t chipno, boolean temp) {
  (void)chipno;

  // temp=true のときはキャッシュ済みの実レジスタ値を一時送信する。
  byte writeData = data;
  if (isYm2151PanRegister(addr) && !temp && ndConfig.get(CFG_YM2151_PAN) == TPAN_INVERT) {
    writeData = invertYm2151Pan(writeData);
  }

  // chマスクでパンをオーバーライド
  if (!temp) {
    ym2151_reg[addr] = writeData;
  }

  if (isYm2151PanRegister(addr) && (ym2151_chmask & (u8_t)(1u << (addr & 0x07)))) {
    writeData &= 0x3F;
  }

  portENTER_CRITICAL(&gpioMux);
  dedic_gpio_bundle_write(dataBus, 0xff, addr);
  A0_LOW;
  CS0_LOW;
  WR_LOW;
  WR_HIGH;
  A0_HIGH;
  ets_delay_us(4);  // 11サイクル, 3us @ 3.57MHz, 2.75us @ 4MHz
  // data
  dedic_gpio_bundle_write(dataBus, 0xff, writeData);
  WR_LOW;
  WR_HIGH;
  CS0_HIGH;
  portEXIT_CRITICAL(&gpioMux);
  ets_delay_us(21);  // 68サイクル, 19us @ 3.57MHz, 17us @ 4MHz
}

void FMChip::setRegisterOPL3(byte port, byte addr, byte data, int chipno) {
  A0_LOW;
  if (port == 1) {
    A1_HIGH;
  } else {
    A1_LOW;
  }
  dedic_gpio_bundle_write(dataBus, 0xff, addr);
  switch (chipno) {
    case 0:
      CS0_LOW;
      break;
    case 1:
      CS1_LOW;
      break;
    case 2:
      // CS2_LOW;
      break;
  }
  // Address
  WR_LOW;
  ets_delay_us(3);
  WR_HIGH;

  ets_delay_us(5);
  // 32 clocks after writing address and data
  // 14.318180 MHz: 69.84 ns / cycle  x 32 = 2,234.88 ns = 2.235 us
  // 8.000000 MHz: 125 ns / cycle x 32 = 4,000 ns = 4 us

  // data
  A0_HIGH;
  dedic_gpio_bundle_write(dataBus, 0xff, data);
  WR_LOW;
  ets_delay_us(3);
  WR_HIGH;

  switch (chipno) {
    case 0:
      CS0_HIGH;
      break;
    case 1:
      CS1_HIGH;
      break;
    case 2:
      // CS2_HIGH;
      break;
  }
  ets_delay_us(7);
}

// OKI M6258のコマンド送信
void FMChip::setOKIM6258command(byte data, int chipno) {
  // data
  // 0b0001: SP: 再生停止
  // 0b0010: PLAY ST: 再生開始
  // 0b0100: REC ST: 録音開始
  // D/~C: High: ADPCM データ入力 (A0 HIGH)
  //       Low: コマンドデータ入力 (A0 LOW)
  //
  portENTER_CRITICAL(&gpioMux);
  CS1_LOW;
  A0_LOW;
  dedic_gpio_bundle_write(dataBus, 0xff, data);
  WR_LOW;
  ets_delay_us(1);
  WR_HIGH;
  A0_HIGH;
  CS1_HIGH;
  portEXIT_CRITICAL(&gpioMux);
}

// OKI M6258のデータ送信
void FMChip::setOKIM6258data(byte data, int chipno) {
  // data
  // D/~C: High: ADPCM データ入力 (A0 HIGH)
  //       Low: コマンドデータ入力 (A0 LOW)
  //

  portENTER_CRITICAL(&gpioMux);
  CS1_LOW;
  A0_HIGH;
  dedic_gpio_bundle_write(dataBus, 0xff, data);
  WR_LOW;
  // tWW = 250ns @ 4.096MHz
  //       128ns @ 8 MHz
  // ets_delay_us(1);
  // nowait は　0 - 200ns
  asm volatile("nop; nop; nop; nop; nop; nop; nop; nop;");  // 4.17ns * 8
  asm volatile("nop; nop; nop; nop; nop; nop; nop; nop;");  // 4.17ns * 8
  asm volatile("nop; nop; nop; nop; nop; nop; nop; nop;");  // 4.17ns * 8
  asm volatile("nop; nop; nop; nop; nop; nop; nop; nop;");  // 4.17ns * 8
  asm volatile("nop; nop; nop; nop; nop; nop; nop; nop;");  // 4.17ns * 8

  WR_HIGH;
  CS1_HIGH;
  portEXIT_CRITICAL(&gpioMux);
}

// 割り込みから OKI M6258 に送信
void IRAM_ATTR FMChip::setOKIM6258dataISR(byte data, int chipno) {
  (void)chipno;
  portENTER_CRITICAL_ISR(&gpioMux);
  CS1_LOW;
  A0_HIGH;
  dedic_gpio_bundle_write(dataBus, 0xff, data);
  WR_LOW;
  // tWW = 250ns @ 4.096MHz
  ets_delay_us(1);
  // nowait は　0 - 200ns

  WR_HIGH;
  CS1_HIGH;
  portEXIT_CRITICAL_ISR(&gpioMux);
}

void FMChip::setOKIM6258divider(tOKIM6258Divider div) {
  if (_currentOKIDiv != div) {
    // Serial.printf("setOKIM6258divider: %d\n", div);
    _currentOKIDiv = div;
    switch (div) {
      case OKIM6258_DIV_1024:
        //_io.write(0, LOW);
        //_io.write(1, LOW);

        input.digitalWrite(KP_DIV_0, LOW);
        input.digitalWrite(KP_DIV_1, LOW);

        break;
      case OKIM6258_DIV_768:
        //_io.write(0, HIGH);
        //_io.write(1, LOW);

        input.digitalWrite(KP_DIV_0, HIGH);
        input.digitalWrite(KP_DIV_1, LOW);

        break;
      case OKIM6258_DIV_512:
        //_io.write(0, LOW);
        //_io.write(1, HIGH);

        input.digitalWrite(KP_DIV_0, LOW);
        input.digitalWrite(KP_DIV_1, HIGH);
        break;
    }
  }
}

void FMChip::requestToggleChannelMask(u8_t ch) {
  if (ch >= 8) {
    return;
  }

  portENTER_CRITICAL(&ym2151ChmaskMux);
  _pendingYm2151ChToggle ^= (u8_t)(1u << ch);
  portEXIT_CRITICAL(&ym2151ChmaskMux);
}

void FMChip::applyPendingChannelMask() {
  updateChannelMaskLedPulses();

  u8_t pending = 0x00;

  portENTER_CRITICAL(&ym2151ChmaskMux);
  pending = _pendingYm2151ChToggle;
  _pendingYm2151ChToggle = 0x00;
  portEXIT_CRITICAL(&ym2151ChmaskMux);

  if (pending == 0x00) {
    return;
  }

  for (u8_t ch = 0; ch < 8; ch++) {
    if (pending & (u8_t)(1u << ch)) {
      toggleChannelMask(ch);
    }
  }
}

// chマスクはパンの両方ミュートで対応
void FMChip::toggleChannelMask(u8_t ch) {
  // Serial.printf("ch=%d\n", ch);
  ym2151_chmask ^= (u8_t)(1u << ch);
  const bool masked = ((ym2151_chmask >> ch) & 0x1) != 0;
  if (!masked) {
    ym2151ChmaskLedPulseUntil[ch] = 0;
  }
  syncChannelMaskLeds(ym2151_chmask);
  if (masked) {
    pulseChannelMaskLed(ch);
  }
  setKeyboardYM2151ChannelMask(ym2151_chmask);
  // Serial.printf("chmask=%x\n", ym2151_chmask);
  const byte addr = 0x20 + ch;
  const byte data = masked ? (ym2151_reg[addr] & 0x3F) : ym2151_reg[addr];

  setRegisterOPM(addr, data, 0, true);
  // Serial.printf("addr: %x, dat: %x\n", addr, data);
}

void FMChip::refreshChannelMaskLeds() {
  syncChannelMaskLeds(ym2151_chmask);
}

FMChip FM;
