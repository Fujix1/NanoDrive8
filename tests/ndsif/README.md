# NDSIF パーサのホスト検証

実際の `lib/ndsif/ndsif.cpp` を DLL にして、Python 標準ライブラリで検証します。
USB ポートや実チップは操作しません。Windows / MSVC x64 を使用します。

プロジェクトルートで以下を実行します。各ツールのパスは環境に合わせてください。

```powershell
$compiler = 'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64/cl.exe'
$includes = 'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/include'
$ucrt = 'C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/ucrt'
$python = "$env:USERPROFILE/.platformio/penv/Scripts/python.exe"
$out = 'tmp/ndsif_validation'
New-Item -ItemType Directory -Force $out | Out-Null
& $compiler /nologo /Od /GS- /Zl /utf-8 /LD /Ilib/ndsif "/I$includes" "/I$ucrt" lib/ndsif/ndsif.cpp tests/ndsif/bridge.cpp "/Fo$out/" /link /NOENTRY /NODEFAULTLIB "/OUT:$out/ndsif.dll" "/IMPLIB:$out/ndsif.lib"
if ($LASTEXITCODE -ne 0) { throw 'Native build failed' }
& $python tests/ndsif/test_protocol.py "$out/ndsif.dll" -v
```

CRT を使用しない検証用 DLL です。LNK4210 の警告は静的初期化を呼ばないため発生しますが、各テストの `init()` がパーサの `configure()` とカウンタ初期化を明示的に行います。
