#ifndef INPUT_H
#define INPUT_H
#include <Arduino.h>

// ----------
// #define INPUT_PIN 1 GPIO1 未使用

#define INPUT_CAPTURE_INTERVAL 60  // ms キャプチャインターバル
#define INPUT_REPEAT_DELAY 400     // ms リピート開始までの時間

#define TCA8418_IRQ_PIN 1

typedef enum {
  btnNONE = -1,
  btn00 = 00,
  btn10 = 10,
  btn20 = 20,
  btn60 = 60,
} Button;

enum class event {
  None,
  Right,
  Up,
  Down,
  Left,
  Option,
  Close,
  Menu,
  EncClick,
  UpDir,
};

extern QueueHandle_t xQueueInput;  // 入力のキュー
extern TaskHandle_t tskEventLoop;  // イベントループタスク
void cancelPlayHoldCountdown();
bool isPlayHoldCountdownActive();
void syncPlayHoldConfig();

class Input {
 public:
  Input();
  bool init();
  void inputHandler();
  void setEnabled(bool state);
  bool isEnabled() const;
  void setEncoderEnabled(bool state);
  bool isEncoderEnabled() const;
  Button inputBuffer = btnNONE;
  bool digitalWrite(u8_t pinnum, u8_t level);

 private:
  volatile bool _enabled = false;
};

extern Input input;

#endif
