#include "keyinfo.h"

keyboard::keyboard() {
  // セマフォ
  keyinfoMutex = xSemaphoreCreateMutex();
  for (int i = 0; i < DEVICE_COUNT; i++) {
    for (int j = 0; j < MAX_CHANNELS; j++) {
      keyInfo[i][j] = (struct NoteInfo){0, 0};
    }
  }
  for (int i = 0; i < 16; i++) {
    trackPan[i] = (i < 8) ? PAN_CENTER : PAN_MUTE;
    trackKeyOn[i] = false;
    trackLevel[i] = 0;
    trackTone[i] = 0xff;
  }
}

void keyboard::reset() {
  if (xSemaphoreTake(keyinfoMutex, portMAX_DELAY) == pdTRUE) {
    for (int i = 0; i < DEVICE_COUNT; i++) {
      for (int j = 0; j < device_channels[i]; j++) {
        keyInfo[i][j] = (struct NoteInfo){0, 0};
      }
    }
    for (int i = 0; i < 16; i++) {
      trackPan[i] = (i < 8) ? PAN_CENTER : PAN_MUTE;
      trackKeyOn[i] = false;
      trackLevel[i] = 0;
      trackTone[i] = 0xff;
    }
    xSemaphoreGive(keyinfoMutex);
    // Serial.printf("Key Info Reset.\n");
  }
}

void keyboard::set(t_device device, u8_t ch, NoteInfo ni) {
  if (xSemaphoreTake(keyinfoMutex, portMAX_DELAY) == pdTRUE) {
    keyInfo[device][ch] = (struct NoteInfo)ni;
    xSemaphoreGive(keyinfoMutex);
  }
}

void setKeyboardYM2151ChannelMask(u8_t mask) {
  if (xSemaphoreTake(KeyBoard.keyinfoMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    KeyBoard.ym2151ChannelMask = mask;
    xSemaphoreGive(KeyBoard.keyinfoMutex);
  }
}

keyboard KeyBoard = keyboard();
