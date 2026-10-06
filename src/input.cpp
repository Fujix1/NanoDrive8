#include "input.h"

#include <Adafruit_TCA8418.h>
#include <driver/gpio.h>

#include "disp.h"
#include "file.h"
#include "mdx.h"
#include "nd.h"
#include "vgm.h"

static TimerHandle_t keyRepeatTimer = nullptr;
enum class KeyRepeatState : u8_t { Idle, Waiting, Repeating };
static volatile KeyRepeatState keyRepeatState = KeyRepeatState::Idle;
static volatile int activeKey = -1;
static event queuedEvents[2];
static bool holdCountdownActive = false;
static int8_t holdCountdownSec = 0;
static uint32_t holdCountdownNextTick = 0;
static int lastPauseConfig = -1;

// ロータリーエンコーダー入力ピン。
static constexpr u8_t kEncoderPinA = 43;
static constexpr u8_t kEncoderPinB = 39;

// 前回の AB 状態。
static u8_t encoderPrevState = 0;
static volatile bool encoderEnabled = true;
static volatile bool encoderStateInitialized = false;
static volatile bool encoderDirty = false;

// 1クリック=4遷移に加え、チャタリング分も保持できるよう十分な余裕を持たせる。
static constexpr u8_t kEncoderBufferSize = 64;
static constexpr u8_t kEncoderBufferMask = kEncoderBufferSize - 1;
// 1ノッチは通常4遷移。ISRが1遷移だけ取り逃した場合も通常操作として受理する。
static constexpr int8_t kEncoderMinTicksPerStep = 3;
static volatile u8_t encoderBuffer[kEncoderBufferSize];
static volatile u8_t encoderBufferHead = 0;
static volatile u8_t encoderBufferTail = 0;
static volatile u8_t encoderIsrLastState = 0;
static volatile u16_t encoderOverflowCount = 0;
static int8_t encoderStepAccumulator = 0;
static int8_t encoderAlternateAccumulator = 0;
static bool encoderHasAlternatePath = false;

// TCA8418
Adafruit_TCA8418 keypad;
TaskHandle_t tcaTaskHandle = nullptr;
void onKeyUp(int keyIndex);
void onKeyDown(int keyIndex);
void sendEventToQueue(event ev);

// ホールド表示時刻を、現在表示中のウィンドウへ即時反映する。
static void drawHoldTimestamp(int64_t sec) {
  playerWindow.dispData.time = sec;
  if (disp.currentView == ViewMode::Player) {
    if (sec == 0) {
      // カウントダウン完了の 0:00 だけは、フレームバッファ競合で描画を捨てない。
      playerWindow.updateHeaderBlocking(sec);
    } else {
      playerWindow.updateHeader(sec);
    }
  } else if (disp.currentView == ViewMode::Visual) {
    visualWindow.drawTimestamp(sec);
  }
}

// 新しい再生リクエストや設定変更で、進行中の3秒カウントダウンだけを止める。
void cancelPlayHoldCountdown() {
  holdCountdownActive = false;
}

bool isPlayHoldCountdownActive() {
  return holdCountdownActive;
}

// ホールドを解除し、0:00から実再生が始まるよう各フォーマットの時刻基準を戻す。
static void finishPlayHold() {
  holdCountdownActive = false;
  drawHoldTimestamp(0);
  nju72342.unmute();
  if (ND::fileFormat == FileFormat::VGM || ND::fileFormat == FileFormat::VGZ) {
    vgm.startTick = micros64() + 1000;
  }
#ifdef USE_MDX
  else if (ND::fileFormat == FileFormat::MDX) {
    MDX._startTick = millis();
  }
#endif
  ND::isPaused = false;
}

