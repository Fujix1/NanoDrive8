/*
  A0 IO18
  A1 IO8

  D0 IO9
  D1 IO10
  D2 IO11
  D3 IO12
  D4 IO13
  D5 IO14
  D6 IO15
  D7 IO16
  IC TCA8418へ

  IO38 CS0
  IO39 CS1
  IO40 WR
  IO43 CS2 (TXD0)
  IO17 CS3 -> 割り込み用333


*/

#ifndef COMMON_H
#define COMMON_H

// Current hardware setup
#define CHIP0 CHIP_YM2151
#define CHIP1 CHIP_OKIM6258
#define CHIP2 CHIP_NONE
#define CHIP3 CHIP_NONE

// どのクロックを使用するか
#define CHIP0_CLOCK CLK_0
#define CHIP1_CLOCK CLK_1
#define CHIP2_CLOCK CLK_NONE
#define CHIP3_CLOCK CLK_NONE

#define USE_YM2203_0
#define USE_YM2203_1
// #define USE_YM2413
#define USE_AY8910
// #define USE_YMF262
#define USE_YM2151
#define USE_OKIM6258

// LCD
#define LCD_W 170
#define LCD_H 320
#define LCD_CLK 2
#define LCD_MOSI 44
#define LCD_RST 42
#define LCD_DC 41

// DISPLAY
#define TITLE_DEVIDER " *** "
#define SCROLL_SPEED_TITLE .7F    // 文字スクロール速度 px
#define SCROLL_SPEED_GAME .5F     // 文字スクロール速度 px
#define SCROLL_SPEED_AUTHOR .35F  // 文字スクロール速度 px
#define SCROLL_DELAY 2000         // スクロール開始までのディレイ ms
#define DISP_TIMER_INTERVAL 20    // ms 表示更新タイマー間隔

#define DISP_UPDATE_TASK_STACK 6144
#define DISP_UPDATE_TASK_PRIORITY 1
#define DISP_UPDATE_TASK_CORE 0

// SD Card
#define SD_CS 7
#define SD_MOSI 6
#define SD_CLK 5
#define SD_MISO 4

#define MAX_PNG_WIDTH 768

#define MAX_MDX_SIZE (256 * 1024)
#define PDX_OFFSET MAX_MDX_SIZE
#define MAX_PDX_SIZE ((1024 + 256) * 1024)
#define PCM_OFFSET (PDX_OFFSET + MAX_PDX_SIZE)
#define MAX_PCM_SIZE (MAX_PDX_SIZE * 4)
#define MAX_FILE_SIZE (PCM_OFFSET + MAX_PCM_SIZE)

// 描画タイマー更新停止 (0: 無効, 1: 有効)
#define ND_DISABLE_DISP_TIMER_UPDATE 0

/**
 * MDXメモリ配置
 * ---------------- 0
 *   MDX DATA 256KB
 * ---------------- PDX_OFFSET
 *   PDX DATA 1.25MB
 * ---------------- PCM_OFFSET
 *  DECODED PCM 5MB
 * ---------------- MAX_FILE_SIZE 6.5MB
 */

// I2C
#define I2C_SDA 16
#define I2C_SCL 15
#define I2C_CLOCK 400000

// GPIO Expander
#define KP_DIV_0 14  // TCA8418_COL6
#define KP_DIV_1 15  // TCA8418_COL7
#define KP_IC 16     // TCA8418_COL8
#define KP_AC 17     // TCA8418_COL9

#endif
