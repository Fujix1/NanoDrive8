#ifndef FILE_H
#define FILE_H

#include <FS.h>
#include <SD.h>

#include "NJU72342.h"
#include "common.h"
#include "disp.h"
#include "fm.h"
#include "nd.h"

#define CACHE_SIZE (128 * 1024)
#define NUM_CACHE 2

struct Node;

// extern u8_t cache[NUM_CACHE][CACHE_SIZE] __attribute__((aligned(4)));
extern u8_t* cache[NUM_CACHE];  // PSRAM用キャッシュ

// 読み込みキャッシュ
extern volatile int activeCache;  // アクティブなキャッシュ
extern volatile int cachePos;     // キャッシュ内の位置

void fillCacheTask(void* pvParameters);
bool initCache(String path);

enum AccessMode { ACCESS_PSRAM, ACCESS_CACHE };  // アクセスモード PSRAM に全部入れる | 逐次

class NDFile {
 public:
  bool init();
  FileFormat readFile(String path);
  bool filePlay(int count);
  bool dirPlay(int count);
  bool openFile(String path);
  void resetRandomSession();
  void requestPlay(Node* node);
  bool processPendingPlayRequest();
  Node* currentNode;

  void getAttValueInDir(const String& dirPath);  // フォルダの音量減衰取得

  u8_t* data;  // データ本体
  u32_t pos;   // データ位置
  u32_t size;  // ファイルサイズ

  u8_t get_ui8();
  u16_t get_ui16();
  u32_t get_ui24();
  u32_t get_ui32();
  u8_t get_ui8_at(u32_t p);
  u16_t get_ui16_at(u32_t p);
  u32_t get_ui24_at(u32_t p);
  u32_t get_ui32_at(u32_t p);

  // キャッシュ対応版
  u8_t get_ui8_at_header(u32_t p);
  u16_t get_ui16_at_header(u32_t p);
  u32_t get_ui24_at_header(u32_t p);
  u32_t get_ui32_at_header(u32_t p);

  AccessMode accessMode;

  u8_t header[256] __attribute__((aligned(4)));  // ヘッダのキャッシュ
  std::vector<u8_t> gd3Cache;                    // GD3部分のキャッシュ

  boolean getHeaderCache(String filePath);  // ヘッダキャッシュ取得

  u16_t getGD3Cache(String filePath, u32_t gd3Offset);  // GD3部分のキャッシュを取得する

  // ビッグエンディアン版
  u16_t get_ui16_be();
  u16_t get_ui16_be_at(u32_t p);

  String pdxName;        // MDX が要求する PDX 名
  String pdxLoadedName;  // 実際にロード済みの PDX 名
  u32_t pdxPos;          // PDX ポインタ
  u32_t pdxSize;         // PDX のサイズ

  u32_t readPDX(String name);

  // PDX用アクセス (ビックエンディアン)
  u8_t get_pdx_ui8_at(u32_t p);
  u16_t get_pdx_ui16_be_at(u32_t p);
  u32_t get_pdx_ui32_be_at(u32_t p);

 private:
  enum RandomStateKind : u8_t {
    RANDOM_STATE_NONE,
    RANDOM_STATE_FOLDER_FILE,
    RANDOM_STATE_ALL_FILE,
  };

  struct RandomSequenceState {
    RandomStateKind kind;
    Node* scopeNode;
    Node* currentFile;
    int total;
    int offset;
    int anchorIndex;
    int anchorPermutation;
    u32_t salt;

    RandomSequenceState()
        : kind(RANDOM_STATE_NONE),
          scopeNode(nullptr),
          currentFile(nullptr),
          total(0),
          offset(0),
          anchorIndex(0),
          anchorPermutation(0),
          salt(0) {
    }
  };

  u8_t _att;  // 現在のフォルダの全体減衰量 db
  volatile bool _playRequestPending = false;
  Node* _requestedNode = nullptr;
  RandomSequenceState _folderFileRandomState;
  RandomSequenceState _allFileRandomState;