// CFG_PAUSE変更を、再生中のホールド状態へ即時反映する。
void syncPlayHoldConfig() {
  const int pauseConfig = ndConfig.get(CFG_PAUSE);
  if (pauseConfig == lastPauseConfig) return;

  lastPauseConfig = pauseConfig;
  if (!ND::isPaused) {
    cancelPlayHoldCountdown();
    return;
  }

  switch (pauseConfig) {
    case HOLD_NONE:
      finishPlayHold();
      break;
    case HOLD_3SEC:
      cancelPlayHoldCountdown();
      drawHoldTimestamp(-3);
      break;
    case HOLD_YES:
    default:
      cancelPlayHoldCountdown();
      drawHoldTimestamp(0);
      break;
  }
}

// 3秒ホールドのカウントダウンを非ブロッキングで進める。
static void updateHoldCountdown() {
  if (!holdCountdownActive) return;

  if (!ND::isPaused) {
    cancelPlayHoldCountdown();
    return;
  }

  if (static_cast<int32_t>(millis() - holdCountdownNextTick) < 0) return;

  holdCountdownSec++;
  holdCountdownNextTick += 1000;
  if (holdCountdownSec < 0) {
    drawHoldTimestamp(holdCountdownSec);
    return;
  }

  finishPlayHold();
}

static bool releasePlayHold() {
  if (!ND::isPaused) return false;

  if (ndConfig.get(CFG_PAUSE) == HOLD_3SEC) {
    if (!holdCountdownActive) {
      // 入力処理を止めず、次回以降の inputHandler() で -2, -1, 0 へ進める。
      holdCountdownActive = true;
      holdCountdownSec = -3;
      holdCountdownNextTick = millis() + 1000;
      drawHoldTimestamp(holdCountdownSec);
    }
    return true;
  }

  finishPlayHold();
  return true;
}

// 現在の AB 相を 2 bit 状態にまとめて返す。
static inline u8_t IRAM_ATTR readEncoderState() {
  const u8_t a = gpio_get_level(static_cast<gpio_num_t>(kEncoderPinA)) ? 1 : 0;
  const u8_t b = gpio_get_level(static_cast<gpio_num_t>(kEncoderPinB)) ? 1 : 0;
  return static_cast<u8_t>((a << 1) | b);
}

// エンコーダー入力変化通知用 ISR。
void IRAM_ATTR encoderGpioIrq() {
  if (!encoderEnabled) return;
  if (!encoderStateInitialized) return;

  const u8_t state = readEncoderState();
  if (state == encoderIsrLastState) return;

  const u8_t nextHead = (encoderBufferHead + 1) & kEncoderBufferMask;
  if (nextHead == encoderBufferTail) {
    encoderBufferTail = (encoderBufferTail + 1) & kEncoderBufferMask;
    encoderOverflowCount++;
  }

  encoderBuffer[encoderBufferHead] = state;
  encoderBufferHead = nextHead;
  encoderIsrLastState = state;
  encoderDirty = true;
}

// 積算済みの移動量を破棄して現在のAB状態を基準にし直す。
static void resetEncoderState() {
  encoderStateInitialized = false;
  const u8_t state = readEncoderState();
  encoderPrevState = state;
  encoderIsrLastState = state;
  encoderBufferHead = 0;
  encoderBufferTail = 0;
  encoderOverflowCount = 0;
  encoderStepAccumulator = 0;
  encoderAlternateAccumulator = 0;
  encoderHasAlternatePath = false;
  encoderStateInitialized = true;
  encoderDirty = false;
}

// 回転方向を現在の画面に応じた入力へ変換する。
static void dispatchEncoderStep(int8_t direction) {
  if (direction == 0) return;

  // 曲ロードや画面更新中の回転は蓄積しない。処理完了後に現在相へ再同期する。
  const bool restoreEncoder = encoderEnabled;
  encoderEnabled = false;
  encoderStateInitialized = false;
  encoderBufferHead = 0;
  encoderBufferTail = 0;
  encoderOverflowCount = 0;
  encoderStepAccumulator = 0;
  encoderAlternateAccumulator = 0;
  encoderHasAlternatePath = false;
  encoderDirty = false;

  switch (disp.currentView) {
    case ViewMode::Player:
    case ViewMode::Visual:
      ndFile.filePlay(direction > 0 ? 1 : -1);
      break;
    case ViewMode::Config:
    case ViewMode::Browser:
      sendEventToQueue(direction > 0 ? event::Down : event::Up);
      break;
    default:
      break;
  }

  if (restoreEncoder) {
    resetEncoderState();
    encoderEnabled = true;
  }
}

