# nskkzime 開発用パッケージ

Windows / x64用の開発中のSKK風IMEです。これは検証用ZIPで、完成版のインストーラーではありません。
署名・本体の公開ライセンス・クリーン環境での導入試験は未完了です。
モデル、SKK辞書、設定、学習データは含みません。既存ファイルを上書きせず、版ごとに別の場所へ展開してください。

## 内容と前提

`bin/` にIME DLL、辞書補助プロセス、設定GUI、辞書管理GUI、設定CLI、辞書管理CLI、入力検証アプリを収めています。
`zenz-cpu` 版だけは実モデル用の `nkkz-ime-server.exe` も含みます。
`manifest.json` に通常版／zenz版の区別、各ファイルのSHA-256・サイズ・依存DLL・署名検査結果を記録しています。
記録したソースrevisionは梱包時点のもので、既存バイナリーのビルド元を証明するものではありません。

- Windows x64、Microsoft Visual C++ v14 Redistributable **x64** が必要です。
  ビルドに使ったツールセット以上の版を、[Microsoft公式配布ページ](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist/)から取得してください。
  ランタイムのインストーラーは同梱・自動実行しません。
- zenzサーバーにはAVX2/FMA/F16C/BMI2とOSのAVX対応が必要です。CPU要件を満たさない端末では起動しないでください。
  初期モデルはzenz-v3.2-smallの `ggml-model-Q5_K_M.gguf`、SHA-256は
  `29c223d4c23327b80fd13ebb5ab2555057a46317997d5da391584ffbef0db673` です。
- PowerShell 7をZIPの検査と以下の登録手順に使います。通常のIME・GUI利用にはPythonやBuild Toolsは不要です。
- 検証対象はWindows 11のx64アプリです。x86、ARM64、AppContainer等は未確認です。

ZIP全体のSHA-256を提供者の値と照合し、展開後のスクリプトで元ZIPの内容を検査できます。
自己記載したハッシュの一致は改変検出の補助であり、署名や提供者の認証の代わりにはなりません。

```powershell
Get-FileHash 'C:\Downloads\nskkzime.zip' -Algorithm SHA256
.\tools\package.ps1 -Verify 'C:\Downloads\nskkzime.zip'
```

検査はファイルを展開・実行・登録しません。元ZIPの検査なので、展開した後に変更されたファイルは対象外です。

## 初回登録と確認

登録には管理者権限が必要です。まず設定・辞書・学習データのバックアップを取り、使用中のアプリを閉じます。
展開したフォルダーを固定した場所へ置き、**管理者のx64 PowerShell 7**で以下を実行します。
管理者として別のユーザー資格情報を使う構成や、複数ユーザーへの展開は未検証です。

```powershell
$dll = (Resolve-Path '.\bin\nskkzime_tsf.dll').Path
& "$env:SystemRoot\System32\regsvr32.exe" $dll
```

DLLの登録は既存のnskkzime登録を上書きしません。既に登録されている場合は下記の更新手順を使います。
DLLと同じ `bin/` 内の補助プロセス・GUIを移動しないでください。
登録後は通常権限に戻し、`bin/nskkzime_settings.exe` で手元のSKK辞書を設定して、
`bin/ime_probe.exe` の標準EDIT／RichEditで入力・変換・確定・取消を確認します。
辞書のない状態ではかな入力のみです。設定保存後は入力先アプリを起動し直します。
zenzは既定オフです。使う場合は設定GUIからモデルと同梱サーバーの絶対パスを指定します。

## 更新・解除・旧版への復帰

新しい版は別フォルダーへ展開し、ZIPを検査します。旧版のファイルは復帰用に残します。
現在登録されているDLLの場所は、次の読み取りコマンドで確認できます。

```powershell
$key = 'HKLM:\Software\Classes\CLSID\{6EF45A57-E12E-49F1-8A4E-2569D8361A50}\InprocServer32'
(Get-ItemProperty -LiteralPath $key).'(default)'
```

使用中のアプリを閉じ、管理者のx64 PowerShellから、**確認した現在のDLL**で解除します。

```powershell
& "$env:SystemRoot\System32\regsvr32.exe" /u 'C:\確認した現在の版\bin\nskkzime_tsf.dll'
```

解除が成功したことを確認してから新しい版を登録します。新しい版の登録に失敗した場合は、
旧版の同じDLLを登録し直します。古いDLLを読み込んだままのアプリは起動し直してください。
アンインストールは解除までで完了し、設定・辞書・学習データは保持します。
復旧に必要なDLLを先に削除しないでください。このZIPはデータ移行や自動ロールバックを実行しません。
モデルサーバーを別フォルダーへ更新した場合は設定GUIのパスも更新し、入力先アプリを起動し直します。

## 依存ライセンス

同梱ライセンスは `licenses/` にあります。nlohmann/jsonはMIT、Unicode対応表はUnicode License v3、
zenz版のskk-zenzとazooKey llama.cppはMITです。
モデルは別取得・別ライセンス（固定モデルのカードはApache-2.0と記載）です。
辞書は利用する配布元の条件に従ってください。本体の公開ライセンスと署名は公開配布前に確定します。
