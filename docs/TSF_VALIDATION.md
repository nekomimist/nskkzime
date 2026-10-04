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
- `candidate_ui`: 送り仮名を含む候補文字列、7件単位のページ、ホスト指定のページ境界、出力バッファの境界と変更通知。
- PageDown／PageUpの候補移動、端での保持、キー判定の副作用なし、編集失敗時の選択維持、Shift付きページキーのアプリへの配送。
- TSF候補UIの開始・更新・終了、ホストによる表示抑制、UIを作れない場合の通常変換。
- レイアウト通知の集約、ビュー破棄／確定後に届く古い通知の無効化、保持されたUIオブジェクトから再表示できないこと。
- Shiftを離す遅れ（`KAnji`／`KAKU`）と、abbrevの英字見出しからの辞書検索・候補表示・確定。
- 空のabbrevのcomposition、英字の大小文字維持、削除・取消・カーソル移動・別入力欄への移動。
- `tsf_learning`: 編集成功時だけの非同期配送、失敗後の再試行、password／private／PIN・属性取得失敗・混在範囲の学習除外。
- `learning`: ユーザー辞書の保存と復旧、別プロセスからの同時更新、同じイベントの再送、保存失敗時の保全、現在の選択候補の固定。
- `registration`: 登録中のSKK変換・送り仮名・英字見出し・再帰・上限・編集コピーの独立性・10,000イベントの状態検査。
- `tsf_learning` では単語登録の編集失敗→再試行、保存前の再変換、内側の登録と外側の取消、
  フォーカス移動時の破棄、入力属性の途中変更、登録UIを作れない場合のかな入力継続も検証する。

辞書の指定は [辞書の利用](DICTIONARIES.md) を参照。
自動テストが変更する環境変数はテストプロセス内だけで、ユーザー設定は変更しない。

実マネージャーに未登録のTIPを直接接続する試行ではキー通知登録がE_INVALIDARGとなった。
そのため、サービスのactivation以降はテスト用マネージャーで検査している。
この成功を「Windowsに登録したIMEで入力できた」とは扱わない。

## 実入力の確認手順

開発用DLLは候補一覧と注釈を表示する。モードアイコンはまだ持たない。
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

登録スクリプトはRelease DLLと辞書補助プロセスを同じ
`out/installed/<両ファイルのハッシュから作ったSHA256の先頭16文字>/` へコピーしてから登録する。
登録中もビルド出力を更新できるが、実アプリは登録時の版を使い続ける。
更新時は使用中アプリを閉じ、解除→再登録→再起動の順に行う。
スクリプトは自分で配置した範囲以外のDLLの登録を解除しない。
以前の手順でビルド先を直接登録していた場合は、元のDLLを `regsvr32 /u` で解除してから移行する。

`ime_probe` は標準EDITと、`SES_USECTF` を有効にしたRichEditを備える。
RichEditは既定でTSFが無効なので、明示設定しない場合は互換入力経路の検証になる。
両方の欄に `SetInputScope(IS_TEXT)` を設定する。入力本文は保存しないが、確定候補はIMEの学習設定に従う。
IME切替前のプロファイルを記録し、復帰ボタンと終了時に戻す。
画面下部に読み込んだDLLのパスと、そのDLLの辞書読み込み結果を表示する。
辞書の永続設定・診断には [設定CLI](DICTIONARIES.md) を使う。
まず `Kanji`→Space→Enter、`KaKu`→Enter、変換取消を両方で確認し、続いて
変換中の矢印・マウス移動・範囲選択・貼り付けと、入力欄の切り替えを確認する。
### ビルド中の繰り返し検証

初回のIME登録後は `ime_probe_local.exe` でビルド先のDLLを検証できる。
COMマニフェストで、この検証プロセスだけ実行ファイルと同じ場所の `nskkzime_tsf.dll` を使う。
タイトルの「ビルド版」と、画面下部の `out/build/...` のパスで区別する。
他のアプリは引き続き `out/installed/...` の登録版を使う。
プロファイルの登録を省略する機能ではなく、登録・更新の検証には通常の `ime_probe.exe` を使う。
再ビルドの前にはビルド版の検証アプリを閉じる。
補助プロセスもビルド先を使用するため、アプリを閉じてから約10秒のアイドル終了を待つ。
補助プロセスや学習ファイルを強制的に削除する必要はない。

`ime_probe_isolated.exe` は自作fixtureの小辞書を使い、実行ファイルと同じ場所の
`probe-user-dictionary.nskkdb` にだけ保存する。ユーザーの設定・辞書には触れず、
単語登録を繰り返し検証できる。画面タイトルの「隔離データ」と診断欄の保存先で区別する。
通常版と同様に初回のIME登録が必要。隔離した保存ファイルは検証アプリを再起動しても保持する。

単語登録とPageDown／PageUp追加後の実画面確認は未実施。Computer Useによる隔離版の起動がアプリ承認待ちで
タイムアウトしたため、自動テストの成功を実入力での確認済みとは扱わない。
確認時は、未知語→登録欄内で漢字変換→Enter、送りありの語幹登録、取消、別欄へのフォーカス移動、
再起動後の登録語の変換を両方の入力欄で試す。

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