// AB 相の状態遷移から回転方向を取り出す。
static void pollEncoder() {
  if (!encoderStateInitialized) {
    resetEncoderState();
    return;
  }

  static constexpr int8_t transitionTable[16] = {
      0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0,
  };

  int8_t latestDirection = 0;

  // 先に通知を下ろすことで、走査終了直後にISRが追加したデータのdirtyを消さない。
  encoderDirty = false;

  if (encoderOverflowCount != 0) {
    // 途中の遷移が失われているため、残った断片から方向を推測せず現在のAB相へ再同期する。
    resetEncoderState();
    return;
  }

  while (encoderBufferTail != encoderBufferHead) {
    const u8_t tail = encoderBufferTail;
    const u8_t currentState = encoderBuffer[tail];
    encoderBufferTail = (encoderBufferTail + 1) & kEncoderBufferMask;

    const int8_t transition =
        transitionTable[((encoderPrevState & 0b11) << 2) | (currentState & 0b11)];

    if (transition == 0 && currentState == encoderPrevState) {
      continue;
    }

    if (transition == 0) {
      // A/B同時変化は、途中の1状態をISRが拾えなかった2遷移として扱う。
      // ±2の両候補を保持し、続く遷移で一方だけが確定閾値へ達した場合に採用する。
      // 1クリック内で2回曖昧になった場合は方向を一意に決められないため破棄する。
      if (encoderHasAlternatePath) {
        encoderStepAccumulator = 0;
        encoderAlternateAccumulator = 0;
        encoderHasAlternatePath = false;
      } else {
        encoderAlternateAccumulator = encoderStepAccumulator - 2;
        encoderStepAccumulator += 2;
        encoderHasAlternatePath = true;
      }
      encoderPrevState = currentState;
      continue;
    }

    encoderStepAccumulator += transition;
    if (encoderHasAlternatePath) {
      encoderAlternateAccumulator += transition;
    }
    encoderPrevState = currentState;

    const bool primaryPositive = encoderStepAccumulator <= -kEncoderMinTicksPerStep;
    const bool primaryNegative = encoderStepAccumulator >= kEncoderMinTicksPerStep;
    const bool alternatePositive =
        encoderHasAlternatePath && encoderAlternateAccumulator <= -kEncoderMinTicksPerStep;
    const bool alternateNegative =
        encoderHasAlternatePath && encoderAlternateAccumulator >= kEncoderMinTicksPerStep;

    if ((primaryPositive || alternatePositive) && !(primaryNegative || alternateNegative)) {
      latestDirection = 1;
    } else if ((primaryNegative || alternateNegative) && !(primaryPositive || alternatePositive)) {
      latestDirection = -1;
    }

    if (latestDirection != 0) {
      encoderStepAccumulator = 0;
      encoderAlternateAccumulator = 0;
      encoderHasAlternatePath = false;
    }

    if (latestDirection != 0) {
      // 1ノッチ確定後は残りの遷移を解釈しない。dispatchEncoderStep()で
      // ISRバッファごと現在のAB相へ再同期し、1操作から複数判定されるのを防ぐ。
      break;
    }
  }

  dispatchEncoderStep(latestDirection);
}

