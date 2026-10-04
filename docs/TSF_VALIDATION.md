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
- `tsf_local_activation`: プロセス内COMマニフェストで、実行ファイルと同じ場所のDLLが選ばれること。
- 通常キーの判定を繰り返しても読みが変わらないことと、矢印・Ctrlショートカットを判定時から処理しないと返すこと。
- キー判定時の終了要求が延期されても、二重終了や新しいcompositionへの干渉が起きないこと。

辞書の指定は [辞書の利用](DICTIONARIES.md) を参照。
自動テストが変更する環境変数はテストプロセス内だけで、ユーザー設定は変更しない。

実マネージャーに未登録のTIPを直接接続する試行ではキー通知登録がE_INVALIDARGとなった。
そのため、サービスのactivation以降はテスト用マネージャーで検査している。
この成功を「Windowsに登録したIMEで入力できた」とは扱わない。

## 実入力の確認手順

開発用DLLは候補UI・モードアイコンをまだ持たない。
以下はPhase 0の作業手順。ビルドスクリプトからは実行しない。

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

`ime_probe` は標準EDITと、`SES_USECTF` を有効にしたRichEditを備える。
RichEditは既定でTSFが無効なので、明示設定しない場合は互換入力経路の検証になる。
入力内容は保存せず、IME切替前のプロファイルを記録し、復帰ボタンと終了時に戻す。
画面下部に読み込んだDLLのパスを表示する。
まず `Kanji`→Space→Enter、`KaKu`→Enter、変換取消を両方で確認し、続いて
変換中の矢印・マウス移動・範囲選択・貼り付けと、入力欄の切り替えを確認する。
### ビルド中の繰り返し検証

初回のIME登録後は `ime_probe_local.exe` でビルド先のDLLを検証できる。
COMマニフェストで、この検証プロセスだけ実行ファイルと同じ場所の `nskkzime_tsf.dll` を使う。
タイトルの「ビルド版」と、画面下部の `out/build/...` のパスで区別する。
他のアプリは引き続き `out/installed/...` の登録版を使う。
プロファイルの登録を省略する機能ではなく、登録・更新の検証には通常の `ime_probe.exe` を使う。
再ビルドの前にはビルド版の検証アプリを閉じる。

### 2026-10-04の実測結果

Windows x64、内蔵の5見出し辞書、実際のキーイベントで確認した。
登録版はコミット `d285de3` のDLL（配置ディレクトリ `3f0a9484c28fa186`）。

- 登録済みCLSIDのDLLパス確認、プロファイル切替成功（HRESULT `0x00000000`）。
- 標準EDIT: `a` →「あ」、`Kanji` → Space → Space → Enter →「感じ」、`KaKu` → Enter →「書く」。
- 標準EDIT: 読みの点線、候補の太い実線、確定後に下線が消えること。
- 旧RichEdit（TSFを明示的に有効化する前）: `KaKu` → Space →「描く」、Escape →「かく」、Escape →取消。
- 旧RichEditで `Ka` の途中に標準EDITへクリックで移動し、移動元の「か」を保持して移動先に「あ」を入力。
- 「元のIMEに戻す」の成功表示と、現在のIMEが別IMEへ変わること。

ビルド版DLLとTSF対応RichEditでは、以下も確認した。

- `Ka` → Left → `a` が「あか」になる（最初のLeftで未確定入力を終了して左へ移動）。
- 文中で `KaKu` → Space → Enter が「描く」になり、後続の本文を保持する。
- `l` → `a` → `C-j` → `a` でASCIIの「a」とひらがなの「あ」が順に入る。
- 読み入力中のShift+Leftで1文字を選択し、次の `a` が選択部分だけを「あ」に置き換える。
- 文中で `Ka` → Escape と入力すると、その未確定部分だけを取り消して既存本文を保持する。

登録解除・登録版の更新、外部アプリ、実入力での外部辞書は未確認。

### 既知の制限

標準EDITの互換入力経路では `Ka` → Left が確定だけになり、直後の `a` は「かあ」になる。
キー判定を処理しないと返し、終了要求が成功してもこの挙動は残る。
TSF対応RichEditでは同じ手順で「あか」になる。標準EDITでは移動前にEnterで確定するとよい。
互換入力経路の確定とキー配送の順序は追加調査が必要。

キー入力通知では、アプリへ渡すキーを一度「処理する」と返す方法をやめ、
`OnTestKeyDown` 内で終了だけを要求して「処理しない」と返す。
通常のSKKキー判定は引き続き読み取り専用。終了は本文の書き戻しをせず、重複通知でも一度だけ行う。

| 項目 | 標準EDIT | RichEdit (TSF) | ブラウザ／VS Code／Windows Terminal |
|---|---|---|---|
| 通常のかな入力 | 確認済み | 確認済み | 未確認 |
| 英数切替 | 未確認 | 確認済み | 未確認 |
| 漢字変換・候補送り・確定 | 確認済み | 送りありで確認済み | 未確認 |
| 送り仮名 | 確認済み | 確認済み | 未確認 |
| フォーカス移動・IME切替 | 一部確認済み（上記参照） | 未確認 | 未確認 |
| 矢印移動 | 最初の1回が確定のみ | Leftで確認済み | 未確認 |
| 範囲選択・読み取消 | 未確認 | Shift+Left／Escapeで確認済み | 未確認 |
| パスワード・読取専用欄 | 未確認 | 未確認 | 未確認 |
| 前後文脈の取得 | 未実装 | 未実装 | 未実装 |

実アプリ検証時は、変換中文字列以外が消えないか、移動先の選択が維持されるか、
アプリ側で編集した文字列が古い候補に戻らないかを確認する。

参照: [Text Service Registration](https://learn.microsoft.com/en-us/windows/win32/tsf/text-service-registration)、
[RequestEditSession](https://learn.microsoft.com/en-us/windows/win32/api/msctf/nf-msctf-itfcontext-requesteditsession)、
[OnEndEdit](https://learn.microsoft.com/en-us/windows/win32/api/msctf/nf-msctf-itftexteditsink-onendedit)。
TSFキー判定の終了処理は [Microsoft SampleIMEのキー通知](https://github.com/microsoft/Windows-classic-samples/blob/main/Samples/IME/cpp/SampleIME/KeyEventSink.cpp) も参照。
検証アプリの設定は [RichEditの編集スタイル](https://learn.microsoft.com/en-us/windows/win32/controls/em-geteditstyle)、
[COMマニフェスト](https://learn.microsoft.com/en-us/windows/win32/sbscs/assembly-manifests) を参照。
