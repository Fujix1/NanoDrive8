# ND8のNDSIF正式v1草案対応

ファームウェア: `1.0b11`。正式v1は策定中であり、この実装は草案の一部を先行実装する。

シリアルモードは [API草案](NDSIF_API.md) の9 byteヘッダー、message_type、明示instance・pageを使用する。wire版は `01` だが、以前のb版の8 byteヘッダーとは互換性がない。ホスト側も新形式に変更する必要があり、旧形式の自動判別は行わない。

| 要求 | ND8での対応 |
| --- | --- |
| `54` WRITE_YM2151 | instance=0、page=0、1～127組、常に無応答BURST |
| `04` SET_CHIP_CLOCK | write_id=`54` / `B7`、instance=0。論理クロックHzを指定 |
| `05` GET_CAPABILITIES | YM2151のBURST・位置付きイベント・クロック、OKIのクロック・ストリームを通知 |
| `10` AUDIO_DATA | stream_id=`B7`、instance=0、最大250 ADPCM byte |
| `11` AUDIO_EVENT | 同じストリーム。レジスタイベントの対象はYM2151・instance=0・page=0 |
| `12` AUDIO_START | 同じストリーム、4 / 8MHz、divider=512 / 768 / 1024 |
| `13` AUDIO_STATUS | 同じストリーム、status + 10個のu32 |

データ容量4096 byte、イベント容量1024レジスタ組。レジスタイベントは全組を検証してから登録する。ND8にない音源・個体・ページの書き込みと予約IDは実行せず、書き込み応答も返さない。`56`を旧YM2151バーストとして扱わず、`57`～`5B`も旧制御・ADPCM要求として扱わない。

RESET時のミュート、OKIのデコード開始と無音供給、出力安定待ちを維持する。音楽データの消費はAUDIO_STARTから開始する。画面操作時の受信・供給処理と異常時停止の対策も維持する。

更新はファームのソースとビルド成果物まで。実機への書き込み・音声確認は別途行う。

## クロック構成情報（API revision 39）

GET_CAPABILITIESのentryは9 byteへ変更し、clock_modeとfixed_clock_hzを返す。
ND8の現在の構成はYM2151、OKIM6258ともSi5351出力を使用するためvariable（02）、fixed_clock_hz=0。
これは設定されたクロックスロットに基づく通知で、ICの自動検出や実周波数の読戻しではない。
YM2151は従来のnormalizeFreqで設定可能な値、OKIM6258は4/8MHzの範囲で変更する。

音源一覧はProtocol内の固定byte配列から、ボード側の`src/serialman.cpp`が渡すCapabilityテーブルへ移した。
別の固定クロック構成ではCLOCK_FIXEDと実際の論理入力Hzを設定し、featuresのクロック設定bitを外す。
固定周波数が不明な構成を推測値で埋めず、CLOCK_UNKNOWN/0を使う。
4 byteの旧entry形式はサポートしない。API文字列は1.0、REVISIONは39。Delphiホストも同時に更新する。
