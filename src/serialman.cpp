#include "serialman.h"

#include <esp_heap_caps.h>
#include <freertos/task.h>

#include "NJU72342.h"
#include "SI5351.hpp"
#include "disp.h"
#include "fm.h"
#include "nd.h"
#include "okim6258.h"
#include "vgm.h"

namespace {
portMUX_TYPE usbEventMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t pendingBusResets = 0;

void usbBusReset(void*, esp_event_base_t, int32_t, void*) {
  // USB イベントタスクからの通知。画面描画や受信バッファ操作は受信タスクで行う。
  portENTER_CRITICAL(&usbEventMux);
  ++pendingBusResets;
  portEXIT_CRITICAL(&usbEventMux);
}
void resetChips() {
  FM.reset();
  okim6258.reset();
  FM.setOKIM6258command(0x01, 1);
}

void writeYM2151(uint8_t address, uint8_t value) {
  FM.setRegisterOPM(address, value, 0);
}

void setChipClock(uint8_t chipID, uint32_t hz) {
  // Only YM2151 clock control is implemented in this FM test phase.
  if (chipID != CHIP_YM2151 || hz == 0) return;
  const t_chip chip = static_cast<t_chip>(chipID);
  const uint8_t slot = ND::clockSlot[chip];
  if (slot >= ND::freq.size()) return;  // Absent or fixed-clock chip.
  const si5351Freq_t freq = vgm.normalizeFreq(hz, chip);
  if (freq == SI5351_UNDEFINED || ND::freq[slot] == freq) return;

  ND::freq[slot] = freq;
  SI5351.setFreq(ND::freq[slot], slot);
  const size_t chipSlot = ND::chipSlot[chip];
  if (chipSlot < ND::chipNames.size())
    ND::chipNames[chipSlot] = ND::formatChipName(freq, chip);
  serialModeUpdateFooter();
}
}  // namespace

