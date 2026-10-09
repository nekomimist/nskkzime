# nskkzime 開発用パッケージ

Windows / x64上の64bit・32bitアプリに対応する開発中のSKK風IMEです。検証用ZIPに導入・更新用のPowerShellスクリプトを含みます。
署名・本体の公開ライセンス・クリーン環境での導入試験は未完了です。
モデル、SKK辞書、設定、学習データは含みません。既存ファイルを上書きせず、版ごとに別の場所へ展開してください。

## 内容と前提

`bin/` にIME DLL、辞書補助プロセス、AppContainer補助プロセス、設定GUI、辞書管理GUI、設定CLI、辞書管理CLI、入力検証アプリを収めています。
`bin/x86/` に32bit用のIME DLLと入力検証アプリを収めています。辞書・推論・GUIはx64版を共用します。
実モデル用の `nkkz-ime-server.exe` を必ず含む、zenz CPUの単一構成です。
`manifest.json` に構成（`zenz-cpu`）、各ファイルのSHA-256・サイズ・依存DLL・署名検査結果を記録しています。
`binary_build_revision` と `binary_build_metadata` は各バイナリーに埋め込んだビルド元・内容ハッシュ・
構成です。32bit側は `x86_build_metadata` に別のCMake構成を記録し、両方のソース内容・MSVC版・Release構成を照合します。
ZIPと展開先の検査では各アーキテクチャーのPEと埋め込み情報も確認します。梱包時点のrevisionは別に記録します。
これらはビルド追跡のための自己記載情報で、署名や提供者の認証にはなりません。

形式3から `nskkzime_broker.exe` を含みます。インストール時に本人のHKCUのRunへ登録し、
通常権限のExplorer経由で起動します。次のサインイン時にも起動します。Windows検索・Store内のIMEは、
このプロセスを通じて既存の設定・辞書・学習・zenzを利用します。モデルを常時ロードする処理ではありません。
Program Files内の公開プログラムにAppContainerの読み取り・実行権限を追加します。
ユーザーの辞書・設定・モデルへのファイルアクセス権は変更しません。
登録解除・旧形式へのロールバックではRun登録を外します。AppContainer内から設定・辞書管理GUIは開けないため、
デスクトップアプリ側の入力モードメニューか `bin/nskkzime_settings.exe` を使用してください。

- Windows x64、Microsoft Visual C++ v14 Redistributable **x64とx86** が必要です。
  ビルドに使ったツールセット以上の版を、[Microsoft公式配布ページ](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist/)から取得してください。
  ランタイムのインストーラーは同梱・自動実行しません。
- zenzサーバーにはAVX2/FMA/F16C/BMI2とOSのAVX対応が必要です。CPU要件を満たさない端末では起動しないでください。
  正確な固定条件はマニフェストの `cpu_requirement` と、下記の `--check-cpu` で確認できます。
  初期モデルはzenz-v3.2-smallの `ggml-model-Q5_K_M.gguf`、SHA-256は
  `29c223d4c23327b80fd13ebb5ab2555057a46317997d5da391584ffbef0db673` です。
- PowerShell 7をZIPの検査と以下の登録手順に使います。通常のIME・GUI利用にはPythonやBuild Toolsは不要です。
- 32bit版の検証アプリでEDIT／RichEditの入力を確認しています。個別の32bit常用アプリは未検証です。
  AppContainerはWindows検索・Microsoft Storeの2パッケージを対象とします。ARM64とその他のAppContainerは対象外です。

ZIP全体のSHA-256を提供者の値と照合し、展開後のスクリプトで元ZIPの内容を検査できます。
自己記載したハッシュの一致は改変検出の補助であり、署名や提供者の認証の代わりにはなりません。

```powershell
Get-FileHash 'C:\Downloads\nskkzime.zip' -Algorithm SHA256
.\tools\package.ps1 -Verify 'C:\Downloads\nskkzime.zip'
```

展開したファイルも検査できます。検査はファイルを実行・登録しません。

```powershell
.\tools\package.ps1 -VerifyDirectory .
```

導入やモデル実行の前に、CPU/OSを検査します。このCLIはモデルや保存設定を読み込みません。

```powershell
.\bin\nskkzime_config.exe --check-cpu
```

JSONの `supported` が `true`、終了コードが0ならCPU/OSの固定条件を満たします。
不足時は終了コード3と `missing` に項目を表示します。その環境はこのパッケージの利用対象外です。
この成功だけでモデルの正常動作や速度まで確認したことにはなりません。