### 2026-10-05の実測結果

ユーザーによる解除・再登録後、登録DLLが `043a22c6f368b467/nskkzime_tsf.dll`
（コミット `b499279` のビルド）になっていることとSHA256を照合した。
通常版 `ime_probe.exe` でも同じパスを表示し、IME切替成功後、TSF対応RichEditで
`Ka` → Left → `a` が「あか」になることを確認した。

候補UI追加後は `ime_probe_local.exe` と内蔵辞書を使い、TSF対応RichEditで以下を確認した。

- `Kanji` → Spaceで本文が「漢字」になり、その下に「漢字 — 文字」「感じ — 感覚」を表示。
- Spaceで「感じ」、`x` で「漢字」へ戻り、一覧の選択行と候補番号が追従する。
- Enterで確定して一覧が閉じ、注釈は本文に入らない。
- `KaKu` で「書く／描く」を表示し、候補にも送り仮名が付く。
- 選択中に標準EDITへクリックすると一覧が閉じ、移動元の「書く」を保持する。続く `a` は移動先に「あ」と入る。
- 検証後の「元のIMEに戻す」が成功し、別IMEに戻ったことを確認してアプリを終了。

この候補UIの検証はビルド版DLLで行った。登録版に反映するには、次の登録更新が必要。
外部アプリ、候補が8件以上ある場合の実画面、DPI・複数画面の実測は未確認。

同日のabbrev追加後、ビルド版DLLで以下の実入力も確認した。

- RichEdit (TSF): `/qL` → Enter → `a` が「qLあ」となり、英字の綴り・大小文字を維持して確定後かなに戻る。
- RichEditで `/` だけ入力して標準EDITへクリックし、続く `a` が移動先で「あ」となる。移動元の本文を保持する。
- 標準EDIT: `/q` → Enter → `a` が「qあ」となる。
- 検証後に元のIMEへ復帰して終了。abbrevの辞書検索と候補UIはファイル辞書を使う自動TSFテストで確認した。

同日の辞書保存設定追加後、ユーザーごとのsettings.iniに手元のUTF-8辞書を指定し、
環境変数なしでビルド版DLLを起動して次を確認した。

- 診断欄に設定・辞書の絶対パス、175,779見出し・240,285候補、スキップ2行の番号を表示。
- RichEdit: `Jisho` → Spaceで「辞書」など6候補と注釈を表示し、Enterで確定。
- 標準EDIT: `/skk` → Spaceで「SKK」など2候補を表示し、Enterで確定。
- 両方の確定時に候補一覧が閉じ、既存の他方の本文を保持。元のIMEへの復帰成功後に終了。

自動テストはユーザー設定に依存しないfixtureを使い、保存・置換失敗時の保全、
複数辞書の優先順と部分失敗、CLI、設定不正時のTSFかな入力を検証する。

同日の候補学習追加後、Releaseのビルド版DLLでRichEditの「掛く」を第二候補から確定し、
診断で保存済み世代1・未保存0件を確認した。アプリの終了後、Debugのビルド版DLLを
新しく起動し、標準EDITの `KaKu` で「掛く」が第一候補、「書く」が第二候補になることを確認した。
これは保存したユーザー辞書を別プロセスから読み直した結果。
標準EDITで「書く」を選び直して確定し、保存済み世代2・未保存0件も確認した。
検証後は「書く」が第一候補の順位に戻り、元のIMEへ復帰してアプリを終了した。

入力属性を明示しない元のRichEditでは `GetValue(GUID_PROP_INPUTSCOPE)` がE_FAILとなり、
学習を見送ることも実測した。検証アプリでは `SetInputScope(IS_TEXT)` を追加して解消したが、
属性を提供しない外部アプリでの学習には制限が残る。通常の変換は継続する。

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
| 漢字変換・候補送り・確定 | 確認済み | 確認済み | 未確認 |
| 候補学習・保存順位の利用 | 再起動後の利用と保存を確認済み | 学習保存を確認済み | 属性の提供状況を含め未確認 |
| 候補一覧・注釈 | 未確認 | ビルド版で確認済み | 未確認 |
| PageDown／PageUpによる候補移動 | 自動ホストのみ | 自動ホストのみ | 未確認 |
| 送り仮名 | 確認済み | 確認済み | 未確認 |
| フォーカス移動・IME切替 | 一部確認済み（上記参照） | 別入力欄への移動で確認済み | 未確認 |
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
候補一覧は [ITfCandidateListUIElement](https://learn.microsoft.com/en-us/windows/win32/api/msctf/nn-msctf-itfcandidatelistuielement)、
[BeginUIElement](https://learn.microsoft.com/en-us/windows/win32/api/msctf/nf-msctf-itfuielementmgr-beginuielement)、
[GetTextExt](https://learn.microsoft.com/en-us/windows/win32/api/msctf/nf-msctf-itfcontextview-gettextext) を参照。
学習の入力属性判定は [GetValue](https://learn.microsoft.com/en-us/windows/win32/api/msctf/nf-msctf-itfreadonlyproperty-getvalue)、
検証ホストの属性設定は [SetInputScope](https://learn.microsoft.com/en-us/windows/win32/api/inputscope/nf-inputscope-setinputscope) を参照。
