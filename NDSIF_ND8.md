# ND8のNDSIF正式v1草案対応

更新日: 2026-10-20。実装API: `1.0` / revision `39`。正式v1は策定中であり、この実装は草案の一部を先行実装する。ファームウェアの識別はGET_INFOで確認する。

シリアルモードは [API草案](NDSIF_API.md) の9 byteヘッダー、message_type、明示instance・pageを使用する。wire版の候補値は `01`。ホストはこのフレームと対応情報に従って使用する。

| 要求 | ND8での対応 |
| --- | --- |
| `54` WRITE_YM2151 | instance=0、page=0、1～127組、常に無応答BURST |
| `04` SET_CHIP_CLOCK | write_id=`54` / `B7`、instance=0。論理クロックHzを指定 |
| `05` GET_CAPABILITIES | YM2151のBURST・位置付きイベント・クロック、OKIのクロック・ストリームを通知 |
| `10` AUDIO_DATA | stream_id=`B7`、instance=0、最大250 ADPCM byte |
| `11` AUDIO_EVENT | 同じストリーム。レジスタイベントの対象はYM2151・instance=0・page=0 |
| `12` AUDIO_START | 同じストリーム、4 / 8MHz、divider=512 / 768 / 1024 |
| `13` AUDIO_STATUS | 同じストリーム、status + 10個のu32 |

データ容量4096 byte、イベント容量1024レジスタ組。レジスタイベントは全組を検証してから登録する。ND8にない音源・個体・ページの書き込みと予約IDは実行せず、書き込み応答も返さない。

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
Capabilityのentryは9 byte。API文字列は1.0、REVISIONは39。文書版0.40の設計方針追加によるwire・ファームウェアの変更はない。

## 即時演奏と位置付き再生の確認範囲

YM2151の即時書き込みは、MIDI鍵盤から実機シンセのような応答で演奏できることをユーザーが確認済み。これは定性的な演奏感の確認であり、押鍵から音声出力までのms単位の測定値ではない。即時経路には演奏用の固定先行待ちを設けない。

位置付きYM2151イベントとADPCMの同期処理は、現在はVGM再生に使用する。ホストがイベントを対応データより先に送り、ND8のADPCM供給済みbyte位置に従って適用する。イベントはタスク側で処理するため、指定位置ちょうどの物理書き込みは保証せず、late_events/max_event_lagで遅れを観測する。

## YM2612時間付きDACプロファイルとの関係

共通通信コアはrevision 40だが、ND8はYM2612時間付きDAC profile 1を搭載しない。プロファイルの仕様は `NDSIF_API.md` 第9節を参照する。ND8の既存 `B7` ストリーム、ADPCM byte位置、DATA/EVENT/START/STATUSのpayloadは維持する。

ND6向けフェーズ1では、この位置付き先行供給の方式を基準として、ホストで正規化したYM2612のDAC値＋waitとFM・PSGを同じVGM時間軸で安定再生する。ND6への実装や新プロファイルへのND8対応が完了したという意味ではない。MIDI鍵盤のリアルタイムPCMドラム合成・送信と低遅延評価はフェーズ2とし、既存YM2151の即時演奏とは検証範囲を分ける。
