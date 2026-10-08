# シリアル ADPCM 受信テスト

`src/serialaudio.cpp` 自体を GPIO/I²C のスタブとリンクする。実機の割り込み遅延・音質は検証しない。

- DATA の順序・チャンク単位の拒否、リング循環、正常終了時の供給継続。
- 不足・あふれ・輸送切断のラッチとミュート、RESET 後の再開。
- FM/PAN/4MHz・768 分周の byte 位置指定、イベント順序・容量・遅延統計。

PowerShell、プロジェクトルートで実行:

```powershell
$OutputEncoding = [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$compiler = 'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64/cl.exe'
$includes = 'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/include'
$ucrt = 'C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/ucrt'
$python = "$env:USERPROFILE/.platformio/penv/Scripts/python.exe"
$out = 'tmp/serialaudio_validation'
New-Item -ItemType Directory -Force $out | Out-Null
& $compiler /nologo /Od /GS- /Zl /utf-8 /LD /Itests/serialaudio/stubs /Iinclude "/I$includes" "/I$ucrt" src/serialaudio.cpp tests/serialaudio/bridge.cpp "/Fo$out/" /link /NOENTRY /NODEFAULTLIB "/OUT:$out/audio.dll" "/IMPLIB:$out/audio.lib"
if ($LASTEXITCODE -ne 0) { throw 'Native audio build failed' }
& $python -X utf8 tests/serialaudio/test_audio.py "$out/audio.dll" -v
```

パーサの COBS/CRC/応答テストは `tests/ndsif`。Delphi のオフラインテストは送信アプリの `diagnostics/ADPCMTests.dpr`（IDE での実行はユーザー側）。


USB 受信の回帰テストは同じスタブに実 `SerialMan` とフレームパーサもリンクする。上記変数を定義したシェルで:

```powershell
& $compiler /nologo /Od /GS- /Zl /utf-8 /Dprivate=public /LD /Itests/serialaudio/stubs /Iinclude /Ilib/ndsif "/I$includes" "/I$ucrt" src/serialaudio.cpp src/serialman.cpp lib/ndsif/ndsif.cpp tests/serialaudio/bridge.cpp tests/serialaudio/receive_bridge.cpp "/Fo$out/" /link /NOENTRY /NODEFAULTLIB "/OUT:$out/receive.dll" "/IMPLIB:$out/receive.lib"
if ($LASTEXITCODE -ne 0) { throw 'Native receive build failed' }
& $python -X utf8 tests/serialaudio/test_receive.py "$out/receive.dll" -v
& $python -X utf8 tests/serialaudio/test_audio.py "$out/receive.dll" -v
```

private の可視性変更はネイティブテストビルド限定で、受信タスクの1周を直接進めるために使用する。CRT 静的初期化の警告は `receiveInit()` による明示初期化で補う。
SOF 監視の変化で途中フレーム・演奏キューを捨てないこと、実バスリセットと受信リングの欠落で故障理由を残すこと、RESET で再開することを検証する。
