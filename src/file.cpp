#include "file.h"

#include <PNGdec.h>
#include <dirent.h>
#include <esp_system.h>

#include "mdx.h"
#include "okim6258.h"
#include "unlz.h"

static SPIClass SPI_SD;

static File hFile;

static SemaphoreHandle_t spFileOpen;  // ファイル開く処理用セマフォ
static u16_t scanProgressX = 0;
static u16_t scanProgressY = 0;

bool tryLockDrawing();
void lockDrawing();
void unlockDrawing();

static void resetPlaybackStartTime() {
  if (ND::fileFormat == FileFormat::VGM || ND::fileFormat == FileFormat::VGZ) {
    vgm.startTick = micros64() + 1000;
  }
#ifdef USE_MDX
  else if (ND::fileFormat == FileFormat::MDX) {
    MDX._startTick = millis();
  }
#endif
}

void showError(String message) {
  // const bool prevStopTimerDrawing = disp.stopTimerDrawing;
  //
  if (disp.currentView == ViewMode::Visual) {
    disp.stopTimerDrawing = true;
  }

  lockDrawing();
  lcd.startWrite();
  // lcd.fillScreen(TFT_BLACK);
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.setCursor(0, 75);
  lcd.print(message);
  lcd.endWrite();
  unlockDrawing();
  // disp.stopTimerDrawing = prevStopTimerDrawing;
}

// LZX解凍

static bool findMdxBodyOffset(const u8_t* bytes, size_t bytesSize, size_t* bodyOffset) {
  if (!bytes || !bodyOffset) {
    return false;
  }

  for (size_t i = 0; i + 2 < bytesSize; i++) {
    if (bytes[i] != 0x0d || bytes[i + 1] != 0x0a || bytes[i + 2] != 0x1a) {
      continue;
    }

    const size_t pdxNameStart = i + 3;
    for (size_t j = pdxNameStart; j < bytesSize; j++) {
      if (bytes[j] == 0x00) {
        *bodyOffset = j + 1;
        return true;
      }
    }
    return false;
  }

  return false;
}

static bool decodeMdxLzInPlace(u8_t* bytes, u32_t* bytesSize, size_t capacity, const String& path) {
  size_t bodyOffset = 0;
  if (!findMdxBodyOffset(bytes, *bytesSize, &bodyOffset)) {
    return true;
  }

  size_t streamOffset = 0;
  if (!unlz::findStreamOffset(bytes, *bytesSize, bodyOffset, &streamOffset)) {
    return true;
  }

  const size_t compressedSize = *bytesSize - streamOffset;
  u8_t* compressed = (u8_t*)heap_caps_malloc(compressedSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!compressed) {
    showError("ERROR: Failed to allocate PSRAM for MDX LZ.\n" + path);
    return false;
  }

  memcpy(compressed, bytes + streamOffset, compressedSize);
  unlz::DecodeResult result =
      unlz::decode(compressed, compressedSize, bytes + bodyOffset, capacity - bodyOffset);
  heap_caps_free(compressed);

  if (result.result != unlz::Result::Ok) {
    showError("ERROR: Failed to decode MDX LZ.\n" + String(unlz::resultText(result.result)) + "\n" +
              path);
    return false;
  }

  if (result.bytesWritten > capacity - bodyOffset) {
    showError("ERROR: The decoded MDX file is too large.\nFound: " +
              String(bodyOffset + result.bytesWritten) + "\nMax file size is " + String(capacity) +
              ".\n" + path);
    return false;
  }

  const u32_t oldSize = *bytesSize;
  *bytesSize = bodyOffset + result.bytesWritten;
  MDX.isLZX = true;
  Serial.printf("MDX LZ decoded: %u -> %u Bytes.\n", (unsigned int)oldSize,
                (unsigned int)*bytesSize);
  return true;
}

static bool decodePdxLzInPlace(u8_t* bytes, u32_t* bytesSize, size_t capacity, const String& name) {
  size_t streamOffset = 0;
  if (!unlz::findStreamOffset(bytes, *bytesSize, 0, &streamOffset)) {
    return true;
  }

  const size_t compressedSize = *bytesSize - streamOffset;
  u8_t* compressed = (u8_t*)heap_caps_malloc(compressedSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!compressed) {
    showError("ERROR: Failed to allocate PSRAM for PDX LZ.\n" + name);
    return false;
  }

  memcpy(compressed, bytes + streamOffset, compressedSize);
  unlz::DecodeResult result = unlz::decode(compressed, compressedSize, bytes, capacity);
  heap_caps_free(compressed);

  if (result.result != unlz::Result::Ok) {
    showError("ERROR: Failed to decode PDX LZ.\n" + String(unlz::resultText(result.result)) + "\n" +
              name);
    return false;
  }

  if (result.bytesWritten > capacity) {
    showError("ERROR: The decoded PDX file is too large.\nFound: " + String(result.bytesWritten) +
              "\nMax file size is " + String(capacity) + ".\n" + name);
    return false;
  }

  const u32_t oldSize = *bytesSize;
  *bytesSize = result.bytesWritten;
  Serial.printf("PDX LZ decoded: %u -> %u Bytes.\n", (unsigned int)oldSize,
                (unsigned int)*bytesSize);
  return true;
}

// 進捗状況表示
static void updateScanProgress(int n) {
  lcd.setCursor(scanProgressX, scanProgressY);
  lcd.printf("%d\n", n);
}

//-------------------------------------------------------------------------
// キャッシュ
struct CacheTaskParam {
  u32_t pos;
  int cacheIndex;
};

QueueHandle_t cacheQueue;  // メッセージキュー

// キャッシュ
u8_t* cache[NUM_CACHE];  // PSRAM用キャッシュ

volatile int activeCache = 0;
static File _cacheFile;
volatile int cachePos = 0;

void fillCache(u32_t pos, int chaceIndex) {
  int readSize = ndFile.size - vgm.gd3Size - pos - CACHE_SIZE;  // 読み込みサイズ

  //
  // |  キャッシュ  |  キャッシュ  |
  // |              vgm               | gd3 |

  // キャッシュサイズ全部読めるとき
  if (readSize >= CACHE_SIZE) {
    int bytesRead = _cacheFile.read(cache[chaceIndex], CACHE_SIZE);
    // Serial.printf("Bytes read: readSize: 0x%x, 0x%x\n", CACHE_SIZE,
    // bytesRead);
    //  Serial.printf("cache[%d]: %0x, %x, %x\n", chaceIndex, pos + CACHE_SIZE,
    //  cache[chaceIndex][0],
    //               cache[chaceIndex][CACHE_SIZE - 1]);
  } else if (0 <= readSize && readSize < CACHE_SIZE) {
    // |  キャッシュ  |  キャッシュ  |
    // |        vgm        | gd3 |

    // キャッシュ終了またぐとき
    int bytesRead = _cacheFile.read(cache[chaceIndex], readSize);
    /*Serial.printf("[またぐ] readSize: 0x%x, Bytes read: 0x%x\n", readSize,
    bytesRead); Serial.printf("cache[%d]: %0x, %x, %x\n", chaceIndex, pos +
    CACHE_SIZE, cache[chaceIndex][0], cache[chaceIndex][readSize - 1]);
*/

    // ループ開始部分補充
    if (vgm.loopOffset != 0) {
      int padSize = CACHE_SIZE - readSize;
      _cacheFile.seek(vgm.loopOffset + 0x1C);  // ループ開始地点

      int bytesRead = _cacheFile.read(cache[chaceIndex] + readSize, padSize);
      /*
            Serial.printf("[ループ開始] loopOffset: 0x%x, readSize: 0x%x, Bytes
         read: 0x%x\n", vgm.loopOffset + 0x1C, readSize, bytesRead);
            Serial.printf("cache[%d]: @0x%0x %x, @0x%x %x\n", chaceIndex,
         readSize, cache[chaceIndex][readSize], CACHE_SIZE,
         cache[chaceIndex][CACHE_SIZE - 1]);
                          */
    }
  } else {
    // |  キャッシュ  |  キャッシュ  |
    // |  vgm   | gd3 |

    // キャッシュが超えるとき

    // ループ続き補充
    if (vgm.loopOffset != 0) {
      readSize = CACHE_SIZE;
      // int start = vgm.loopOffset + 0x1C + CACHE_SIZE - readSize;
      //_cacheFile.seek(vgm.loopOffset + 0x1C + CACHE_SIZE - readSize);

      int bytesRead = _cacheFile.read(cache[chaceIndex], readSize);
      // Serial.printf("[ループ続き]: %x, %x\n", cache[chaceIndex][0],
      // cache[chaceIndex][readSize - 1]);
    }
  }
}

