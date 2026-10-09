# nskkzime 開発用パッケージ

Windows x64用のSKK風IME。zenzによる推論サーバーを同梱した開発版で、署名・クリーン環境での導入確認はしていない。
モデル、SKK辞書、設定、学習データは含まない。

## 内容と前提

- `bin/`: IME DLL、辞書補助プロセス、推論サーバー（`nkkz-ime-server.exe`）、AppContainer用broker、
  設定GUI、学習辞書の管理GUI、設定CLI、辞書管理CLI、入力検証アプリ
- `bin/x86/`: 32bitアプリ用のIME DLLと入力検証アプリ（辞書・推論・GUIはx64版を共用）
- `tools/`: 導入スクリプトとZIP検査スクリプト、`manifest.json`: 各ファイルのSHA-256とビルド情報
- Visual C++ v14 Redistributable のx64とx86が必要（同梱しない）。
- zenzにはAVX2/FMA/F16C/BMI2に対応したCPUが必要。`.\bin\nskkzime_config.exe --check-cpu` で確認できる。
- モデルは zenz-v3.2-small の `ggml-model-Q5_K_M.gguf`
  （SHA-256 `29c223d4c23327b80fd13ebb5ab2555057a46317997d5da391584ffbef0db673`）を想定している。

```powershell
.\tools\package.ps1 -Verify 'C:\Downloads\nskkzime.zip'   # ZIPの検査
.\tools\package.ps1 -VerifyDirectory .                    # 展開したファイルの検査
```

## 導入・更新・解除

使用中のアプリを閉じ、展開したフォルダーで**管理者のPowerShell 7**（スタートメニューから起動したもの）を使う。

```powershell
.\tools\install.ps1 -Action Install      # 導入・更新（旧版のファイルは残す）
.\tools\install.ps1 -Action Status       # 登録中の版と、残っている版の一覧
.\tools\install.ps1 -Action Rollback -Version '<Statusで確認した版ID>'
.\tools\install.ps1 -Action Recover      # 失敗・中断した更新を元に戻す
.\tools\install.ps1 -Action Unregister   # 登録の解除（ファイルと設定は残す）
```

`%ProgramFiles%\nskkzime\versions\<版ID>` に配置して登録し、brokerをサインイン時に起動するよう登録する。
起動中のアプリは古いDLLを使い続けるので、起動し直すかサインアウト・サインインする。

導入後は入力モード「あ／ア／A」の右クリックメニューの「設定…」（`bin/nskkzime_settings.exe`）で
SKK辞書とzenzのモデルを指定し、`bin/ime_probe.exe` で入力を確認する。推論サーバーは同梱版を自動で使う。

## ライセンス

`licenses/` を参照。nlohmann/jsonはMIT、Unicode対応表はUnicode License v3、skk-zenzとazooKey llama.cppはMIT。
モデルは別途取得し、そのライセンス（モデルカードではApache-2.0）に従う。本体のライセンスは未定。
