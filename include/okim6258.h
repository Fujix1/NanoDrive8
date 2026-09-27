/**
 * Nano Drive OKI MSM6258 Controller
 * (c) 2026 Fujix
 *
 * OKI M6258 codec logic derived from MAME at
 * https://github.com/mamedev/mame/blob/master/src/devices/sound/okim6258.cpp
 * license:BSD-3-Clause
 * copyright-holder:Barry Rodewald
 *
 */

#ifndef OKIM6258_H
#define OKIM6258_H
#include <Arduino.h>

#include <vector>

#include "freertos/portmacro.h"
#include "util.h"

#define OKIM6285_ADPCM_CACHE_SIZE 256
// ISR間のタスク起床遅延を吸収しつつ、PCM8イベントの反映遅延を約1ms以内に抑える。
#define OKIM6258_PCM_OUTPUT_QUEUE_SIZE 4
#define OKIM6258_VGM_OUTPUT_QUEUE_SIZE 1

class OKIM6258 {
 public:
  void init();
  void decode();
  u8_t encode(int16_t sample12);
  u8_t encodePair(int16_t sample12a, int16_t sample12b);
  void resetEncodeState();
  void latchAdpcmConfig();
  void applyAdpcmConfigRealtime();
  bool beginVgmPcmReencode(bool preserveInputCache = false);
  void endVgmPcmReencode(bool preserveInputCache = false);
  void requestVgmPcmEncode();
  void setVgmPcmPlaying(bool playing, bool forceDecoderReset = false,
                        bool preserveDecoderState = false);
  bool isVgmPcmReencodeActive() const;
  bool detectVgmBlockDcOffset(u32_t startPos, u32_t size);

  struct OKIM6258State {
    tOKIM6258Divider divider;
    u8_t adpcmBits;
    u8_t pan;
    bool isPlaying;
    int currentBlockId[8];
    u32_t posInBlock[8];
    u32_t blockStartPos[8];
    u32_t blockSizeBytes[8];
    u32_t playSerial[8];
    u8_t adpcmVolume[8];
    u8_t adpcmBlockVolume[8];
    u8_t pcmDataKind[8];
    bool hold[8];
    volatile u8_t pcmOutputQueue[OKIM6258_PCM_OUTPUT_QUEUE_SIZE];
    volatile u8_t pcmOutputHead;
    volatile u8_t pcmOutputTail;
    volatile u8_t pcmOutputCount;
    volatile u32_t pcmOutputUnderflows;
    volatile u32_t pcmEncodedBytes;
    volatile u32_t pcmEncodeMaxUs;
    volatile u8_t adpcmMode;
    volatile bool pcm1ReencodeEnabled;
    volatile bool vgmReencodeEnabled;
    volatile bool lowPassEnabled;
    volatile bool vgmChipPresent;
    volatile bool vgmPhysicalPlaying;
    volatile bool vgmPcmReencodeActive;
    volatile bool pcmProducerBusy;
    volatile bool pcmProducerPaused;
    volatile u8_t vgmOutputQueue[OKIM6258_VGM_OUTPUT_QUEUE_SIZE];
    volatile u8_t vgmOutputHead;
    volatile u8_t vgmOutputTail;
    volatile u8_t vgmOutputCount;
    volatile u32_t vgmOutputUnderflows;
    volatile u32_t vgmInputOverflows;
    volatile u32_t vgmEncodedBytes;
    volatile u32_t vgmEncodeMaxUs;
    volatile bool vgmProducerBusy;
    volatile bool vgmProducerPaused;

    ByteQueue cache;
    OKIM6258State() : cache(OKIM6285_ADPCM_CACHE_SIZE) {}
  };

  OKIM6258State state;
  int encodeSignal = 0;
  int encodeStepIndex = 0;

  struct DataBlock {
    u32_t startPos;
    u32_t size;
  };

  std::vector<DataBlock> dataBlocks;
  std::vector<u8_t> vgmBlockDcRemoval;

  void reset() {
    state.divider = OKIM6258_DIV_512;
    state.adpcmBits = 4;
    state.pan = 0x00;
    state.isPlaying = false;
    for (int i = 0; i < 8; ++i) {
      state.currentBlockId[i] = -1;
      state.posInBlock[i] = 0;
      state.blockStartPos[i] = 0;
      state.blockSizeBytes[i] = 0;
      state.playSerial[i] = 0;
      state.adpcmVolume[i] = 8;
      state.adpcmBlockVolume[i] = 8;
      state.pcmDataKind[i] = 4;
      state.hold[i] = false;
    }
    state.cache.clear();
    dataBlocks.clear();
    vgmBlockDcRemoval.clear();
    state.pcmOutputHead = 0;
    state.pcmOutputTail = 0;
    state.pcmOutputCount = 0;
    state.pcmOutputUnderflows = 0;
    state.pcmEncodedBytes = 0;
    state.pcmEncodeMaxUs = 0;
    state.adpcmMode = 0;
    state.pcm1ReencodeEnabled = false;
    state.vgmReencodeEnabled = false;
    state.lowPassEnabled = false;
    state.vgmChipPresent = false;
    state.vgmPhysicalPlaying = false;
    state.vgmPcmReencodeActive = false;
    state.pcmProducerBusy = false;
    state.pcmProducerPaused = false;
    state.vgmOutputHead = 0;
    state.vgmOutputTail = 0;
    state.vgmOutputCount = 0;
    state.vgmOutputUnderflows = 0;
    state.vgmInputOverflows = 0;
    state.vgmEncodedBytes = 0;
    state.vgmEncodeMaxUs = 0;
    state.vgmProducerBusy = false;
    state.vgmProducerPaused = false;
    resetEncodeState();
  }

 private:
};

extern OKIM6258 okim6258;
extern portMUX_TYPE okim6258Mux;

#endif