void fillCacheTask(void* pvParameters) {
  CacheTaskParam* param = (CacheTaskParam*)pvParameters;
  u32_t pos = param->pos;
  int cacheIndex = param->cacheIndex;

  fillCache(pos, cacheIndex);
  delete param;
  Serial.printf("Task End.\n");
  vTaskDelete(NULL);
}

void cacheTask(void* pvParameters) {
  CacheTaskParam param;
  while (1) {
    if (xQueueReceive(cacheQueue, &param, portMAX_DELAY) == pdTRUE) {
      fillCache(param.pos, param.cacheIndex);
    }
    delay(1);
  }
}

// キャッシュ初期化
bool initCache(String path) {
  if (_cacheFile) {
    _cacheFile.close();
  }
  _cacheFile = SD.open(path.c_str());
  if (!_cacheFile) {
    Serial.printf("ERROR: Failed to open cache file: %s\n", path.c_str());
    return false;
  }

  // 初期充填
  activeCache = 0;
  _cacheFile.seek(0);
  _cacheFile.read(cache[0], CACHE_SIZE);
  _cacheFile.read(cache[1], CACHE_SIZE);
  cachePos = 0;

  return true;
}

//-------------------------------------------------------------------------

bool NDFile::init() {
  _att = 0;

  // セマフォ作成
  spFileOpen = xSemaphoreCreateBinary();
  xSemaphoreGive(spFileOpen);

  // SD用 SPI開始
  SPI_SD.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
  int n = 0;
  lcd.print("Checking SD card");
  while (!SD.begin(SD_CS, SPI_SD, 80000000)) {  // SD マウント試行 @ 80MHz
    vTaskDelay(200);
    n++;
    lcd.print(".");
    if (n == 10) {
      lcd.print("\n\nERROR: SD card open failed.\n");
      return false;
    }
  }

  u8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    lcd.println("\n\nERROR: No SD card attached.\n");
    return false;
  }

  lcd.print("\nSD card detected.");
  lcd.print("\n- Type: ");
  if (cardType == CARD_MMC) {
    lcd.println("MMC");
  } else if (cardType == CARD_SD) {
    lcd.println("SDSC");
  } else if (cardType == CARD_SDHC) {
    lcd.println("SDHC");
  } else {
    lcd.println("UNKNOWN");
  }
  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
  lcd.printf("- Size: %llu MB\n", cardSize);
  vTaskDelay(100);

  // メモリ確保
  psramInit();  // ALWAYS CALL THIS BEFORE USING THE PSRAM
  data = (u8_t*)ps_calloc(MAX_FILE_SIZE, sizeof(u8_t));

  // PSRAMキャッシュ確保
  for (int i = 0; i < NUM_CACHE; i++) {
    if (!cache[i]) {
      cache[i] = (u8_t*)ps_calloc(CACHE_SIZE, sizeof(u8_t));
    }
  }

  // キューを初期化
  cacheQueue = xQueueCreate(2, sizeof(CacheTaskParam));
  if (!cacheQueue) {
    Serial.println("ERROR: cacheQueue create failed!");
    return false;
  }

  // キャッシュタスク
  xTaskCreatePinnedToCore(cacheTask, "cacheTask", 4096, NULL, 1, NULL, PRO_CPU_NUM);

  return true;
}

