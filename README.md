# nskkzime

SKK風の操作に、将来 [skk-zenz](https://github.com/nekomimist/skk-zenz) と同様の
zenzによる変換・候補採点を追加する日本語IME。最初の対象はWindows / x64です。

現在は **最初の開発用プロトタイプ** です。OS非依存の入力コア、キー列を再生するCLI、
SKK辞書の読み込み、TSF DLLとテスト用ホストがあります。日常利用できるIMEの完成版ではありません。

## ビルド

PowerShell 7、Visual Studio 2022 Build ToolsのC++ツールセット、Windows SDK、
「C++ CMake tools for Windows」を使用します。外部ライブラリのダウンロードはありません。

```powershell
.\scripts\build.ps1
.\scripts\build.ps1 -Configuration Release
```

スクリプトは `vswhere` でBuild Toolsと付属CMakeを検出し、configure・build・CTestを実行します。
Developer Command Promptや、CMakeをPATHに追加する操作は不要です。
子プロセスへ渡す環境変数の名前を正規化するため、`PATH` と `Path` の重複がある起動環境でも
MSBuildを実行できます。ユーザーの永続的な環境変数は変更しません。

出力先は `out/build/windows-x64/Debug/` または `Release/` です。

| ファイル | 用途 |
|---|---|
| `skk_trace.exe` | キー列と、未確定・確定文字列の遷移をJSON Linesで出力 |
| `core_tests.exe` | 入力コアの回帰テスト |
| `dictionary_tests.exe` | 辞書形式・文字コード・候補の統合の回帰テスト |
| `nskkzime_config.exe` | ユーザーごとの辞書設定を保存・表示・検査 |
| `nskkzime_tsf.dll` | 開発用のTSF Text Input Processor |
| `tsf_smoke.exe` | DLLのCOMインターフェースとTSFアダプターの契約テスト |
| `ime_probe.exe` | 標準EDIT／RichEditを使う、登録後の実入力検証アプリ |
| `ime_probe_local.exe` | 同じ検証をビルド先のDLLで行うアプリ。初回のIME登録は必要 |

## 操作を試す

```powershell
.\out\build\windows-x64\Debug\skk_trace.exe 'Kanji<Space><Space><Enter>'
.\out\build\windows-x64\Debug\skk_trace.exe 'KaKu<Space><Enter>'
.\out\build\windows-x64\Debug\skk_trace.exe 'Kanji<Space>desu'
.\out\build\windows-x64\Debug\skk_trace.exe 'Kanji<Space><C-g><C-g>'
```

順に「感じ」「描く」「漢字です」、変換取消を確認できます。
CLIはUTF-8を出力します。入力はASCIIのキー列です。

辞書を指定しない場合はテスト用の5見出し（かんじ・にほん・かいとう・かk・たb）を使います。
未知語では読みを保持し、Enterでかなのまま確定できます。

既存のSKK辞書も読み取り専用で利用できます。

```powershell
.\out\build\windows-x64\Release\skk_trace.exe --dict 'C:\辞書\SKK-JISYO.L' 'Jisho<Space><Enter>'
```

UTF-8／EUC-JPを判別し、候補と注釈をJSON Linesへ、読み込みの件数・診断を標準エラーへ出します。
`--dict` を繰り返すと先に指定した辞書を優先します。TSFの辞書設定は次のコマンドで保存できます。

```powershell
.\out\build\windows-x64\Release\nskkzime_config.exe --set-dictionaries 'C:\辞書\SKK-JISYO.L'
.\out\build\windows-x64\Release\nskkzime_config.exe --check
```

保存先は `%LOCALAPPDATA%\nskkzime\settings.ini`。複数の絶対パスを並べると先の辞書を優先します。
設定後は利用するアプリを起動し直します。`NSKKZIME_DICTIONARY` は一時的な上書き指定として使えます。
詳しい設定と対応形式は [辞書の利用](docs/DICTIONARIES.md) を参照してください。

## 実装済み

- ローマ字かな変換、撥音・促音・拗音、未完成ローマ字の編集。
- 最初のかな・送り仮名の途中でShiftを離すのが遅れた入力（`KAnji`、`KOroNde`、`KaKU`）を許容。
- 大文字による変換開始、送り仮名の指定、候補送り・前候補・確定・取消。
- ひらがな／カタカナ／直接英数入力、次の文字入力による候補確定。
- `/` からのabbrev入力。英字の綴り・大小文字を保った辞書検索と、確定・取消後のかな入力への復帰。
- Windows APIに依存しないコアと辞書インターフェース。
- SKK辞書のUTF-8／EUC-JP取り込み、注釈、送り仮名別候補、複数辞書の優先順と重複排除。
- TSFの複数辞書設定の保存、設定CLI、検証アプリでの実際の読み込み結果・エラーの表示。
- TSFのCOM生成・初期化・終了、キー通知、編集セッション、composition。
- 失敗した同期編集でキーを奪わない処理と、フォーカス移動時の旧入力欄の後処理。
- 選択範囲・アプリ側の変更の監視、上書き前の内容照合、読み／候補のTSF下線表示。
- キャレット付近の候補一覧・注釈・7件単位の表示。Space／xに追従し、確定・取消・フォーカス移動で閉じます。
- TSFのUIElement通知と、入力アプリが独自の候補UIを表示する場合の抑制。
- 開発用登録・解除エクスポート。ビルドとテストは登録を実行しません。

## 検証の範囲

2026-10-05、MSVC 19.44 / Windows SDK 10.0.26100.0で検証しています。
`core` は46ケース（固定シードによる10,000イベントの状態検査を含む）、
`dictionary` は31ケースで、形式・文字コード・送り仮名・辞書間の候補統合を検証します。
`settings` は14ケースで、設定形式・保存失敗時の保全・複数辞書の部分的な読み込み失敗を検証します。
`config_cli` で設定コマンド、`tsf_settings` / `tsf_invalid_settings` で保存設定のTSF接続を検証します。
`tsf_smoke` はCOM生成・参照寿命、実TSFマネージャーの起動、テスト用ホスト上での
入力・候補確定・取消・失敗時の復旧・遅延した後処理と新入力欄の分離を検証します。
辞書を指定したTSF入力、辞書読み込み失敗時のかな入力、CLIの辞書読み込みもCTestで確認します。
選択変更・貼り付け・外部編集・空のabbrev・同一入力欄での遅延終了との競合を16シナリオで検証し、
表示属性のCOMインターフェースも検査します。
`tsf_local_activation` はCOMマニフェストによるビルド先DLLの読み込みを検証します。
`candidate_ui` は送り仮名付き候補・ページ境界・ホスト指定のページ・COM契約を検査します。
TSF側ではUIの開始・更新・終了、表示抑制、UI失敗時の変換継続、古いレイアウト通知も検証します。

**登録したIMEで標準EDIT／RichEditのかな入力・候補送り・確定・取消を確認しました。**
TSFを有効にしたRichEditでは、ビルド版DLLで矢印移動後の入力も確認しています。
標準EDITの互換入力経路では最初の矢印が確定だけになる制限が残ります。
テスト用ホストはWindowsのテキストストアやキー配送の完全な再現ではありません。
登録解除・更新後のDLLハッシュと実入力を確認しました。
2026-10-05には、ビルド版の候補一覧・注釈・選択行の追従と確定時の終了をRichEditで確認しています。
abbrevの英字入力・確定後のかなへの復帰も標準EDIT／RichEditで確認しました。
保存した外部辞書を環境変数なしで読み込み、RichEditの「辞書」と標準EDITのabbrev「SKK」を確定できました。
ブラウザ等の互換性は引き続き検証します。

候補のクリック／直接選択、ユーザー辞書の永続化・単語登録、モードアイコン、
周辺テキスト取得、非同期推論IPC、llama.cpp連携は今後実装します。

詳細は [実装計画](docs/PLAN.md)、[現在の入力仕様](docs/INPUT_SPEC.md)、
[TSFの手動検証手順](docs/TSF_VALIDATION.md) を参照してください。
