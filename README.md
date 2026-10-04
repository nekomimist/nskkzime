# nskkzime

SKK風の操作に、将来 [skk-zenz](https://github.com/nekomimist/skk-zenz) と同様の
zenzによる変換・候補採点を追加する日本語IME。最初の対象はWindows / x64です。

現在は **最初の開発用プロトタイプ** です。OS非依存の入力コア、キー列を再生するCLI、
TSF DLLとテスト用ホストがあります。日常利用できるIMEの完成版ではありません。

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
| `nskkzime_tsf.dll` | 開発用のTSF Text Input Processor |
| `tsf_smoke.exe` | DLLのCOMインターフェースとTSFアダプターの契約テスト |

## 操作を試す

```powershell
.\out\build\windows-x64\Debug\skk_trace.exe 'Kanji<Space><Space><Enter>'
.\out\build\windows-x64\Debug\skk_trace.exe 'KaKu<Space><Enter>'
.\out\build\windows-x64\Debug\skk_trace.exe 'Kanji<Space>desu'
.\out\build\windows-x64\Debug\skk_trace.exe 'Kanji<Space><C-g><C-g>'
```

順に「感じ」「描く」「漢字です」、変換取消を確認できます。
CLIはUTF-8を出力します。入力はASCIIのキー列です。

辞書はテスト用の5見出し（かんじ・にほん・かいとう・かk・たb）だけです。
未知語では読みを保持し、Enterでかなのまま確定できます。

## 実装済み

- ローマ字かな変換、撥音・促音・拗音、未完成ローマ字の編集。
- 大文字による変換開始、送り仮名の指定、候補送り・前候補・確定・取消。
- ひらがな／カタカナ／直接英数入力、次の文字入力による候補確定。
- Windows APIに依存しないコアと辞書インターフェース。
- TSFのCOM生成・初期化・終了、キー通知、編集セッション、composition。
- 失敗した同期編集でキーを奪わない処理と、フォーカス移動時の旧入力欄の後処理。
- 開発用登録・解除エクスポート。ビルドとテストは登録を実行しません。

## 検証の範囲

2026-10-04、MSVC 19.44 / Windows SDK 10.0.26100.0で検証しています。
`core` は32ケース（固定シードによる10,000イベントの状態検査を含む）、
`tsf_smoke` はCOM生成・参照寿命、実TSFマネージャーの起動、テスト用ホスト上での
入力・候補確定・取消・失敗時の復旧・遅延した後処理と新入力欄の分離を検証します。

**実アプリでの入力、WindowsへのIME登録、候補ウィンドウ、マウスによる選択変更は未検証です。**
テスト用ホストはWindowsのテキストストアやキー配送の完全な再現ではありません。

SKK辞書ファイル、ユーザー辞書の永続化・単語登録、abbrev、モードアイコン、
周辺テキスト取得、非同期推論IPC、llama.cpp連携は今後実装します。

詳細は [実装計画](docs/PLAN.md)、[現在の入力仕様](docs/INPUT_SPEC.md)、
[TSFの手動検証手順](docs/TSF_VALIDATION.md) を参照してください。