//----------------------------------------------------------------------
// ファイル開いてPSRAMに配置
FileFormat NDFile::readFile(String path) {
  int n = 0;
  size = 0;
  MDX.isLZX = false;
  Serial.printf("readFile: %s\n", path.c_str());

  hFile = SD.open(path.c_str());
  if (!hFile) {
    showError("ERROR: Failed to open file.\n" + path);
    hFile.close();
    return FileFormat::Unknown;
  }

  // ヘッダチェック
  u8_t header[4] = {0};
  if (hFile.read(header, sizeof(header)) != sizeof(header)) {
    showError("ERROR: Invalid file.\n" + path);
    hFile.close();
    return FileFormat::Unknown;
  }

  bool isVgm = (header[0] == 'V' && header[1] == 'g' && header[2] == 'm' && header[3] == ' ');
  bool isGz = (header[0] == 0x1F && header[1] == 0x8B);

  if (!isVgm && !isGz) {  // MDX のとき
    if (hFile.size() > MAX_MDX_SIZE) {
      showError("ERROR: The MDX file is too large.\nFound: " + String(hFile.size()) +
                "\nMax file size is " + String(MAX_MDX_SIZE) + ".\n" + path);
      hFile.close();
      return FileFormat::Unknown;
    }
    accessMode = ACCESS_PSRAM;
    size = hFile.size();
    hFile.seek(0);
    hFile.read(data, size);
    if (!decodeMdxLzInPlace(data, &size, MAX_MDX_SIZE, path)) {
      hFile.close();
      return FileFormat::Unknown;
    }
    Serial.printf("MDX file name: %s\n", path.c_str());
    hFile.close();
    return FileFormat::MDX;
  }

  if (isVgm) {  // VGM のとき
    size = hFile.size();
    Serial.printf("size: %u Bytes.\n", size);

    if (hFile.size() > MAX_FILE_SIZE) {
      //  シーケンシャルモード
      accessMode = ACCESS_CACHE;
      Serial.printf("Sequential mode.\n");
    } else {
      // PSRAM モード
      accessMode = ACCESS_PSRAM;
      Serial.printf("PSRAM mode.\n");
    }

    if (accessMode == ACCESS_PSRAM) {
      hFile.seek(0);
      hFile.read(data, size);
      // Serial.printf("File name: %s\n", path.c_str());
    }
    hFile.close();

    if (accessMode == ACCESS_CACHE) {
      initCache(path.c_str());
    }
    return FileFormat::VGM;
  }

  if (isGz) {  // VGZ のとき
    Serial.printf("isGz\n");
    accessMode = ACCESS_PSRAM;

    // gzip footer(ISIZE) から解凍後サイズを先読みして上限チェック
    const u32_t gzFileSize = hFile.size();
    if (gzFileSize < 18) {  // minimal gzip size: header(10) + trailer(8)
      showError("ERROR: Invalid gzip file.\n" + path);
      hFile.close();
      return FileFormat::Unknown;
    }
    u8_t isizeBuf[4];
    hFile.seek(gzFileSize - 4);
    if (hFile.read(isizeBuf, sizeof(isizeBuf)) != sizeof(isizeBuf)) {
      showError("ERROR: Invalid gzip file.\n" + path);
      hFile.close();
      return FileFormat::Unknown;
    }
    u32_t unzipSize = (u32_t)isizeBuf[0] | ((u32_t)isizeBuf[1] << 8) | ((u32_t)isizeBuf[2] << 16) |
                      ((u32_t)isizeBuf[3] << 24);
    if (unzipSize > MAX_FILE_SIZE) {
      showError(
          "ERROR: The original VGM file is too large. Ungzip the "
          "file.\nFound: " +
          String(unzipSize) + "\nMax file size is " + String(MAX_FILE_SIZE) + ".\n" + path);
      hFile.close();
      return FileFormat::Unknown;
    }

    // gzip 解凍して PSRAM に展開する
    hFile.seek(0);

    auto readByte = [&](void) -> int {
      int c = hFile.read();
      if (c < 0) return -1;
      return c & 0xFF;
    };

    auto skipBytes = [&](size_t count) -> bool {
      while (count--) {
        if (readByte() < 0) return false;
      }
      return true;
    };

    // Parse gzip header.
    int id1 = readByte();
    int id2 = readByte();
    int cm = readByte();
    int flg = readByte();
    if (id1 != 0x1F || id2 != 0x8B || cm != 8 || flg < 0) {
      showError("ERROR: Invalid gzip header\n" + path);
      hFile.close();
      return FileFormat::Unknown;
    }
    // MTIME(4), XFL(1), OS(1)
    if (!skipBytes(6)) {
      showError("ERROR: Invalid gzip header\n" + path);
      hFile.close();
      return FileFormat::Unknown;
    }

    if (flg & 0x04) {  // FEXTRA
      int xlen0 = readByte();
      int xlen1 = readByte();
      if (xlen0 < 0 || xlen1 < 0) {
        showError("ERROR: Invalid gzip header\n" + path);
        hFile.close();
        return FileFormat::Unknown;
      }
      u16_t xlen = (u16_t)xlen0 | ((u16_t)xlen1 << 8);
      if (!skipBytes(xlen)) {
        showError("ERROR: Invalid gzip header\n" + path);
        hFile.close();
        return FileFormat::Unknown;
      }
    }
    if (flg & 0x08) {  // FNAME
      while (true) {
        int c = readByte();
        if (c < 0) {
          showError("ERROR: Invalid gzip header\n" + path);
          hFile.close();
          return FileFormat::Unknown;
        }
        if (c == 0) break;
      }
    }
    if (flg & 0x10) {  // FCOMMENT
      while (true) {
        int c = readByte();
        if (c < 0) {
          showError("ERROR: Invalid gzip header\n" + path);
          hFile.close();
          return FileFormat::Unknown;
        }
        if (c == 0) break;
      }
    }
    if (flg & 0x02) {  // FHCRC
      if (!skipBytes(2)) {
        showError("ERROR: Invalid gzip header\n" + path);
        hFile.close();
        return FileFormat::Unknown;
      }
    }

    const size_t zlibBufSize = sizeof(inflate_state) + 32768;
    u8_t* zlib_buf = (u8_t*)heap_caps_malloc(zlibBufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!zlib_buf) {
      showError("ERROR: Failed to allocate PSRAM for gzip.\n" + path);
      hFile.close();
      return FileFormat::Unknown;
    }
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    memset(zlib_buf, 0, zlibBufSize);
    stream.zalloc = (alloc_func)0;
    stream.zfree = (free_func)0;
    stream.opaque = (voidpf)0;

    inflate_state* state = (inflate_state*)zlib_buf;
    stream.state = (struct internal_state*)state;
    state->window = &zlib_buf[sizeof(inflate_state)];
    if (inflateInit2(&stream, -15) != Z_OK) {
      showError("ERROR: gzip init failed.\n" + path);
      heap_caps_free(zlib_buf);
      hFile.close();
      return FileFormat::Unknown;
    }

    u8_t inbuf[1024];
    size_t out_pos = 0;
    int status = Z_OK;

    while (true) {
      if (stream.avail_in == 0) {
        int r = hFile.read(inbuf, sizeof(inbuf));
        if (r <= 0) {
          status = Z_DATA_ERROR;
          break;
        }
        stream.next_in = inbuf;
        stream.avail_in = (unsigned int)r;
      }

      if (out_pos >= MAX_FILE_SIZE) {
        status = Z_BUF_ERROR;
        break;
      }
      stream.next_out = data + out_pos;
      stream.avail_out = (unsigned int)(MAX_FILE_SIZE - out_pos);

      int ret = inflate(&stream, Z_NO_FLUSH, 0);
      size_t produced = (MAX_FILE_SIZE - out_pos) - stream.avail_out;
      out_pos += produced;
      if (out_pos > MAX_FILE_SIZE) {
        status = Z_BUF_ERROR;
        break;
      }

      if (ret == Z_STREAM_END) {
        status = Z_STREAM_END;
        break;
      }
      if (ret == Z_BUF_ERROR && stream.avail_out == 0) {
        status = Z_BUF_ERROR;
        break;
      }
      if (ret == Z_BUF_ERROR && stream.avail_in == 0) {
        continue;
      }
      if (ret != Z_OK && ret != Z_BUF_ERROR) {
        status = ret;
        break;
      }
    }

    inflateEnd(&stream);
    heap_caps_free(zlib_buf);

    if (status != Z_STREAM_END) {
      if (status == Z_BUF_ERROR) {
        showError("ERROR: The file is too large.\nMax file size is " + String(MAX_FILE_SIZE) +
                  ".\n" + path);
      } else {
        showError("ERROR: gzip decode failed.\n" + path);
      }
      hFile.close();
      return FileFormat::Unknown;
    }

    if (get_ui32_at(0) != 0x206d6756) {
      showError("ERROR: File format is not VGM.\n" + path);
      hFile.close();
      return FileFormat::Unknown;
    }

    // Serial.printf("File name: %s\n", path.c_str());
    size = (u32_t)out_pos;
    hFile.close();

    return FileFormat::VGZ;
  }

  return FileFormat::Unknown;
}

//----------------------------------------------------------------------
// PDX 読み込む
// dir: ディレクトリ
// name: 想定パターン
// 戻り値: 0 なし、サイズ大きすぎ
//        1 すでに読み込みずみ
//        サイズ 読み込んだ

u32_t NDFile::readPDX(String name) {
  if (name == "") {  // PDX無し
    pdxSize = 0;
    pdxName = "";
    pdxLoadedName = "";
    return 0;
  }

  if (pdxSize != 0 && pdxLoadedName == name) {
    return 1;
  }

  pdxSize = 0;
  pdxLoadedName = "";

  String baseName = name;

  String ext = name.substring(name.length() - 4);
  if (!ext.equalsIgnoreCase(".pdx")) {
    baseName = baseName + ".pdx";
  }

  String lower = baseName;
  lower.toLowerCase();  // 小文字
  String upper = baseName;
  upper.toUpperCase();  // 大文字

  static File _pdxFile;
  String dir = fileTree.getFullPath(ndFile.currentNode->parent);

  _pdxFile = SD.open(dir + "/" + lower.c_str());
  if (!_pdxFile) {
    _pdxFile = SD.open(dir + "/" + upper.c_str());
    if (!_pdxFile) {
      return 0;
    }
  }
  if (_pdxFile.size() > MAX_PDX_SIZE) {
    _pdxFile.close();
    return 0;
  }

  pdxSize = _pdxFile.size();
  _pdxFile.read(data + PDX_OFFSET, pdxSize);
  _pdxFile.close();
  if (!decodePdxLzInPlace(data + PDX_OFFSET, &pdxSize, MAX_PDX_SIZE, name)) {
    pdxSize = 0;
    pdxLoadedName = "";
    return 0;
  }
  pdxLoadedName = name;

  return pdxSize;
}

