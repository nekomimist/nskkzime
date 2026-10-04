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
- `tsf_dictionary`: 環境変数で指定した辞書の候補を使った変換・確定。
- `tsf_missing_dictionary`: 辞書を開けない場合も読みを保ち、かなで確定・続けて入力できること。

辞書の指定は [辞書の利用](DICTIONARIES.md) を参照。
自動テストが変更する環境変数はテストプロセス内だけで、ユーザー設定は変更しない。

実マネージャーに未登録のTIPを直接接続する試行ではキー通知登録がE_INVALIDARGとなった。
そのため、サービスのactivation以降はテスト用マネージャーで検査している。
この成功を「Windowsに登録したIMEで入力できた」とは扱わない。

## 実アプリの手動確認（未実施）

開発用DLLは候補UI・モードアイコン・完全な選択変更処理をまだ持たない。
以下は次のPhase 0の作業手順。現時点でビルドスクリプトからは実行しない。

1. Releaseをビルドする。最初は開発用Windows環境で確認する。
2. 管理者PowerShellから、x64のregsvr32でDLLを登録する。
3. Windowsの日本語キーボード設定に `nskkzime (development)` を追加する。
4. 入力先アプリを起動し直し、Win+Spaceで選ぶ。既定IMEの自動変更はしない。
5. 下の一覧を実行し、アプリ名・版・結果を記録する。
6. 別IMEへ切り替え、使用中アプリを閉じてから登録解除する。

```powershell
# 管理者PowerShell、リポジトリのルートで実行
$dll = (Resolve-Path .\out\build\windows-x64\Release\nskkzime_tsf.dll).Path
& "$env:SystemRoot\System32\regsvr32.exe" $dll

# 登録解除
& "$env:SystemRoot\System32\regsvr32.exe" /u $dll
```

登録はこのプロジェクト専用CLSID `{6EF45A57-E12E-49F1-8A4E-2569D8361A50}` と
日本語プロファイル、キーボードカテゴリーを使用する。別のビルドを誤って上書きしないよう、
CLSIDが既に登録済みなら登録を拒否する。登録先のDLLを変更する前に、元のDLLで解除する。
アンインストーラー・署名・更新処理はPhase 3で実装する。

| 項目 | 標準入力欄 | ブラウザ | VS Code | Windows Terminal |
|---|---|---|---|---|
| 通常のかな入力・英数切替 | 未確認 | 未確認 | 未確認 | 未確認 |
| 漢字変換・候補送り・取消 | 未確認 | 未確認 | 未確認 | 未確認 |
| 送り仮名・連続入力 | 未確認 | 未確認 | 未確認 | 未確認 |
| フォーカス移動・IME切替 | 未確認 | 未確認 | 未確認 | 未確認 |
| マウス・矢印・選択範囲変更 | 未確認 | 未確認 | 未確認 | 未確認 |
| パスワード・読取専用欄 | 未確認 | 未確認 | 未確認 | 未確認 |
| 前後文脈の取得 | 未実装 | 未実装 | 未実装 | 未実装 |

特に、マウスやショートカットでcompositionの途中を編集する場合の扱いは未完成。
実アプリ検証時は、変換中文字列以外が消えないか、マーカーが残らないかを確認する。

参照: [Text Service Registration](https://learn.microsoft.com/en-us/windows/win32/tsf/text-service-registration)、
[RequestEditSession](https://learn.microsoft.com/en-us/windows/win32/api/msctf/nf-msctf-itfcontext-requesteditsession)。