static event getRepeatEventForKey(Button key) {
  switch (disp.currentView) {
    case ViewMode::Config:
      switch (key) {
        case btn00:
          return event::Up;
        case btn10:
          return event::Down;
        case btn20:
          return event::Left;
        default:
          return event::None;
      }
    case ViewMode::Browser:
      switch (key) {
        case btn00:
          return event::Up;
        case btn10:
          return event::Down;
        case btn20:
          return event::Left;
        default:
          return event::None;
      }
    default:
      return event::None;
  }
}

static void discardQueuedEvent(event discardEvent) {
  if (xQueueInput == nullptr || discardEvent == event::None) return;

  int keepCount = 0;
  event queuedEvent;
  while (xQueueReceive(xQueueInput, &queuedEvent, 0) == pdTRUE) {
    if (queuedEvent != discardEvent && keepCount < 2) {
      queuedEvents[keepCount++] = queuedEvent;
    }
  }

  for (int i = 0; i < keepCount; i++) {
    xQueueSend(xQueueInput, &queuedEvents[i], 0);
  }
}

static bool isKeyRepeatActive() {
  return activeKey >= 0 || keyRepeatState != KeyRepeatState::Idle;
}

static bool shouldIgnoreEncoderButtonPress(int key) {
  return key == btn60 && isKeyRepeatActive();
}

// キーリピートタイマー処理
void postRepeatKey(int key) {
  //
  // Serial.printf("キーリピート: %d\n", key);
  input.inputBuffer = (Button)key;
}

// キーリピートタイマーハンドラ
static void hKeyRepeat(TimerHandle_t) {
  if (activeKey < 0) return;

  if (keyRepeatState == KeyRepeatState::Waiting) {
    postRepeatKey(activeKey);

    // リピ開始
    keyRepeatState = KeyRepeatState::Repeating;
    xTimerChangePeriod(keyRepeatTimer, pdMS_TO_TICKS(INPUT_CAPTURE_INTERVAL), 0);
    return;
  }

  if (keyRepeatState == KeyRepeatState::Repeating) {
    postRepeatKey(activeKey);
  }
}

// キー入力のロックとアンロック
static void tcaSetLock(bool lock) {
  u8_t lockEc = keypad.readRegister(TCA8418_REG_KEY_LCK_EC);
  if (lock) {
    lockEc |= TCA8418_REG_LCK_EC_K_LCK_EN;
  } else {
    lockEc &= ~TCA8418_REG_LCK_EC_K_LCK_EN;
  }
  keypad.writeRegister(TCA8418_REG_KEY_LCK_EC, lockEc);
}

// TCA8418 割り込み
void TCA8418_irq() {
  if (tcaTaskHandle == nullptr) return;
  BaseType_t hpTaskWoken = pdFALSE;
  vTaskNotifyGiveFromISR(tcaTaskHandle, &hpTaskWoken);
  if (hpTaskWoken) portYIELD_FROM_ISR();
}

// TCA8418 割り込み実処理
void tcaTask(void* arg) {
  tcaTaskHandle = xTaskGetCurrentTaskHandle();
  while (1) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // 割り込みくるまで待機

    while (1) {
      int k = keypad.getEvent();
      if (k == 0) {
        break;
      }
      if (!input.isEnabled()) {
        continue;
      }

      int key = (k & 0x7F) - 1;

      if ((key & 0x01) == 0) {  // カラム0
                                // Serial.printf("key: %d\n", key);
        if (key == btn00 || key == btn10 || key == btn20) {
          if (k & 0x80) {
            onKeyDown(key);
          } else {
            onKeyUp(key);
          }
        } else if (key == btn60) {
          if (k & 0x80) {
            if (shouldIgnoreEncoderButtonPress(key)) {
              continue;
            }
            input.inputBuffer = (Button)key;
          }
        }

      } else if ((key & 0x01) == 1) {  // カラム1
        if (k & 0x80) {
          u8_t ch = (key / 10) % 10;
          // Serial.printf("ch: %d\n", ch);
          FM.requestToggleChannelMask(ch);
        }
      }
    }

    keypad.writeRegister(TCA8418_REG_INT_STAT, 0x01);
  }
}

