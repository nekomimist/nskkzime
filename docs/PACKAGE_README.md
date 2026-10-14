# nskkzime パッケージ

Windows x64用のSKK風IME。zenzによる推論サーバーを同梱した開発版で、署名・クリーン環境での導入確認はしていない。
モデル、SKK辞書、設定、学習データは含まない。

## 内容と前提

- `bin/`: IME DLL、推論サーバー（`nkkz-ime-server.exe`）、AppContainer用broker、
  設定GUI、学習辞書の管理GUI、設定CLI、辞書管理CLI、入力検証アプリ
- `bin/x86/`: 32bitアプリ用のIME DLLと入力検証アプリ（推論・GUIはx64版を共用）
- `tools/`: 導入スクリプトとZIP検査スクリプト、`manifest.json`: 各ファイルのSHA-256とビルド情報
- `tools/scoop.ps1`: Scoopから同じユーザーの管理者承認を受けて導入・登録解除するための呼び出し処理
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

本体はMIT License（`LICENSE`）。依存ライブラリ・データのライセンスは `licenses/` を参照。
nlohmann/jsonはMIT、Unicode対応表はUnicode License v3、skk-zenzとazooKey llama.cppはMIT。
モデルは別途取得し、そのライセンス（モデルカードではApache-2.0）に従う。

## 完全削除

`Unregister` はIMEの登録と、このユーザーのbrokerの起動登録を解除する。プログラムとユーザーデータは残す。
本体も削除する場合は、登録解除が成功したことを `Status` で確認し、別のIMEへ切り替えて
サインアウト・サインインする。ほかにサインイン中のユーザーがいれば、そのアプリも終了する。
使用中のDLLを残したまま削除しない。

その後、管理者のPowerShell 7で本体の保存先を確認して削除する。

```powershell
$installRoot = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'nskkzime'
Get-Content -LiteralPath (Join-Path $installRoot 'install-root.json') # product が nskkzime であることを確認
Remove-Item -LiteralPath $installRoot -Recurse
```

設定・学習辞書も不要な場合だけ、使用する本人の通常権限のPowerShellで
`%LOCALAPPDATA%\nskkzime` を削除する。`NSKKZIME_CONFIG` で指定した別の設定ファイルと、
別の場所に保存したSKK辞書・モデルは、この手順では削除しない。

既定の設定・学習辞書・キャッシュ・診断ログをすべて削除する場合は、通常権限で実行する。

```powershell
$dataRoot = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'nskkzime'
Remove-Item -LiteralPath $dataRoot -Recurse
```