## 初回登録と確認

登録には管理者権限が必要です。まず設定・辞書・学習データのバックアップを取り、使用中のアプリを閉じます。
展開したフォルダーで、**本人のアカウントの管理者のx64 PowerShell 7**から以下を実行します。
管理者として別のユーザー資格情報を使う構成や、複数ユーザーへの展開は未検証です。

```powershell
.\tools\install.ps1 -Action Status
.\tools\install.ps1 -Action Install
```

配置先は `%ProgramFiles%\nskkzime\versions\<版ID>` です。版IDはマニフェストのSHA-256です。
配置後にもう一度全ファイルを検査して登録します。一般ユーザーが書き換えられる配置先や再解析ポイントは拒否します。
`Status` は管理者権限なしで使え、登録DLL、現在の版ID、保存済み版ID、未完了の更新を表示します。
管理外の登録（従来の開発用登録や手動で登録したZIP）は自動解除しません。元の手順で解除してから導入します。
旧手順の `out/installed` に登録している場合は、その元DLLを `System32/regsvr32 /u` で解除してから移行します。
現在の `scripts/register-dev.ps1` はこのパッケージ方式の導入・解除を使用します。

登録後は通常権限に戻し、登録DLLと同じ `bin/` の `nskkzime_settings.exe` で手元のSKK辞書を設定して、
`ime_probe.exe` の標準EDIT／RichEditで入力・変換・確定・取消を確認します。
辞書のない状態ではかな入力のみです。設定保存後は入力先アプリを起動し直します。
zenzは既定オフです。使う場合は設定GUIからモデルの絶対パスを指定します。推論サーバーは同梱版を自動で使います。

入力ログは既定オフです。調査時に設定GUIの「診断」で有効化すると、読み・前後の本文・候補を
指定フォルダーへ平文で保存します。有効化・無効化には入力先アプリの再起動が必要です。
1ファイル1 MiB・同じフォルダーに64ファイルで停止し、既存ログは自動削除しません。
`ime_probe.exe` の「遅延計測（本文なし）」はこれとは別の診断で、時間だけをメモリーへ保持します。
TSFキー処理と候補描画のp50/p95を表示しますが、OSキー配送や実際の画面提示までの時間は含みません。

## 更新・解除・旧版への復帰

新しいZIPは別フォルダーへ展開して検査します。使用中のアプリを閉じ、管理者のPowerShellで新しい版の
`tools/install.ps1 -Action Install` を実行します。旧版を解除して新しい版を登録し、旧版のファイルも保持します。
失敗した場合は旧版への復帰を試みます。復帰にも失敗した場合や処理を中断した場合は更新記録を保持します。
原因を解消し、動作中の `regsvr32` が終了した後に、同じスクリプトの `Recover` で変更前の状態へ戻します。

```powershell
.\tools\install.ps1 -Action Status
.\tools\install.ps1 -Action Recover
```

正常に更新した後でも、`Status` の `AvailableVersions` にある旧版IDを指定して復帰できます。

```powershell
.\tools\install.ps1 -Action Rollback -Version '<Statusで確認した64桁の旧版ID>'
.\tools\install.ps1 -Action Unregister
```

`Unregister` はWindowsの登録だけを解除します。配置した全ての版、設定・辞書・学習データ・モデルは保持します。
ファイル削除や「インストールされているアプリ」への追加は行いません。復旧に必要なDLLを先に削除しないでください。
古いDLLを読み込んだままのアプリは起動し直してください。OS登録全体が一度に切り替わる仕組みではなく、
電源断・ファイル破損等から必ず復旧できる保証はありません。開発端末では本人のUAC昇格により
初回導入・更新・解除・旧版復帰と、中断記録を作った状態からの復旧を確認しました。
クリーン環境や実際の電源断からの復旧は未検証です。
モデルサーバーを別フォルダーへ更新した場合は設定GUIのパスも更新し、入力先アプリを起動し直します。

## 依存ライセンス

同梱ライセンスは `licenses/` にあります。nlohmann/jsonはMIT、Unicode対応表はUnicode License v3、
skk-zenzとazooKey llama.cppはMITです。
モデルは別取得・別ライセンス（固定モデルのカードはApache-2.0と記載）です。
辞書は利用する配布元の条件に従ってください。本体の公開ライセンスと署名は公開配布前に確定します。
