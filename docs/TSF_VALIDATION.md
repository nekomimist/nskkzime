# TSFの検証

## 自動確認済みの範囲

`tsf_smoke` はDLLを直接LoadLibraryし、WindowsのIME設定には登録しない。

- DLLエクスポート、クラスファクトリー、IUnknown同一性、参照数とアンロード。
- 実TSFマネージャーの生成・起動・終了。
- テスト用マネージャーでの初期化・終了、途中の登録失敗からの後始末。
- テスト用テキストストアでの未確定表示、候補選択、送り仮名、確定、取消。
- 同期編集拒否、compositionのS_OK＋null応答、SetText失敗時の状態維持。
- compositionの範囲変更失敗時に、確定済みの文字列を次のキーで消さないこと。
- 別入力欄へ移った後の遅延処理、ホストからの強制終了、読み取り専用欄。
- 末尾以外への移動、範囲選択、貼り付け、外部編集、通知の遅延、読み取り失敗。
- 同じ入力欄での古い終了処理と新しいcompositionの競合。
- 表示属性の列挙・取得・複製・リセット。描画そのものは登録後の実アプリで確認する。
- `tsf_dictionary`: 環境変数で指定した辞書の候補を使った変換・確定。
- `tsf_missing_dictionary`: 辞書を開けない場合も読みを保ち、かなで確定・続けて入力できること。

辞書の指定は [辞書の利用](DICTIONARIES.md) を参照。
自動テストが変更する環境変数はテストプロセス内だけで、ユーザー設定は変更しない。

実マネージャーに未登録のTIPを直接接続する試行ではキー通知登録がE_INVALIDARGとなった。
そのため、サービスのactivation以降はテスト用マネージャーで検査している。
この成功を「Windowsに登録したIMEで入力できた」とは扱わない。

## 実アプリの手動確認（未実施）

開発用DLLは候補UI・モードアイコンをまだ持たない。
以下は次のPhase 0の作業手順。現時点でビルドスクリプトからは実行しない。

1. Releaseをビルドする。最初は開発用Windows環境で確認する。
2. 管理者PowerShell 7から `scripts/register-dev.ps1` でDLLを登録する。
3. 通常権限のPowerShellから辞書の環境変数を設定し、`ime_probe.exe` を起動する。
4. 日本語入力言語の状態で「nskkzimeを選ぶ」を押す。このアプリのプロセス内だけで切り替える。
5. 下の一覧を実行し、アプリ名・版・結果を記録する。
6. 別IMEへ切り替え、使用中アプリを閉じてから登録解除する。

```powershell
# 管理者PowerShell 7、リポジトリのルートで実行
.\scripts\register-dev.ps1

# 通常権限のPowerShell、リポジトリのルートで実行
$env:NSKKZIME_DICTIONARY = (Resolve-Path .\SKK-JISYO.utf8.L).Path
.\out\build\windows-x64\Release\ime_probe.exe

# 入力検証アプリ等を閉じてから、管理者PowerShellで登録解除
.\scripts\register-dev.ps1 -Unregister
```

登録はこのプロジェクト専用CLSID `{6EF45A57-E12E-49F1-8A4E-2569D8361A50}` と
日本語プロファイル、キーボード・表示属性カテゴリーを使用する。
プロファイルを有効化するが、既定IMEは変更しない。別のビルドを誤って上書きしないよう、
CLSIDが既に登録済みなら登録を拒否する。登録先のDLLを変更する前に、元のDLLで解除する。
アンインストーラー・署名・更新処理はPhase 3で実装する。

登録スクリプトはRelease DLLを `out/installed/<SHA256の先頭16文字>/` へコピーしてから登録する。
登録中もビルド出力を更新できるが、実アプリは登録時の版を使い続ける。
更新時は使用中アプリを閉じ、解除→再登録→再起動の順に行う。
スクリプトは自分で配置した範囲以外のDLLの登録を解除しない。
以前の手順でビルド先を直接登録していた場合は、元のDLLを `regsvr32 /u` で解除してから移行する。

`ime_probe` は標準EDITとRichEditを備える。入力内容は保存せず、終了時に元のIMEへ戻す。
まず `Kanji`→Space→Enter、`KaKu`→Enter、変換取消を両方で確認し、続いて
変換中の矢印・マウス移動・範囲選択・貼り付けと、入力欄の切り替えを確認する。
2026-10-04時点では、検証アプリの起動・画面・未登録時のエラー表示まで確認済み。
登録スクリプトは構文を検査済みだが、管理者権限での登録・解除は未実施。

| 項目 | 標準入力欄 | ブラウザ | VS Code | Windows Terminal |
|---|---|---|---|---|
| 通常のかな入力・英数切替 | 未確認 | 未確認 | 未確認 | 未確認 |
| 漢字変換・候補送り・取消 | 未確認 | 未確認 | 未確認 | 未確認 |
| 送り仮名・連続入力 | 未確認 | 未確認 | 未確認 | 未確認 |
| フォーカス移動・IME切替 | 未確認 | 未確認 | 未確認 | 未確認 |
| マウス・矢印・選択範囲変更 | 未確認 | 未確認 | 未確認 | 未確認 |
| パスワード・読取専用欄 | 未確認 | 未確認 | 未確認 | 未確認 |
| 前後文脈の取得 | 未実装 | 未実装 | 未実装 | 未実装 |

実アプリ検証時は、変換中文字列以外が消えないか、移動先の選択が維持されるか、
アプリ側で編集した文字列が古い候補に戻らないかを確認する。

参照: [Text Service Registration](https://learn.microsoft.com/en-us/windows/win32/tsf/text-service-registration)、
[RequestEditSession](https://learn.microsoft.com/en-us/windows/win32/api/msctf/nf-msctf-itfcontext-requesteditsession)、
[OnEndEdit](https://learn.microsoft.com/en-us/windows/win32/api/msctf/nf-msctf-itftexteditsink-onendedit)。