void onKeyDown(int key) {
  // 同じキーが既に押されてリピートしてるとき
  if (activeKey == key && keyRepeatState != KeyRepeatState::Idle) {
    return;
  }

  activeKey = key;
  keyRepeatState = KeyRepeatState::Waiting;
  if (keyRepeatTimer != nullptr) {
    xTimerStop(keyRepeatTimer, 0);  // 念のため
    xTimerChangePeriod(keyRepeatTimer, pdMS_TO_TICKS(INPUT_REPEAT_DELAY), 0);
    xTimerStart(keyRepeatTimer, 0);
  }
  input.inputBuffer = (Button)key;
}

void onKeyUp(int keyIndex) {
  // Ignore unrelated release events.
  if (activeKey != keyIndex) {
    return;
  }

  event repeatEvent = getRepeatEventForKey((Button)keyIndex);

  if (keyRepeatTimer != nullptr) {
    xTimerStop(keyRepeatTimer, 0);
  }
  keyRepeatState = KeyRepeatState::Idle;
  activeKey = -1;

  // Serial.printf("\tonKeyUp: %d\n", keyIndex);

  input.inputBuffer = btnNONE;
  discardQueuedEvent(repeatEvent);
}

QueueHandle_t xQueueInput = nullptr;
TaskHandle_t tskEventLoop = nullptr;

// イベントループ
void eventLoop(void* pvPrams) {
  event event;
  while (1) {
    if (xQueueReceive(xQueueInput, &event, portMAX_DELAY) == pdTRUE) {
      // 表示中の画面にイベントを送信
      switch (disp.currentView) {
        case ViewMode::Player: {
          playerWindow.eventHandler(event);
          break;
        }
        case ViewMode::Config: {
          cfgWindow.eventHandler(event);
          break;
        }
        case ViewMode::Visual: {
          visualWindow.eventHandler(event);
          break;
        }
        case ViewMode::Browser: {
          browserWindow.eventHandler(event);
          break;
        }
        default:
          break;
      }
    }
    // vTaskDelay(16);
  }
}

// イベント送信
void sendEventToQueue(event ev) {
  if (xQueueInput == nullptr) return;
  xQueueOverwrite(xQueueInput, &ev);
}

Input::Input() {
}