//----------------------------------------------------------------------
// ディレクトリ内の 次曲再生。マイナスは前の曲
// 戻り値: 成功/不成功
bool NDFile::filePlay(int count) {
  switch (ndConfig.get(CFG_SHUFFLE)) {
    case TRANDOM_FOLDER:
      return _playRandomFile(count);
    case TRANDOM_ALL:
      return _playRandomAll(count);
    default:
      break;
  }

  Node* targetFile = nullptr;
  // フォルダリピート時だけ同一フォルダ内をぐるぐるする
  if (count < 0) {
    const bool wrapInFolder = (ndConfig.get(CFG_REPEAT) == REPEAT_FOLDER);
    targetFile = fileTree.getPrevFileNode(ndFile.currentNode, wrapInFolder);
    if (!targetFile && !wrapInFolder) {
      // 前のディレクトリの最後の曲
      Node* targetDir = fileTree.getPrevDirNode(ndFile.currentNode);
      targetFile = fileTree.getPrevFileNode(targetDir, false);
    }
  } else if (count > 0) {
    const bool wrapInFolder = (ndConfig.get(CFG_REPEAT) == REPEAT_FOLDER);
    targetFile = fileTree.getNextFileNode(ndFile.currentNode, wrapInFolder);
    if (!targetFile && !wrapInFolder) {
      return dirPlay(1);
    }
  } else {
    targetFile = ndFile.currentNode;
  }
  if (!targetFile) {
    return false;
  }
  return _playNode(targetFile);
}

//----------------------------------------------------------------------
// count 個あとのディレクトリを開いて最初のファイルを再生。
// マイナスは前のディレクトリ
// 戻り値: 成功/不成功
bool NDFile::dirPlay(int count) {
  switch (ndConfig.get(CFG_SHUFFLE)) {
    case TRANDOM_FOLDER:
      break;
    case TRANDOM_ALL:
      return _playRandomAll(count);
    default:
      break;
  }

  Node* targetDir = nullptr;
  if (count < 0) {
    targetDir = fileTree.getPrevDirNode(ndFile.currentNode);
  } else if (count > 0) {
    targetDir = fileTree.getNextDirNode(ndFile.currentNode);
  } else {
    targetDir = (ndFile.currentNode && ndFile.currentNode->type == NODE_TYPE_DIR)
                    ? ndFile.currentNode
                    : ndFile.currentNode->parent;
  }

  Node* targetFile = nullptr;
  if (ndConfig.get(CFG_SHUFFLE) == TRANDOM_FOLDER && count != 0 && targetDir &&
      targetDir->fileCount > 0) {
    // フォルダ移動時は、移動先フォルダの直下ファイルからシャッフルに開始する
    int randomIndex = (int)(_nextRandomValue() % (u32_t)targetDir->fileCount);
    targetFile = fileTree.getFileNodeByIndexInDir(targetDir, randomIndex);
  } else {
    targetFile = fileTree.getNextFileNode(targetDir, false);
  }

  if (!targetFile) {
    return false;
  }
  return _playNode(targetFile);
}

// 別タスクからのプレイリクエスト
void NDFile::requestPlay(Node* node) {
  if (node == nullptr || node->type != NODE_TYPE_FILE) return;
  _requestedNode = node;
  _playRequestPending = true;
}

// 別タスクからのプレイリクエスト処理
bool NDFile::processPendingPlayRequest() {
  if (!_playRequestPending || _requestedNode == nullptr) return false;

  Node* requestedNode = _requestedNode;
  Node* prevNode = currentNode;
  ViewMode prevView = disp.currentView;
  bool prevStopTimerDrawing = disp.stopTimerDrawing;
  _requestedNode = nullptr;
  _playRequestPending = false;

  currentNode = requestedNode;
  disp.stopTimerDrawing = true;

  // disp.currentView = ViewMode::Player;
  if (disp.lastView == ViewMode::Visual) {
    disp.currentView = ViewMode::Visual;
  } else {
    disp.currentView = ViewMode::Player;
  }

  if (openFile(fileTree.getFullPath(currentNode))) {
    return true;
  }

  currentNode = prevNode;
  disp.currentView = prevView;
  disp.stopTimerDrawing = prevStopTimerDrawing;
  return false;
}

void NDFile::resetRandomSession() {
  // 設定変更などでシャッフル順のセッションを明示的に破棄する
  _resetRandomState(_folderFileRandomState);
  _resetRandomState(_allFileRandomState);
}

Node* NDFile::_getCurrentDirNode() const {
  if (!currentNode) return nullptr;
  if (currentNode->type == NODE_TYPE_DIR) return currentNode;
  return currentNode->parent;
}

void NDFile::_resetRandomState(RandomSequenceState& state) {
  state = RandomSequenceState();
}

u32_t NDFile::_nextRandomValue() const {
  return esp_random() ^ ((u32_t)micros() << 1);
}

int NDFile::_normalizeModulo(int value, int mod) const {
  if (mod <= 0) return 0;
  int result = value % mod;
  if (result < 0) {
    result += mod;
  }
  return result;
}

u32_t NDFile::_permuteDomainValue(u32_t value, int bits, u32_t salt) const {
  if (bits <= 0) {
    return 0;
  }

  int halfBits = bits / 2;
  u32_t halfMask = (1u << halfBits) - 1u;
  u32_t left = (value >> halfBits) & halfMask;
  u32_t right = value & halfMask;

  for (u32_t round = 0; round < 4; round++) {
    u32_t mix = right;
    mix ^= salt + 0x9e3779b9u * (round + 1u);
    mix *= 0x45d9f3bu;
    mix ^= mix >> 16;
    u32_t next = (left ^ mix) & halfMask;
    left = right;
    right = next;
  }

  return ((left & halfMask) << halfBits) | (right & halfMask);
}

int NDFile::_getPermutationValue(int index, int total, u32_t salt) const {
  if (total <= 1) {
    return 0;
  }

  int bits = 0;
  u32_t domain = 1;
  while ((int)domain < total) {
    domain <<= 1;
    bits++;
  }
  if ((bits & 1) != 0) {
    domain <<= 1;
    bits++;
  }

  // offset を疑似順列に写して、一定刻みではない前後移動を作る
  u32_t value = (u32_t)index;
  while (true) {
    value = _permuteDomainValue(value, bits, salt) & (domain - 1u);
    if ((int)value < total) {
      return (int)value;
    }
  }
}

bool NDFile::_prepareRandomState(RandomSequenceState& state, RandomStateKind kind, Node* scopeNode,
                                 int total, int currentIndex) {
  if (!currentNode || currentNode->type != NODE_TYPE_FILE) return false;
  if (total <= 0 || currentIndex < 0 || currentIndex >= total) return false;

  if (state.kind == kind && state.scopeNode == scopeNode && state.currentFile == currentNode &&
      state.total == total) {
    return true;
  }

  state.kind = kind;
  state.scopeNode = scopeNode;
  state.currentFile = currentNode;
  state.total = total;
  state.offset = 0;
  state.anchorIndex = currentIndex;
  state.salt = _nextRandomValue();
  // 現在曲を起点に prev/next できるよう、基準位置の写像値を保持する
  state.anchorPermutation = _getPermutationValue(0, total, state.salt);
  return true;
}

Node* NDFile::_getNodeFromRandomState(const RandomSequenceState& state, int logicalIndex) const {
  switch (state.kind) {
    case RANDOM_STATE_FOLDER_FILE:
      return fileTree.getFileNodeByIndexInDir(state.scopeNode, logicalIndex);
    case RANDOM_STATE_ALL_FILE:
      return fileTree.getFileNodeByGlobalIndex(logicalIndex);
    default:
      break;
  }
  return nullptr;
}

