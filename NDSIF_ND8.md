# NanoDrive 8 NDSIF 実装

更新日 2026-11-20。FW `1.0.6`。API `1.0` / 共通コア revision `40`。GET_INFOは `NanoDrive 8` とFW文字列を返す。

シリアルモードは9 byteヘッダーのNDSIFフレーム版 `01` を使用する。共通仕様は `NDSIF.md`、wire仕様は `NDSIF_API.md`、ホスト手順は `NDSIF_HOST_GUIDE.md` を参照する。

| 要求 | ND8での対応 |
| --- | --- |
| `00`～`03` | RESET、PING、GET_INFO、SET_OUTPUT_VOLUMEに対応 |
| `54` WRITE_YM2151 | instance=0、page=0、1～127組、常に無応答BURST |
| `04` SET_CHIP_CLOCK | write_id=`54` / `B7`、instance=0。論理クロックHzを指定 |
| `05` GET_CAPABILITIES | YM2151のBURST・位置付きイベント・クロック、OKIのクロック・ストリームを通知 |
| `10` AUDIO_DATA | stream_id=`B7`、instance=0、最大250 ADPCM byte |
| `11` AUDIO_EVENT | 同じストリーム。レジスタイベントの対象はYM2151・instance=0・page=0 |
| `12` AUDIO_START | 同じストリーム、4 / 8MHz、divider=512 / 768 / 1024 |
| `13` AUDIO_STATUS | 同じストリーム、status + 10個のu32 |

データ容量4096 byte、イベント容量1024レジスタ組。レジスタイベントは全組を検証してから登録する。ND8にない音源・個体・ページの書き込みと予約IDは実行せず、書き込み応答も返さない。

RESET時のミュート、OKIのデコード開始と無音供給、出力安定待ちを維持する。音楽データの消費はAUDIO_STARTから開始する。画面操作時の受信・供給処理と異常時停止の対策も維持する。

## クロック構成情報

GET_CAPABILITIESのentryは9 byteで、clock_modeとfixed_clock_hzを返す。
ND8の現在の構成はYM2151、OKIM6258ともSi5351出力を使用するためvariable（02）、fixed_clock_hz=0。
これは設定されたクロックスロットに基づく通知で、ICの自動検出や実周波数の読戻しではない。
YM2151はnormalizeFreqで設定可能な値（4,026,987 Hzを含む）、OKIM6258は4/8MHzの範囲で変更する。

音源一覧は `src/serialman.cpp` のCapabilityテーブルで構成する。YM2151は即時BURST・位置付きイベント、MSM6258はADPCMストリームを通知し、クロック設定bitは設定された可変クロックスロットに応じて加える。
別の固定クロック構成ではCLOCK_FIXEDと実際の論理入力Hzを設定し、featuresのクロック設定bitを外す。
固定周波数が不明な構成を推測値で埋めず、CLOCK_UNKNOWN/0を使う。

## 即時演奏と位置付き再生の確認範囲

YM2151の即時書き込みは、MIDI鍵盤から実機シンセのような応答で演奏できることをユーザーが確認済み。これは定性的な演奏感の確認であり、押鍵から音声出力までのms単位の測定値ではない。即時経路には演奏用の固定先行待ちを設けない。

位置付きYM2151イベントとADPCMの同期処理は、現在はVGM再生に使用する。ホストがイベントを対応データより先に送り、ND8のADPCM供給済みbyte位置に従って適用する。イベントはタスク側で処理するため、指定位置ちょうどの物理書き込みは保証せず、late_events/max_event_lagで遅れを観測する。

ND8で使う任意ストリームは `NDSIF_API.md` 第8節のMSM6258互換ADPCM。データ位置とイベント位置はADPCM byte単位で扱う。