// シリアルモード初期化
void SerialMan::init() {
  receiveMutex = xSemaphoreCreateMutex();
  receiveBuffer = static_cast<uint8_t*>(
      heap_caps_malloc(RECEIVE_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  configASSERT(receiveMutex != nullptr && receiveBuffer != nullptr);
  protocol.configure(resetChips, writeYM2151, ND_FIRMWARE_VERSION, setChipClock);
  status.usbLinkActive = Serial.isPlugged();
  Serial.onEvent(ARDUINO_HW_CDC_BUS_RESET_EVENT, usbBusReset);

  ND::canPlay = false;
  ND::isPaused = false;
  ND::fileFormat = FileFormat::Unknown;
  ND::freq = {SI5351_4000, SI5351_8000, SI5351_UNDEFINED};
  SI5351.setFreq(ND::freq[0], 0);
  SI5351.setFreq(ND::freq[1], 1);

  // YM2151 の IC と OKI の AC をリセットし、ソフトウェア状態も初期化する。
  resetChips();
  nju72342.setVolumeAll(0);

  ND::chipNames = {ND::formatChipName(ND::freq[0], CHIP_YM2151),
                   ND::formatChipName(ND::freq[1], CHIP_OKIM6258)};
  playerWindow.show();
}

void SerialMan::startSerialTask() {
  if (receiveTask != nullptr) return;
  const BaseType_t result = xTaskCreatePinnedToCore(
      serialTask, "serialTask", 4096, this, 1, &receiveTask, APP_CPU_NUM);
  configASSERT(result == pdPASS && receiveTask != nullptr);
}

void SerialMan::serialTask(void* arg) {
  auto& receiver = *static_cast<SerialMan*>(arg);
  while (true) {
    // Apply masks in the same task as incoming YM2151 register writes.
    FM.applyPendingChannelMask();
    receiver.receive();
    receiver.processCommands();
    vTaskDelay(1);
  }
}

void SerialMan::resetReceiveLocked() {
  status.discardedBytes += status.bufferedBytes;
  status.bufferedBytes = 0;
  status.dataLost = false;
  receiveHead = receiveTail = 0;
  protocolResetPending = true;
}

void SerialMan::receive() {
  uint32_t resets;
  portENTER_CRITICAL(&usbEventMux);
  resets = pendingBusResets;
  pendingBusResets = 0;
  portEXIT_CRITICAL(&usbEventMux);
  const bool plugged = Serial.isPlugged();

  xSemaphoreTake(receiveMutex, portMAX_DELAY);
  const bool linkChanged = plugged != status.usbLinkActive;
  if (linkChanged) {
    if (plugged) ++status.connections;
    else ++status.disconnections;
    status.usbLinkActive = plugged;
  }
  status.busResets += resets;
  if (linkChanged || resets != 0) {
    // リンク変更・再列挙をまたいで古い受信データを連結しない。
    resetReceiveLocked();
    const int queued = Serial.available();
    usbDiscardRemaining = queued > 0 ? static_cast<size_t>(queued) : 0;
  }

  uint8_t bytes[64];
  // 1 周の仕事量を制限する。RX 状態・入力処理を長時間占有しない。
  for (unsigned block = 0; block < 8 && Serial.available() > 0; ++block) {
    const size_t request = usbDiscardRemaining != 0 && usbDiscardRemaining < sizeof(bytes)
        ? usbDiscardRemaining : sizeof(bytes);
    const size_t count = Serial.read(bytes, request);
    if (count == 0 || count > sizeof(bytes)) break;
    status.receivedBytes += count;
    status.lastReceiveMs = millis();
    if (usbDiscardRemaining != 0) {
      const size_t discard = count < usbDiscardRemaining ? count : usbDiscardRemaining;
      usbDiscardRemaining -= discard;
      status.discardedBytes += count;
      continue;
    }
    if (!plugged || status.dataLost) {
      status.discardedBytes += count;
      continue;
    }
    if (count > RECEIVE_SIZE - status.bufferedBytes) {
      // 欠落後の byte 列を正常なフレームとして渡さない。
      ++status.overflows;
      resetReceiveLocked();
      status.dataLost = true;
      status.discardedBytes += count;
      continue;
    }
    for (size_t i = 0; i < count; ++i) {
      receiveBuffer[receiveHead] = bytes[i];
      receiveHead = (receiveHead + 1) % RECEIVE_SIZE;
    }
    status.bufferedBytes += count;
  }
  xSemaphoreGive(receiveMutex);
}

size_t SerialMan::read(uint8_t* data, size_t size) {
  if (receiveMutex == nullptr || data == nullptr || size == 0) return 0;
  xSemaphoreTake(receiveMutex, portMAX_DELAY);
  const size_t count = status.dataLost ? 0 :
      (size < status.bufferedBytes ? size : status.bufferedBytes);
  for (size_t i = 0; i < count; ++i) {
    data[i] = receiveBuffer[receiveTail];
    receiveTail = (receiveTail + 1) % RECEIVE_SIZE;
  }
  status.bufferedBytes -= count;
  xSemaphoreGive(receiveMutex);
  return count;
}

SerialMan::ReceiveStatus SerialMan::receiveStatus() {
  if (receiveMutex == nullptr) return {};
  xSemaphoreTake(receiveMutex, portMAX_DELAY);
  const ReceiveStatus result = status;
  xSemaphoreGive(receiveMutex);
  return result;
}

void SerialMan::clearReceiveBuffer() {
  if (receiveMutex == nullptr) return;
  xSemaphoreTake(receiveMutex, portMAX_DELAY);
  resetReceiveLocked();
  xSemaphoreGive(receiveMutex);
}

void SerialMan::processCommands() {
  // Parser / response state belongs exclusively to this task.
  xSemaphoreTake(receiveMutex, portMAX_DELAY);
  const bool reset = protocolResetPending;
  protocolResetPending = false;
  if (status.dataLost) resetReceiveLocked();
  protocolResetPending = false;
  xSemaphoreGive(receiveMutex);
  if (reset) protocol.clear(true);
  protocol.expire(millis());

  for (size_t count = 0; count < 512; ++count) {
    if (protocol.outputSize() != 0) {
      // HWCDC isConnected is used only for TX readiness, never as COM-open status.
      if (!Serial.isConnected()) return;
      const int space = Serial.availableForWrite();
      if (space <= 0) return;
      const size_t pending = protocol.outputSize();
      const size_t amount = pending < static_cast<size_t>(space) ? pending : space;
      protocol.consumeOutput(Serial.write(protocol.output(), amount));
      if (protocol.outputSize() != 0) return;
    }
    uint8_t byte;
    if (read(&byte, 1) == 0) return;
    protocol.feed(byte, millis());
  }
}

SerialMan serialMan;