Node* NDFile::_advanceRandomState(RandomSequenceState& state, int count) {
  if (state.kind == RANDOM_STATE_NONE || state.total <= 0) return nullptr;

  if (count == 0) {
    return currentNode;
  }

  state.offset = _normalizeModulo(state.offset + count, state.total);
  int permuted = _getPermutationValue(state.offset, state.total, state.salt);
  int logicalIndex =
      _normalizeModulo(state.anchorIndex + permuted - state.anchorPermutation, state.total);

  Node* target = _getNodeFromRandomState(state, logicalIndex);
  if (target && target->type == NODE_TYPE_FILE) {
    state.currentFile = target;
  }
  return target;
}

bool NDFile::_playNode(Node* node) {
  if (!node || node->type != NODE_TYPE_FILE) return false;

  Node* prevNode = currentNode;
  currentNode = node;
  browserWindow.onCurrentNodeChanged(prevNode, currentNode);
  return openFile(fileTree.getFullPath(currentNode));
}

bool NDFile::_playRandomFile(int count) {
  Node* dirNode = _getCurrentDirNode();
  if (!dirNode || dirNode->type != NODE_TYPE_DIR || dirNode->fileCount <= 0) {
    _resetRandomState(_folderFileRandomState);
    return false;
  }

  if (count == 0) {
    return _playNode(currentNode);
  }

  int currentIndex = fileTree.getFileIndexInParent(currentNode);
  if (!_prepareRandomState(_folderFileRandomState, RANDOM_STATE_FOLDER_FILE, dirNode,
                           dirNode->fileCount, currentIndex)) {
    return false;
  }

  Node* target = _advanceRandomState(_folderFileRandomState, count);
  if (!target) {
    return false;
  }
  return _playNode(target);
}

bool NDFile::_playRandomAll(int count) {
  if (!currentNode || currentNode->type != NODE_TYPE_FILE) {
    _resetRandomState(_allFileRandomState);
    return false;
  }

  if (count == 0) {
    return _playNode(currentNode);
  }

  int currentIndex = fileTree.getGlobalFileIndex(currentNode);
  if (!_prepareRandomState(_allFileRandomState, RANDOM_STATE_ALL_FILE, fileTree.getRoot(),
                           fileTree.getTotalFiles(), currentIndex)) {
    return false;
  }

  Node* target = _advanceRandomState(_allFileRandomState, count);
  if (!target) {
    return false;
  }
  return _playNode(target);
}

//----------------------------------------------------------------------
// パスでファイルを開く
// 戻り値: 成功/不成功
bool NDFile::openFile(String path) {
  nju72342.mute();
  // 新しい曲を開くときは、前曲の3秒カウントダウンを破棄する。
  cancelPlayHoldCountdown();
  ND::canPlay = false;
  ND::isPaused = false;
  nju72342.resetFadeout();
  ndConfig.saveNodePath(path);

  // int lastSlash = path.lastIndexOf('/');
  // String dirPath = (lastSlash > 0) ? path.substring(0, lastSlash) : "/";
  // ndFile.getAttValueInDir(dirPath);
  nju72342.panMute();

  // 常時PLAY中のVGMから手動で曲を切り替える場合も、ミュート後に物理STOPする。
  if (okim6258.isVgmPcmReencodeActive()) {
    FM.setOKIM6258command(0b00000001, 1);
    okim6258.state.vgmPhysicalPlaying = false;
    ets_delay_us(260);
    okim6258.endVgmPcmReencode();
  }

  // IC_HIGH;

  // Serial.printf("Folder attenuation : %d dB\n", _att);
  if (xSemaphoreTake(spFileOpen, 0) != pdTRUE) {
    // Serial.printf("Semapho is already taken.\n");
    return false;
  }

  FM.reset();
  ndConfig.flushLastView();

  ND::fileFormat = readFile(path);

  if (ND::fileFormat != FileFormat::MDX) {  // MDX->VGM->MDXのとき PDX 上書きされるので
    pdxName = "";
    pdxLoadedName = "";
    pdxSize = 0;
  }

  switch (ND::fileFormat) {
    case FileFormat::VGM: {
      ND::canPlay = vgm.ready();
      break;
    }
    case FileFormat::VGZ: {
      ND::canPlay = vgm.ready();
      break;
    }
    case FileFormat::MDX: {
      ND::canPlay = MDX.ready();
      // ready()中のADPCM無音送信から、通常再生のISR許可へ引き継いだ。
      MDX.adpcmWarmupActive = false;
      break;
    }
    case FileFormat::Unknown: {
      // 不明なファイル
      break;
    }
  }

  if (ND::canPlay && ndConfig.get(CFG_PAUSE) != HOLD_NONE) {
    ND::isPaused = true;
    // HOLD_3SECは開始前の待機表示だけ -0:03 にする。
    playerWindow.dispData.time = ndConfig.get(CFG_PAUSE) == HOLD_3SEC ? -3 : 0;
  }

  nju72342.reset(_att);
  nju72342.panReset();

  xSemaphoreGive(spFileOpen);
  if (!ND::isPaused) {
    nju72342.unmute();
  }

  //
  // エラー表示時の描画再開
  if (visualWindow.visible && ND::canPlay) {
    disp.stopTimerDrawing = false;
    visualWindow.draw();
    if (!ND::isPaused) {
      resetPlaybackStartTime();
    }
  }

  return ND::canPlay;
}

//----------------------------------------------------------------------
// 指定されたディレクトリ内の減衰指定 "att*" ファイルを調べて値を返す
// 戻り値: 0 ~ 24
void NDFile::getAttValueInDir(const String& dirPath) {
  bool isDir;
  _att = 0;

  File dir = SD.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    return;
  }

  while (1) {
    String filePath = dir.getNextFileName(&isDir);
    if (filePath == "") break;

    if (!isDir) {
      String fileName = filePath.substring(filePath.lastIndexOf("/") + 1);
      if (fileName.startsWith("att")) {
        String numberPart = fileName.substring(3);
        u8_t value = numberPart.toInt();
        if (value > 0 && value <= 24) {
          _att = value;
        }
      }
    }
  }
  dir.close();
  return;
}

//----------------------------------------------------------------------
// ヘッダのキャッシュ取得
// true: 成功
bool NDFile::getHeaderCache(String filePath) {
  if (accessMode == ACCESS_PSRAM) {
    // PSRAMモードのとき
    memcpy(header, data, sizeof(header));

  } else if (accessMode == ACCESS_CACHE) {
    // キャッシュモードのとき
    File file = SD.open(filePath);
    if (!file) {
      // Serial.println("getHeaderCache: failed to open file");
      return false;
    }

    if (file.size() < 256) {
      // Serial.println("getHeaderCache: file too small");
      file.close();
      return false;
    }

    file.read(header, 256);
    file.close();
  }

  return true;
}

//----------------------------------------------------------------------
// GD3部分のキャッシュ取得
// 0 = 取得できなかった
u16_t NDFile::getGD3Cache(String filePath, u32_t gd3Offset) {
  if (gd3Offset == 0x14) return 0;  // 0x14 = data offset

  gd3Cache.clear();

  // PSRAM のときはメモリから
  if (accessMode == ACCESS_PSRAM) {
    if (gd3Offset >= size) return 0;
    gd3Cache.assign(data + gd3Offset, data + size);
    return gd3Cache.size();
  }

  // CACHE のときはファイルから
  File file = SD.open(filePath);
  if (!file) return 0;

  const u32_t fileSize = file.size();
  if (gd3Offset >= fileSize) {
    file.close();
    return 0;
  }

  const u32_t readSize = fileSize - gd3Offset;
  gd3Cache.resize(readSize);
  file.seek(gd3Offset);
  const size_t bytesRead = file.read(gd3Cache.data(), readSize);
  file.close();
  gd3Cache.resize(bytesRead);
  return gd3Cache.size();
}