  Node* _getCurrentDirNode() const;
  bool _playNode(Node* node);
  bool _playRandomFile(int count);
  bool _playRandomAll(int count);
  Node* _advanceRandomState(RandomSequenceState& state, int count);
  bool _prepareRandomState(RandomSequenceState& state, RandomStateKind kind, Node* scopeNode,
                           int total, int currentIndex);
  Node* _getNodeFromRandomState(const RandomSequenceState& state, int logicalIndex) const;
  int _normalizeModulo(int value, int mod) const;
  int _getPermutationValue(int index, int total, u32_t salt) const;
  u32_t _permuteDomainValue(u32_t value, int bits, u32_t salt) const;
  u32_t _nextRandomValue() const;
  void _resetRandomState(RandomSequenceState& state);
};

extern NDFile ndFile;

//------------------------------------------------------
// FileTree 保持
enum NodeType : u8_t { NODE_TYPE_DIR, NODE_TYPE_FILE };

// ファイルノード
struct Node {
  NodeType type;
  char* name;            // ファイル名 PSRAM配置
  char* pngName;         // ディレクトリ既定 or ファイル固有のpngファイル名 PSRAM配置
  Node* parent;          // 親ディレクトリ
  Node* firstChild;      // 最初の子ノード
  Node* lastChild;       // 最後の子ノード
  Node* prev;            // 前の兄弟
  Node* next;            // 次の兄弟
  int fileCount;         // ディレクトリ直下の有効ファイル数
  int dirCount;          // ディレクトリ直下の有効ディレクトリ数
  int subtreeFileCount;  // 自ノード配下の全有効ファイル数

  // デフォルト初期化用
  Node()
      : type(NODE_TYPE_FILE),
        name(nullptr),
        pngName(nullptr),
        parent(nullptr),
        firstChild(nullptr),
        lastChild(nullptr),
        prev(nullptr),
        next(nullptr),
        fileCount(0),
        dirCount(0),
        subtreeFileCount(0) {
  }
};

class FileTree {
 public:
  FileTree();
  ~FileTree();

  bool begin(const char* rootPath);  // キャッシュの構築開始
  String getFullPath(Node* node);    // 現在のノードからフルパスを生成（再生時に使用）
  Node* findNodeByPath(const String& path);
  Node* getNextDirNode(Node* node);  // 次の再生可能ディレクトリを取得
  Node* getPrevDirNode(Node* node);  // 前の再生可能ディレクトリを取得

  Node* getNextFileNode(Node* node, bool wrap);  // 次のファイルを取得
  Node* getPrevFileNode(Node* node, bool wrap);  // 前のファイルを取得
  int getFileIndexInParent(Node* node) const;    // 親ディレクトリ内でのファイル順インデックスを取得
  Node* getFileNodeByIndexInDir(Node* dir, int index) const;  // 直下ファイルの n 番目を取得
  int getGlobalFileIndex(Node* node) const;                   // 全ファイル順でのインデックスを取得
  Node* getFileNodeByGlobalIndex(int index) const;            // 全ファイル順での n 番目を取得

  Node* getRoot() const {
    return _rootNode;
  }  // ルートノード取得
  int getTotalFiles() const {
    return _totalFiles;
  }  // 合計ファイル数取得

 private:
  Node* _rootNode;
  int _totalFiles;

  bool _isPlayableDir(Node* node) const;
  Node* _findNextDirSibling(Node* node) const;
  Node* _findPrevDirSibling(Node* node) const;
  Node* _findFirstRootDir() const;
  Node* _findLastRootDir() const;
  Node* _findNodeByPath(Node* node, const String& path);
  Node* _findFirstPlayableDirFrom(Node* node) const;
  Node* _findLastPlayableDirFrom(Node* node) const;
  Node* _findFirstPlayableDirInSubtree(Node* node) const;
  Node* _findLastPlayableDirInSubtree(Node* node) const;
  Node* _findFileNodeByGlobalIndex(Node* node, int& index) const;
  bool _findGlobalFileIndex(Node* node, Node* target, int& index) const;
  Node* _buildTree(
      const char* path,
      Node* parent);  // 再帰的にディレクトリを走査し、有効なファイルがある場合のみノードを作成
  bool _isTargetFile(const char* filename);  // 指定されたファイル名が対象（.vgm, .vgz, .mdx）か判定
  char* _ps_strdup(const char* s);           // PSRAM用文字列コピー
  void _deleteTree(Node* node);              // メモリ解放（デストラクタ用）
};

extern FileTree fileTree;

#endif
