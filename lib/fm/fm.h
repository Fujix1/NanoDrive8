#ifndef FM_H
#define FM_H
#include <Arduino.h>
#include <soc/gpio_reg.h>

#include <vector>

#include "../../include/util.h"
#include "SI5351_types.hpp"

// GPIO Assignment
#define D0 10
#define D1 11
#define D2 12
#define D3 13
#define D4 14
#define D5 21
#define D6 47
#define D7 48

#define A0 18
#define A1 8
#define WR 40
// #define CS0 38

// #define CS1 39
#define CS0 9
#define CS1 38
#define CS2 42
#define MCK 17

// GPIO 0～31用のマクロ
#define A0_HIGH (REG_WRITE(GPIO_OUT_W1TS_REG, (1 << A0)))
#define A0_LOW (REG_WRITE(GPIO_OUT_W1TC_REG, (1 << A0)))
#define A1_HIGH (REG_WRITE(GPIO_OUT_W1TS_REG, (1 << A1)))
#define A1_LOW (REG_WRITE(GPIO_OUT_W1TC_REG, (1 << A1)))
#define CS0_HIGH (REG_WRITE(GPIO_OUT_W1TS_REG, (1 << CS0)))
#define CS0_LOW (REG_WRITE(GPIO_OUT_W1TC_REG, (1 << CS0)))

// GPIO 32～48用のマクロ（ビット位置は32を引いた値を使用）
#define WR_HIGH (REG_WRITE(GPIO_OUT1_W1TS_REG, (1 << (WR - 32))))
#define WR_LOW (REG_WRITE(GPIO_OUT1_W1TC_REG, (1 << (WR - 32))))
#define CS1_HIGH (REG_WRITE(GPIO_OUT1_W1TS_REG, (1 << (CS1 - 32))))
#define CS1_LOW (REG_WRITE(GPIO_OUT1_W1TC_REG, (1 << (CS1 - 32))))
#define CS2_HIGH (REG_WRITE(GPIO_OUT1_W1TS_REG, (1 << (CS2 - 32))))
#define CS2_LOW (REG_WRITE(GPIO_OUT1_W1TC_REG, (1 << (CS2 - 32))))

class FMChip {
 public:
  // FMChip();
  void begin();
  void reset();
  void setRegister(byte addr, byte value, int chipno);
  void setRegisterOPM(byte addr, byte value, u8_t chipno, boolean temp = false);
  void setRegisterOPL3(byte port, byte addr, byte data, int chipno);
  void setYM2612(byte port, byte addr, byte data, u8_t chipno);
  void setYM2612DAC(byte data, u8_t chipno);
  void write(byte data, byte chipno, si5351Freq_t freq);
  void writeRaw(byte data, byte chipno, si5351Freq_t freq);
  void setOKIM6258command(byte data, int chipno);
  void setOKIM6258data(byte data, int chipno);
  void setOKIM6258dataISR(byte data, int chipno);
  void setOKIM6258divider(tOKIM6258Divider);

  void requestToggleChannelMask(u8_t ch);
  void applyPendingChannelMask();
  void toggleChannelMask(u8_t ch);
  void refreshChannelMaskLeds();

  u8_t ym2151_reg[256] = {0};
  bool ym2151_iskeyOn[8] = {false};
  u8_t ym2151_keyOnSlots[8] = {0};
  u8_t ym2151_chmask = 0x00;

 private:
  u8_t _psgFrqLowByte = 0;
  tOKIM6258Divider _currentOKIDiv = OKIM6258_DIV_512;
  volatile u8_t _pendingYm2151ChToggle = 0x00;
};

extern FMChip FM;

#endif