// data access
// 8 bit 返す
u8_t NDFile::get_ui8() {
  u8_t result;

  if (accessMode == ACCESS_PSRAM) {
    result = data[pos++];
  } else {
    result = cache[activeCache][cachePos++];
    pos++;
    if (cachePos == CACHE_SIZE) {
      CacheTaskParam param;
      param.pos = pos;
      param.cacheIndex = activeCache;
      xQueueSend(cacheQueue, &param, 0);

      cachePos = 0;
      activeCache = 1 - activeCache;
    }
  }

  return result;
}

// data access
// 8 bit 返す
// u8_t NDFile::get_ui8() { return data[pos++]; }

// 16 bit 返す
u16_t NDFile::get_ui16() {
  return get_ui8() | (get_ui8() << 8);
}
// 24 bit 返す
u32_t NDFile::get_ui24() {
  return get_ui8() | (get_ui8() << 8) | (get_ui8() << 16);
}
// 32 bit 返す
u32_t NDFile::get_ui32() {
  return get_ui8() | (get_ui8() << 8) | (get_ui8() << 16) | (get_ui8() << 24);
}
// 指定場所の 8 bit 返す
u8_t NDFile::get_ui8_at(u32_t p) {
  return data[p];
}
// 指定場所の 16 bit 返す
u16_t NDFile::get_ui16_at(u32_t p) {
  return (u32_t(data[p])) | (u32_t(data[p + 1]) << 8);
}
// 指定場所の 24 bit 返す
u32_t NDFile::get_ui24_at(u32_t p) {
  return (u32_t(data[p])) + (u32_t(data[p + 1]) << 8) + (u32_t(data[p + 2]) << 16);
}
// 指定場所の 32 bit 返す
u32_t NDFile::get_ui32_at(u32_t p) {
  return (u32_t(data[p])) + (u32_t(data[p + 1]) << 8) + (u32_t(data[p + 2]) << 16) +
         (u32_t(data[p + 3]) << 24);
}

// キャッシュ版
u8_t NDFile::get_ui8_at_header(u32_t p) {
  if (p >= sizeof(header)) {
    // Serial.printf("[WARN] get_ui8_at_header: out of range! p=%u (size=%u)\n", p, sizeof(header));
    return 0;
  }
  return header[p];
}

u16_t NDFile::get_ui16_at_header(u32_t p) {
  if (p + 1 >= sizeof(header)) {
    // Serial.printf("[WARN] get_ui16_at_header: out of range! p=%u (size=%u)\n", p,
    // sizeof(header));
    return 0;
  }
  return (u32_t(header[p])) + (u32_t(header[p + 1]) << 8);
}

u32_t NDFile::get_ui24_at_header(u32_t p) {
  if (p + 2 >= sizeof(header)) {
    // Serial.printf("[WARN] get_ui24_at_header: out of range! p=%u (size=%u)\n", p,
    // sizeof(header));
    return 0;
  }
  return (u32_t(header[p])) + (u32_t(header[p + 1]) << 8) + (u32_t(header[p + 2]) << 16);
}

u32_t NDFile::get_ui32_at_header(u32_t p) {
  if (p + 3 >= sizeof(header)) {
    Serial.printf("[WARN] get_ui32_at_header: out of range! p=%u (size=%u)\n", p, sizeof(header));
    return 0;
  }
  return (u32_t(header[p])) + (u32_t(header[p + 1]) << 8) + (u32_t(header[p + 2]) << 16) +
         (u32_t(header[p + 3]) << 24);
}

/// ビッグエンディアン版
u16_t NDFile::get_ui16_be() {
  return (get_ui8() << 8) | get_ui8();
}
u16_t NDFile::get_ui16_be_at(u32_t p) {
  return (u16_t(data[p + 1])) | (u16_t(data[p]) << 8);
}

// PDX用アクセス (ビッグエンディアン)
u8_t NDFile::get_pdx_ui8_at(u32_t p) {
  return data[PDX_OFFSET + p];
}
u16_t NDFile::get_pdx_ui16_be_at(u32_t p) {
  return (u16_t(data[PDX_OFFSET + p + 1])) | (u16_t(data[PDX_OFFSET + p]) << 8);
}
u32_t NDFile::get_pdx_ui32_be_at(u32_t p) {
  return (u32_t(data[PDX_OFFSET + p + 3])) | (u32_t(data[PDX_OFFSET + p + 2]) << 8) |
         (u32_t(data[PDX_OFFSET + p + 1]) << 16) | (u32_t(data[PDX_OFFSET + p]) << 24);
}

NDFile ndFile = NDFile();

//------------------------------------------------------
// FileTree 保持

#define ND_SD_MOUNTPOINT "/sd"
#define ND_FILETREE_SCAN_PROGRESS_INTERVAL 20

FileTree::FileTree() : _rootNode(nullptr), _totalFiles(0) {
}

FileTree::~FileTree() {
  if (_rootNode) _deleteTree(_rootNode);
}

// PSRAM 用文字列コピー
char* FileTree::_ps_strdup(const char* s) {
  if (s == nullptr) return nullptr;
  size_t len = strlen(s) + 1;
  char* d = (char*)ps_malloc(len);  // PSRAMへ確保
  if (d) memcpy(d, s, len);
  return d;
}

// 対象ファイルか
bool FileTree::_isTargetFile(const char* filename) {
  const char* ext = strrchr(filename, '.');
  if (!ext) return false;
  // 大文字小文字を区別せずに比較
  if (strcasecmp(ext, ".vgm") == 0) return true;
  if (strcasecmp(ext, ".vgz") == 0) return true;
  if (strcasecmp(ext, ".mdx") == 0) return true;
  return false;
}

bool FileTree::begin(const char* rootPath) {
  if (_rootNode) {
    _deleteTree(_rootNode);
    _rootNode = nullptr;
  }

  _totalFiles = 0;
  lcd.print("Scanned files: ");
  scanProgressX = lcd.getCursorX();
  scanProgressY = lcd.getCursorY();
  updateScanProgress(_totalFiles);

  // ルートノード
  _rootNode = (Node*)ps_malloc(sizeof(Node));
  *_rootNode = Node();
  _rootNode->type = NODE_TYPE_DIR;
  _rootNode->name = _ps_strdup("");  // ルートは空名にする
  _rootNode->parent = nullptr;

  // 子を構築
  String scanRootPath = String(ND_SD_MOUNTPOINT);
  if (rootPath && strcmp(rootPath, "/") != 0) {
    if (rootPath[0] != '/') {
      scanRootPath += "/";
    }
    scanRootPath += rootPath;
  }
  Node* children = _buildTree(scanRootPath.c_str(), _rootNode);
  _rootNode->firstChild = children;
  updateScanProgress(_totalFiles);

  return true;
}

bool FileTree::_isPlayableDir(Node* node) const {
  return node && node->type == NODE_TYPE_DIR && node->fileCount > 0;
}

Node* FileTree::_findNextDirSibling(Node* node) const {
  if (!node) return nullptr;

  Node* sibling = node->next;
  while (sibling) {
    if (sibling->type == NODE_TYPE_DIR) {
      return sibling;
    }
    sibling = sibling->next;
  }
  return nullptr;
}

Node* FileTree::_findPrevDirSibling(Node* node) const {
  if (!node) return nullptr;

  Node* sibling = node->prev;
  while (sibling) {
    if (sibling->type == NODE_TYPE_DIR) {
      return sibling;
    }
    sibling = sibling->prev;
  }
  return nullptr;
}