bool Input::init() {
  // ボタン状態をチェックするタスク
  // xTaskCreateUniversal(inputTask, "inputTask", 10000, NULL, 1, NULL,
  // PRO_CPU_NUM);

  // 外付けプルアップとRCフィルターを使用するため、内部プルアップは無効にする。
  pinMode(kEncoderPinA, INPUT);
  pinMode(kEncoderPinB, INPUT);
  resetEncoderState();
  attachInterrupt(digitalPinToInterrupt(kEncoderPinA), encoderGpioIrq, CHANGE);
  attachInterrupt(digitalPinToInterrupt(kEncoderPinB), encoderGpioIrq, CHANGE);

  // キュー作成
  xQueueInput = xQueueCreate(1, sizeof(event));

  // イベントループタスク
  xTaskCreateUniversal(eventLoop, "eventloop", 10000, NULL, 1, &tskEventLoop, PRO_CPU_NUM);

  // TCA8418 初期化
  pinMode(TCA8418_IRQ_PIN, INPUT);
  // 優先度 1 で ADPCMの割り込み 2 より低くしないとコンフリクトする
  xTaskCreatePinnedToCore(tcaTask, "tcaTask", 4096, nullptr, 1, nullptr,
                          PRO_CPU_NUM);  // メインコア
  attachInterrupt(digitalPinToInterrupt(TCA8418_IRQ_PIN), TCA8418_irq, FALLING);

  if (!keypad.begin(TCA8418_DEFAULT_ADDR, &Wire)) {
    Serial.println("Failed: TCA8418 init.");
    return false;
  } else {
    Serial.println("TCA8418 init.");

    keypad.matrix(1, 2);  // ROW0-7, COL0-9

    /*
      ## Keypad or GPIO Selection Registers, KP_GPIO1–3 (Address 0x1D–0x1F)

      ADDRESS   REGISTER NAME   REGISTER DESCRIPTION      BIT 7    6    5    4
      3    2    1    0 0x1D      KP_GPIO1        Keypad/GPIO Select 1 ROW7 ROW6
      ROW5 ROW4 ROW3 ROW2 ROW1 ROW0 0x1E      KP_GPIO2        Keypad/GPIO Select
      2          COL7 COL6 COL5 COL4 COL3 COL2 COL1 COL0 0x1F      KP_GPIO3
      Keypad/GPIO Select 3          N/A  N/A  N/A  N/A  N/A  N/A  COL9 COL8

    */
    keypad.writeRegister(0x1D,
                         0b11111111);  // 0x1D (KP_GPIO1): ROW0-7 全部マトリクス
    keypad.writeRegister(0x1E,
                         0b00000011);        // 0x1E (KP_GPIO2): COL0-7  -> 0x03
                                             // (COL0, COL1のみマトリクス、他はGPIO)
    keypad.writeRegister(0x1F, 0000000000);  // 0x1F (KP_GPIO3): COL8-9  -> 0x00 (すべてGPIO)

    keypad.pinMode(KP_DIV_0, OUTPUT);
    keypad.digitalWrite(KP_DIV_0, HIGH);
    keypad.pinMode(KP_DIV_1, OUTPUT);
    keypad.digitalWrite(KP_DIV_1, HIGH);
    keypad.pinMode(KP_IC, OUTPUT);
    keypad.digitalWrite(KP_IC, HIGH);
    keypad.pinMode(KP_AC, OUTPUT);
    keypad.digitalWrite(KP_AC, LOW);

    /*
      ## GPIO Data Direction Registers, GPIO_DIR1–3 (Address 0x23–0x25)
      A bit value of '0' in any of the unreserved bits sets the corresponding
      pin as an input. This is the default value. A 1 in any of these bits sets
      the pin as an output.

      ADDRESS   REGISTER NAME   REGISTER DESCRIPTION      BIT 7    6    5    4
      3    2    1    0 0x23      GPIO_DIR1       GPIO Direction 1 ROW7 ROW6 ROW5
      ROW4 ROW3 ROW2 ROW1 ROW0 0x24      GPIO_DIR2       GPIO Direction 2 COL7
      COL6 COL5 COL4 COL3 COL2 COL1 COL0 0x25      GPIO_DIR3       GPIO
      Direction 3              N/A  N/A  N/A  N/A  N/A  N/A  COL9 COL8

    */
    keypad.writeRegister(0x20, 0x00);  // ROW GPIOイベント全部オフ
    keypad.writeRegister(0x21, 0x00);  // COL0-7 GPIOイベント全部オフ
    keypad.writeRegister(0x22, 0x00);  // COL8-9 GPIOイベント全部オフ

    while (keypad.getEvent() != 0);
    keypad.writeRegister(TCA8418_REG_INT_STAT, 0x01);

    //  イベント削除
    keypad.flush();

    u8_t cfg = keypad.readRegister(TCA8418_REG_CFG);
    cfg &= ~TCA8418_REG_CFG_GPI_IEN;
    cfg |= TCA8418_REG_CFG_KE_IEN;
    keypad.writeRegister(TCA8418_REG_CFG, cfg);

    tcaSetLock(!isEnabled());

    // キーリピートタイマー初期化
    keyRepeatTimer = xTimerCreate("keyRepeat",
                                  pdMS_TO_TICKS(INPUT_REPEAT_DELAY),  // 周期
                                  pdTRUE,                             // auto-reload（繰り返し）
                                  nullptr,                            //
                                  hKeyRepeat);
  }

  return true;
}

