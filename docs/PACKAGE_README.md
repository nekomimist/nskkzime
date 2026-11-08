# nskkzime パッケージ

Windows x64用のSKK風IME。zenzによる推論サーバーを同梱した開発版で、署名・クリーン環境での導入確認はしていない。
モデル、SKK辞書、設定、学習データは含まない。

## 内容と前提

- `bin/`: IME DLL、推論サーバー（`nkkz-ime-server.exe`）、AppContainer用broker、
  設定GUI、学習辞書の管理GUI、設定CLI、辞書管理CLI、入力検証アプリ
- `bin/x86/`: 32bitアプリ用のIME DLLと入力検証アプリ（推論・GUIはx64版を共用）
- `bin/nskkzime_setup.exe`: 導入・検証・更新・復旧・登録解除を行う専用CLI
- `tools/`: 開発用のPowerShell入口とZIP検査スクリプト
- `manifest.json`: 各ファイルのSHA-256とビルド情報
- `tools/scoop.ps1`: Scoopから専用CLIを通常権限で起動する処理
- Visual C++ v14 Redistributable のx64とx86が必要（同梱しない）。
- zenzにはAVX2/FMA/F16C/BMI2に対応したCPUが必要。`.\bin\nskkzime_config.exe --check-cpu` で確認できる。
- モデルは zenz-v3.2-small の `ggml-model-Q5_K_M.gguf`
  （SHA-256 `29c223d4c23327b80fd13ebb5ab2555057a46317997d5da391584ffbef0db673`）を想定している。

```powershell
.\bin\nskkzime_setup.exe --action Verify # 展開したファイルの検査（PowerShell 7は不要）
```

## 導入・更新・解除

使用中のアプリを閉じ、展開したフォルダーで通常権限の端末を使う。PowerShell 7は不要。
変更操作では専用CLIがUACを要求するので、同じ本人のアカウントで承認する。
パッケージ化されたアプリの内蔵端末など、データの仮想化を検出した場合は変更前に停止する。
その場合はExplorerから通常の端末を開き、表示された理由を確認する。

```powershell
.\bin\nskkzime_setup.exe --action Install    # 導入・更新（旧版のファイルは残す）
.\bin\nskkzime_setup.exe --action Status     # 登録中の版と、残っている版の一覧
.\bin\nskkzime_setup.exe --action Rollback --version '<Statusで確認した版ID>'
.\bin\nskkzime_setup.exe --action Recover    # 失敗・中断した更新を元に戻す
.\bin\nskkzime_setup.exe --action Unregister # 登録の解除（ファイルと設定は残す）
```

`%ProgramFiles%\nskkzime\versions\<版ID>` に配置して登録し、brokerをサインイン時に起動するよう登録する。
起動中のアプリは古いDLLを使い続けるので、起動し直すかサインアウト・サインインする。
管理者として直接CLIを起動した場合、brokerは次のサインイン時に起動する。
検証とStatusは昇格しない。旧版の登録と更新記録も、新しいCLIで検証・復旧する。
導入ログは、保護された配置先を確認した後に `%ProgramFiles%\nskkzime\setup.log` へ保存する。
配布物は無署名。ハッシュ検証はファイルの整合性を検査し、発行元の身元は保証しない。
EXE形式でも、会社のEDRや実行ポリシーで停止する場合がある。

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

その後、管理者のWindows PowerShellで本体の保存先を確認して削除する。

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