Node* FileTree::_findFirstRootDir() const {
  if (!_rootNode) return nullptr;

  Node* node = _rootNode->firstChild;
  while (node) {
    if (node->type == NODE_TYPE_DIR) {
      return node;
    }
    node = node->next;
  }
  return nullptr;
}

Node* FileTree::_findLastRootDir() const {
  if (!_rootNode) return nullptr;

  Node* node = _rootNode->lastChild;
  while (node) {
    if (node->type == NODE_TYPE_DIR) {
      return node;
    }
    node = node->prev;
  }
  return nullptr;
}

Node* FileTree::findNodeByPath(const String& path) {
  if (!_rootNode || path == "") return nullptr;
  if (path == "/") return _rootNode;
  return _findNodeByPath(_rootNode->firstChild, path);
}

Node* FileTree::_findNodeByPath(Node* node, const String& path) {
  Node* current = node;
  while (current) {
    if (getFullPath(current) == path) {
      return current;
    }
    if (current->firstChild) {
      Node* child = _findNodeByPath(current->firstChild, path);
      if (child) {
        return child;
      }
    }
    current = current->next;
  }
  return nullptr;
}

Node* FileTree::_findFirstPlayableDirFrom(Node* node) const {
  if (!node || node->type != NODE_TYPE_DIR) return nullptr;
  if (_isPlayableDir(node)) {
    return node;
  }
  return _findFirstPlayableDirInSubtree(node->firstChild);
}

Node* FileTree::_findLastPlayableDirFrom(Node* node) const {
  if (!node || node->type != NODE_TYPE_DIR) return nullptr;
  Node* nested = _findLastPlayableDirInSubtree(node->lastChild);
  if (nested) {
    return nested;
  }
  if (_isPlayableDir(node)) {
    return node;
  }
  return nullptr;
}

Node* FileTree::_findFirstPlayableDirInSubtree(Node* node) const {
  Node* child = node;
  while (child) {
    if (child->type == NODE_TYPE_DIR) {
      if (_isPlayableDir(child)) {
        return child;
      }
      Node* nested = _findFirstPlayableDirInSubtree(child->firstChild);
      if (nested) {
        return nested;
      }
    }
    child = child->next;
  }
  return nullptr;
}

Node* FileTree::_findLastPlayableDirInSubtree(Node* node) const {
  Node* child = node;
  while (child) {
    if (child->type == NODE_TYPE_DIR) {
      Node* nested = _findLastPlayableDirInSubtree(child->lastChild);
      if (nested) {
        return nested;
      }
      if (_isPlayableDir(child)) {
        return child;
      }
    }
    child = child->prev;
  }
  return nullptr;
}

Node* FileTree::getNextDirNode(Node* node) {
  if (!_rootNode) return nullptr;

  if (!node || node == _rootNode) {
    return _findFirstPlayableDirFrom(_rootNode);
  }

  if (node->type == NODE_TYPE_DIR) {
    Node* childDir = _findFirstPlayableDirInSubtree(node->firstChild);
    if (childDir) {
      return childDir;
    }
  }

  Node* cursor = node;
  while (cursor) {
    Node* candidate = _findNextDirSibling(cursor);
    if (candidate) {
      return _findFirstPlayableDirFrom(candidate);
    }

    Node* parent = cursor->parent;
    if (!parent) {
      break;
    }
    if (parent == _rootNode) {
      return _findFirstPlayableDirFrom(_rootNode);
    }
    cursor = parent;
  }

  return nullptr;
}

Node* FileTree::getPrevDirNode(Node* node) {
  if (!_rootNode) return nullptr;

  if (node && node->type == NODE_TYPE_FILE) {
    node = node->parent;
  }

  if (!node || node == _rootNode) {
    return _findLastPlayableDirFrom(_findLastRootDir());
  }

  Node* cursor = node;
  while (cursor) {
    Node* candidate = _findPrevDirSibling(cursor);
    if (candidate) {
      return _findLastPlayableDirFrom(candidate);
    }

    Node* parent = cursor->parent;
    if (!parent) {
      break;
    }
    if (_isPlayableDir(parent)) {
      return parent;
    }
    if (parent == _rootNode) {
      return _findLastPlayableDirFrom(_findLastRootDir());
    }
    cursor = parent;
  }

  return nullptr;
}

int FileTree::getFileIndexInParent(Node* node) const {
  if (!node || node->type != NODE_TYPE_FILE || !node->parent) {
    return -1;
  }

  int index = 0;
  Node* sibling = node->parent->firstChild;
  while (sibling) {
    if (sibling->type == NODE_TYPE_FILE) {
      if (sibling == node) {
        return index;
      }
      index++;
    }
    sibling = sibling->next;
  }

  return -1;
}

Node* FileTree::getFileNodeByIndexInDir(Node* dir, int index) const {
  if (!dir || dir->type != NODE_TYPE_DIR || index < 0) {
    return nullptr;
  }

  int currentIndex = 0;
  Node* child = dir->firstChild;
  while (child) {
    if (child->type == NODE_TYPE_FILE) {
      if (currentIndex == index) {
        return child;
      }
      currentIndex++;
    }
    child = child->next;
  }
  return nullptr;
}

bool FileTree::_findGlobalFileIndex(Node* node, Node* target, int& index) const {
  Node* current = node;
  while (current) {
    if (current->type == NODE_TYPE_FILE) {
      if (current == target) {
        return true;
      }
      index++;
    } else if (current->subtreeFileCount > 0) {
      if (_findGlobalFileIndex(current->firstChild, target, index)) {
        return true;
      }
    }
    current = current->next;
  }
  return false;
}

int FileTree::getGlobalFileIndex(Node* node) const {
  if (!node || node->type != NODE_TYPE_FILE || !_rootNode) {
    return -1;
  }

  int index = 0;
  return _findGlobalFileIndex(_rootNode->firstChild, node, index) ? index : -1;
}

Node* FileTree::_findFileNodeByGlobalIndex(Node* node, int& index) const {
  Node* current = node;
  while (current) {
    if (current->type == NODE_TYPE_FILE) {
      if (index == 0) {
        return current;
      }
      index--;
    } else if (current->subtreeFileCount > 0) {
      if (index < current->subtreeFileCount) {
        return _findFileNodeByGlobalIndex(current->firstChild, index);
      }
      index -= current->subtreeFileCount;
    }
    current = current->next;
  }
  return nullptr;
}

Node* FileTree::getFileNodeByGlobalIndex(int index) const {
  if (!_rootNode || index < 0 || index >= _totalFiles) {
    return nullptr;
  }

  return _findFileNodeByGlobalIndex(_rootNode->firstChild, index);
}

Node* FileTree::getNextFileNode(Node* node, bool wrap) {
  if (!node) return nullptr;

  if (node->type == NODE_TYPE_DIR) {
    Node* child = node->firstChild;
    while (child) {
      if (child->type == NODE_TYPE_FILE) {
        return child;
      }
      child = child->next;
    }
    return nullptr;
  }

  Node* sibling = node->next;
  while (sibling) {
    if (sibling->type == NODE_TYPE_FILE) {
      return sibling;
    }
    sibling = sibling->next;
  }

  if (!wrap) return nullptr;

  Node* parent = node->parent;
  if (!parent) return nullptr;

  sibling = parent->firstChild;
  while (sibling && sibling != node) {
    if (sibling->type == NODE_TYPE_FILE) {
      return sibling;
    }
    sibling = sibling->next;
  }

  return (node->type == NODE_TYPE_FILE) ? node : nullptr;
}

