# MAME → NanoDrive ブリッジ

MAME のエミュレーション音を残したまま、対応する音源への書き込みを NDSIF 経由で NanoDrive に送ります。Windows、Python 3、`pyserial` が必要です。

```powershell
python -m pip install pyserial
```

## 起動

`NanoDriveBridge.vbs` をダブルクリックすると画面が開きます。COM ポートは接続中の一覧から選び、抜き差ししたときは「更新」で再取得できます。選んだ機器と COM ポートは、ブリッジと同じフォルダーの `setting.ini` に保存し、次回起動時に復元します。最初の既定値は ND6 / COM5 です。前回のポートが見つからないときは一覧の先頭を選びます。起動時にポートがあれば自動的に開始し、ポートがないときは選択と開始を無効にします。接続後に「更新」を押してください。変更するときは一度停止してください。

`pythonw.exe` が PATH にある必要があります。`.pyw` が Python に関連付けられている環境では `NanoDriveBridge.pyw` も直接起動できます。

コマンドラインで起動する場合の既定値も ND6 / COM5 です。`bridge.py` のあるフォルダーを PowerShell で開き、実行します。

```powershell
python .\bridge.py
```

MAME の終了後もブリッジは次の接続を待ちます。接続中に別の MAME が接続した場合、先の接続を維持し、後の接続を切断します。切断された側の MAME プラグインはブリッジに再接続しないため、先の MAME の終了後にその MAME を再起動してください。ブリッジ終了時は画面の「停止」または Ctrl+C を使用します。

ND6 の既定値は順序付き方式、まとめ送り 5 ms / 100 件です。再生中にバーストごとの応答や状態照会を待ちません。`--batch-ms` / `--batch-events` で調整できます。`--device nd8` の既定ポートは COM3、`--port COM番号` で指定を変更できます。`--once` は MAME １回の終了後に終了します。実験用の従来方式は `--live-dac` で選べますが、FM・DAC の実行順序と PSG 転送は保証されません。

順序付き方式では、MAME の時間が PC の実時間より 10 ms を超えて先行すると、先行が約 2 ms に戻るまでの区間を飛ばします。区間内の DAC 値は捨て、FM・DAC 有効切替・PSG の書き込みは順番に残して待ちを短縮します。PCM は飛ばした部分が欠け、FM・PSG は一時的に密集する場合があります。ポーズや長い入力停止の後は比較の基準を取り直します。MAME 停止時に `ND6 clock skip` として短縮した時間と捨てた DAC 件数を表示します。これは PC 側の時計からの推定で、ND6 内部の実際の遅れは直接測れません。ND6 に送信済みのデータは削除できないため、即時同期は保証されません。

早送り中は DAC データをプラグイン側で送らず、FM・DAC 有効切替・PSG は順番を維持して待ちなしで送ります。MAME のスロットル無効、または実測速度 120% 超を検出すると有効になり、通常速度へ戻ると DAC 送信を再開します。早送りキーの状態は直接読まず、実際に速度が上がったことを見て判定します。検出・解除には MAME のフレーム通知や速度平均の更新まで遅れがあり、既に送信済みのデータは消せません。`ND6 fast-forward` のログで切り替えを確認できます。この機能には `nanodrive6` プラグイン 0.3.0 と対応するブリッジの両方が必要です。

## Mega Drive / Genesis と Sega System C/C2 → ND6

ND6 を NDSIF モードにします。順序付き PSG の転送には firmware 3.1dev25 以降が必要です。`plugins/nanodrive6` と `plugins/nanodrive8` を MAME の `plugins` フォルダーに配置してください。例として、ブリッジ起動後、MAME フォルダーを別の PowerShell で開き、`sonic2` を実行します。

```powershell
.\mame.exe megadrij -cart sonic2 -noreadconfig -rompath 'roms;software' -plugins -plugin nanodrive6 -video gdi -window
```

`sega/mdconsole.cpp` と `sega/segac2.cpp` を対象に、YM2612 / YM3438 の FM・DAC と VDP 内蔵 PSG を同じ順序付き FIFO に送ります。PSG は ND6 で Sega VDP 用に変換して SN76489 に出力します。NTSC / PAL のクロックを切り替えます。System C2 の uPD7759 は MAME の音声のみです。

リセット時には ND6 をリセットし、対応機ではポーズ中に出力をミュートします。キューが空になった場合は到着を待ちます。MAME の速度、USB や実チップの処理負荷による音の遅れや乱れは残り得ます。セーブステートや途中接続からの状態復元には対応していません。

## Sega System 16A / 16B → ND8

ND8 を NDSIF モードにして画面の機器を ND8 に変更するか、`bridge.py` のあるフォルダーから以下で起動します。

```powershell
python .\bridge.py --device nd8 --port COM3
```

ドライバー名を限定せず YM2151 を探し、CPU の 8 bit または 16 bit 空間に公開された直接書き込みマップを捕捉します。ブリッジ起動後、MAME フォルダーを PowerShell で開き、例として次を実行します。

```powershell
.\mame.exe fantzone -noreadconfig -rompath 'roms;software' -plugins -plugin nanodrive8 -video gdi -window
```

MAME の起動・リセット時に ND8 をリセットし、音量制御に対応する機体ではポーズ時にミュートします。ポーズ解除後にチップ状態を復元する処理は行いません。

YM2151 のクロックは、起動中の MAME と同じ実行ファイルの `-listxml` からマシン名とデバイスタグを照合して取得し、ND8 に設定を要求します。ND8 が生成する周波数はファームウェアの対応周波数に正規化されるため、要求値と完全には一致しない場合があります。現在の ND8 ファームウェアの対応表にない周波数は警告します。

MAME の Lua API はアドレスマップ内の書き込み先デバイスを常に特定できないため、YM2151 が複数あるマシンには転送しません。また、ドライバー独自の中継関数や動的マップなど、直接書き込みを捕捉できない構成は対象外です。起動時のログで検出結果を確認してください。Windows では MAME の実行ファイル名が `mame` で始まる必要があります。