bool Input::digitalWrite(u8_t pinnum, u8_t level) {  //
  return keypad.digitalWrite(pinnum, level);
}

void Input::inputHandler() {
  if (!_enabled) return;

  updateHoldCountdown();

  if (encoderEnabled && encoderDirty) {
    pollEncoder();
  }

  if (inputBuffer == btnNONE) return;

  const bool isControlSet2 = ndConfig.get(CFG_CONTROL) == CTRL_2;

  // 入力処理
  switch (disp.currentView) {
    case ViewMode::Player: {  // プレイヤーのとき
      switch (inputBuffer) {
        case btn00: {
          if (isControlSet2) {
            ndFile.dirPlay(1);
          } else {
            sendEventToQueue(event::Left);
          }
          break;
        }
        case btn10: {
          sendEventToQueue(event::Menu);
          break;
        }
        case btn20: {
          if (releasePlayHold()) {
            break;
          }
          sendEventToQueue(event::Option);
          break;
        }
        case btn60:
          sendEventToQueue(event::EncClick);
          break;
      }
      break;
    }
    case ViewMode::Config: {  // 設定ウィンドウのとき
      switch (inputBuffer) {
        case btn00:
          sendEventToQueue(event::Left);
          break;
        case btn10:
          sendEventToQueue(event::Right);
          break;
        case btn20:
          if (cfgWindow._isOptionMode) {
            sendEventToQueue(event::EncClick);
          } else {
            sendEventToQueue(event::Close);
          }
          break;
        case btn60:
          sendEventToQueue(event::EncClick);
          break;
      }
      break;
    }
    case ViewMode::Visual: {  // ビジュアル
      switch (inputBuffer) {
        case btn00:
          if (isControlSet2) {
            ndFile.dirPlay(1);
          } else {
            sendEventToQueue(event::Close);
          }
          break;
        case btn10:
          sendEventToQueue(event::Menu);
          break;
        case btn20:
          if (releasePlayHold()) {
            break;
          }
          sendEventToQueue(isControlSet2 ? event::Close : event::Option);
          break;
        case btn60: {
          sendEventToQueue(event::Menu);
          break;
        }
        default:
          break;
      }
      break;
    }
    case ViewMode::Browser: {  // ブラウザウィンドウのとき
      switch (inputBuffer) {
        case btn00:
          sendEventToQueue(event::Close);  // 閉じる
          break;
        case btn10:
          sendEventToQueue(event::UpDir);  // 上ディレクトリ
          break;
        case btn20:
          sendEventToQueue(event::EncClick);
          break;
        case btn60:  // エンコーダボタン
          sendEventToQueue(event::EncClick);
          break;
        default:
          break;
      }
      break;
    }
    default: {
    }
  }

  inputBuffer = btnNONE;
}

void Input::setEnabled(bool state) {
  _enabled = state;

  if (state) {
    setEncoderEnabled(true);
    tcaSetLock(false);
    return;
  }

  setEncoderEnabled(false);

  while (keypad.getEvent() != 0) {
  }
  keypad.writeRegister(TCA8418_REG_INT_STAT, 0x03);
  tcaSetLock(true);
}

bool Input::isEnabled() const {
  return _enabled;
}

void Input::setEncoderEnabled(bool state) {
  encoderEnabled = false;
  encoderStateInitialized = false;
  encoderBufferHead = 0;
  encoderBufferTail = 0;
  encoderOverflowCount = 0;
  encoderStepAccumulator = 0;
  encoderAlternateAccumulator = 0;
  encoderHasAlternatePath = false;
  encoderDirty = false;
  if (xQueueInput != nullptr) {
    xQueueReset(xQueueInput);
  }

  if (state && _enabled) {
    resetEncoderState();
    encoderEnabled = true;
  }
}

bool Input::isEncoderEnabled() const {
  return encoderEnabled;
}

Input input = Input();
