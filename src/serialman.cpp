#include "serialman.h"
#include "serialaudio.h"

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
bool audioFooterPending = false;

void usbBusReset(void*, esp_event_base_t, int32_t, void*) {
  // USB イベントタスクからの通知。画面描画や受信バッファ操作は受信タスクで行う。
  portENTER_CRITICAL(&usbEventMux);
  ++pendingBusResets;
  portEXIT_CRITICAL(&usbEventMux);
}
void resetChips() {
  // Mute the main output while resetting both chips and stopping OKI.
  nju72342.mute();
  SerialAudio::reset();
  FM.reset();
  okim6258.reset();
  FM.setOKIM6258command(0x01, 1);
  nju72342.unmute();
}

void writeYM2151(uint8_t address, uint8_t value) {
  FM.setRegisterOPM(address, value, 0);
}

void setChipClockImpl(uint8_t chipID, uint32_t hz, bool drawFooter) {
  if ((chipID != CHIP_YM2151 && chipID != CHIP_OKIM6258) || hz == 0) return;
  if (chipID == CHIP_OKIM6258 && hz != 4000000 && hz != 8000000) return;
  const t_chip chip = static_cast<t_chip>(chipID);
  const uint8_t slot = ND::clockSlot[chip];
  if (slot >= ND::freq.size()) return;  // Absent or fixed-clock chip.
  const si5351Freq_t freq = chip == CHIP_OKIM6258
      ? (hz == 4000000 ? SI5351_4000 : SI5351_8000) : vgm.normalizeFreq(hz, chip);
  if (freq == SI5351_UNDEFINED || ND::freq[slot] == freq) return;

  ND::freq[slot] = freq;
  SI5351.setFreq(ND::freq[slot], slot);
  const size_t chipSlot = ND::chipSlot[chip];
  if (chipSlot < ND::chipNames.size())
    ND::chipNames[chipSlot] = ND::formatChipName(freq, chip);
  if (drawFooter) serialModeUpdateFooter();
  else audioFooterPending = true;
}
void setChipClock(uint8_t chipID, uint32_t hz) { setChipClockImpl(chipID, hz, true); }
void setOkiClock(uint32_t hz) { setChipClockImpl(CHIP_OKIM6258, hz, false); }
}  // namespace

// シリアルモード初期化
void SerialMan::init() {
  receiveMutex = xSemaphoreCreateMutex();
  receiveBuffer = static_cast<uint8_t*>(
      heap_caps_malloc(RECEIVE_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  configASSERT(receiveMutex != nullptr && receiveBuffer != nullptr);
  SerialAudio::init(setOkiClock);
  protocol.configure(resetChips, writeYM2151, ND_FIRMWARE_VERSION, setChipClock, SerialAudio::command);
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
    SerialAudio::service();
    receiver.receive();
    receiver.processCommands();
    SerialAudio::service();
    if (audioFooterPending) { audioFooterPending = false; serialModeUpdateFooter(); }
    vTaskDelay(1);
  }
}

void SerialMan::resetReceiveLocked(uint32_t reason) {
  status.discardedBytes += status.bufferedBytes;
  status.bufferedBytes = 0;
  status.dataLost = false;
  receiveHead = receiveTail = 0;
  protocolResetPending = true;
  transportLossPending |= reason;
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
  // isPlugged() is a SOF observation, not proof of transport data loss.
  // A transient observation must not discard valid queued bytes or stop audio.
  // An explicit USB bus reset still ends the old stream and resynchronizes RX.
  if (resets != 0) {
    resetReceiveLocked(SerialAudio::BusReset);
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
    if (status.dataLost) {
      status.discardedBytes += count;
      continue;
    }
    if (count > RECEIVE_SIZE - status.bufferedBytes) {
      // 欠落後の byte 列を正常なフレームとして渡さない。
      ++status.overflows;
      resetReceiveLocked(SerialAudio::ReceiveOverflow);
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
  resetReceiveLocked(SerialAudio::ExplicitDiscard);
  xSemaphoreGive(receiveMutex);
}

void SerialMan::processCommands() {
  // Parser / response state belongs exclusively to this task.
  xSemaphoreTake(receiveMutex, portMAX_DELAY);
  const bool reset = protocolResetPending;
  const uint32_t lossReason = transportLossPending;
  protocolResetPending = false;
  transportLossPending = 0;
  // Overflow already discarded the ring in receive(). Resume at a delimiter.
  status.dataLost = false;
  xSemaphoreGive(receiveMutex);
  if (reset) { protocol.clear(true); SerialAudio::transportLost(lossReason); }
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