Node* FileTree::getPrevFileNode(Node* node, bool wrap) {
  if (!node) return nullptr;

  if (node->type == NODE_TYPE_DIR) {
    Node* child = node->lastChild;
    while (child) {
      if (child->type == NODE_TYPE_FILE) {
        return child;
      }
      child = child->prev;
    }
    return nullptr;
  }

  Node* sibling = node->prev;
  while (sibling) {
    if (sibling->type == NODE_TYPE_FILE) {
      return sibling;
    }
    sibling = sibling->prev;
  }

  if (!wrap) return nullptr;

  Node* parent = node->parent;
  if (!parent) return nullptr;

  sibling = parent->lastChild;
  while (sibling && sibling != node) {
    if (sibling->type == NODE_TYPE_FILE) {
      return sibling;
    }
    sibling = sibling->prev;
  }

  return (node->type == NODE_TYPE_FILE) ? node : nullptr;
}

// 再帰走査
Node* FileTree::_buildTree(const char* path, Node* parent) {
  DIR* dir = opendir(path);
  if (!dir) {
    return nullptr;
  }
  rewinddir(dir);

  Node* fileHead = nullptr;
  Node* fileTail = nullptr;
  Node* dirHead = nullptr;
  Node* dirTail = nullptr;

  struct PendingDirEntry {
    String fullPath;
    String baseName;
  };

  std::vector<String> targetFiles;
  std::vector<PendingDirEntry> childDirs;
  String snapDirPath;

  auto appendNode = [](Node*& head, Node*& tail, Node* node) {
    if (!head) {
      head = node;
    }
    if (tail) {
      tail->next = node;
      node->prev = tail;
    }
    tail = node;
  };

  auto getBaseNameWithoutExt = [](const String& name) {
    int dotPos = name.lastIndexOf('.');
    return (dotPos == -1) ? name : name.substring(0, dotPos);
  };
  auto isNumericName = [](const String& name) {
    if (name.length() == 0) return false;
    for (size_t i = 0; i < name.length(); i++) {
      char c = name.charAt(i);
      if (c < '0' || c > '9') return false;
    }
    return true;
  };

  while (true) {
    struct dirent* entry = readdir(dir);
    if (!entry) break;  // 終了

    String baseName = String(entry->d_name);

    // 不要な隠しファイルやシステムフォルダを除外
    if (baseName.startsWith(".") || baseName.equalsIgnoreCase("System Volume Information") ||
        baseName.equalsIgnoreCase("__MACOSX")) {
      continue;
    }

    if (entry->d_type == DT_DIR) {
      if (baseName.equalsIgnoreCase("snap")) {
        snapDirPath = String(path);
        if (!snapDirPath.endsWith("/")) {
          snapDirPath += "/";
        }
        snapDirPath += baseName;
      } else {
        String fullPath = String(path);
        if (!fullPath.endsWith("/")) {
          fullPath += "/";
        }
        fullPath += baseName;
        childDirs.push_back({fullPath, baseName});
      }
    } else {
      const char* ext = strrchr(baseName.c_str(), '.');
      if (ext && strcasecmp(ext, ".png") == 0) {
        if (parent->pngName) free(parent->pngName);
        parent->pngName = _ps_strdup(baseName.c_str());
      }

      if (_isTargetFile(baseName.c_str())) {
        targetFiles.push_back(baseName);
      }
    }
  }
  closedir(dir);

  std::vector<Node*> fileNodes;
  for (const String& fileName : targetFiles) {
    Node* fileNode = (Node*)ps_malloc(sizeof(Node));
    *fileNode = Node();
    fileNode->type = NODE_TYPE_FILE;
    fileNode->name = _ps_strdup(fileName.c_str());
    fileNode->parent = parent;
    fileNode->subtreeFileCount = 1;
    fileNodes.push_back(fileNode);
    appendNode(fileHead, fileTail, fileNode);
    parent->fileCount++;
    _totalFiles++;
    if ((_totalFiles % ND_FILETREE_SCAN_PROGRESS_INTERVAL) == 0) {
      updateScanProgress(_totalFiles);
    }
  }

  if (snapDirPath.length() > 0 && !fileNodes.empty()) {
    struct PendingSnapMatch {
      String stem;
      String fileName;
      int index;
      bool isIndex;
    };

    std::vector<PendingSnapMatch> snapMatches;
    DIR* snapDir = opendir(snapDirPath.c_str());
    if (snapDir) {
      while (true) {
        struct dirent* snapEntry = readdir(snapDir);
        if (!snapEntry) break;
        if (snapEntry->d_type == DT_DIR) continue;

        String snapBaseName = String(snapEntry->d_name);
        const char* snapExt = strrchr(snapBaseName.c_str(), '.');
        if (!snapExt || strcasecmp(snapExt, ".png") != 0) {
          continue;
        }

        String snapStem = getBaseNameWithoutExt(snapBaseName);
        PendingSnapMatch match = {snapStem, snapBaseName, 0, false};
        if (isNumericName(snapStem)) {
          match.index = snapStem.toInt();
          match.isIndex = true;
        }
        snapMatches.push_back(match);
      }
      closedir(snapDir);
    }

    for (Node* fileNode : fileNodes) {
      String fileStem = getBaseNameWithoutExt(String(fileNode->name));
      for (const PendingSnapMatch& match : snapMatches) {
        if (!match.isIndex && strcasecmp(match.stem.c_str(), fileStem.c_str()) == 0) {
          if (fileNode->pngName) free(fileNode->pngName);
          fileNode->pngName = _ps_strdup(match.fileName.c_str());
          break;
        }
      }
    }

    for (const PendingSnapMatch& match : snapMatches) {
      if (!match.isIndex || match.index <= 0 || match.index > (int)fileNodes.size()) {
        continue;
      }
      Node* fileNode = fileNodes[match.index - 1];
      if (fileNode->pngName == nullptr) {
        fileNode->pngName = _ps_strdup(match.fileName.c_str());
      }
    }
  }

  for (const PendingDirEntry& childDir : childDirs) {
    Node* dirNode = (Node*)ps_malloc(sizeof(Node));
    *dirNode = Node();
    dirNode->type = NODE_TYPE_DIR;
    dirNode->name = _ps_strdup(childDir.baseName.c_str());
    dirNode->parent = parent;

    Node* subChild = _buildTree(childDir.fullPath.c_str(), dirNode);
    if (subChild == nullptr) {
      free(dirNode->name);
      if (dirNode->pngName) free(dirNode->pngName);
      free(dirNode);
      continue;
    }

    dirNode->firstChild = subChild;
    appendNode(dirHead, dirTail, dirNode);
    parent->dirCount++;
  }

  parent->subtreeFileCount = parent->fileCount;
  for (Node* child = dirHead; child != nullptr; child = child->next) {
    parent->subtreeFileCount += child->subtreeFileCount;
  }

  Node* firstChild = fileHead ? fileHead : dirHead;
  Node* lastChild = dirTail ? dirTail : fileTail;

  if (fileTail && dirHead) {
    fileTail->next = dirHead;
    dirHead->prev = fileTail;
  }

  parent->firstChild = firstChild;
  parent->lastChild = lastChild;

  return firstChild;
}

String FileTree::getFullPath(Node* node) {
  if (!node) return "";

  String path = "";
  Node* current = node;

  while (current && current->parent) {  // ルートはスキップ
    String name = String(current->name);
    if (path == "")
      path = name;
    else
      path = name + "/" + path;
    current = current->parent;
  }

  return "/" + path;
}

void FileTree::_deleteTree(Node* node) {
  while (node) {
    Node* nextSibling = node->next;
    if (node->firstChild) _deleteTree(node->firstChild);  // 子を先に消去
    if (node->name) free(node->name);
    if (node->pngName) free(node->pngName);
    free(node);
    node = nextSibling;
  }
}

FileTree fileTree = FileTree();
