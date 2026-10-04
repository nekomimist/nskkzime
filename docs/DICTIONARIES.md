# 辞書の利用

現在はSKK辞書を読み取り専用で利用する。元ファイルへの書き込み、単語登録、学習による
候補順の保存はまだ行わない。外部の共有辞書はリポジトリに同梱していない。

入力コアには候補確定時の学習イベントを用意した。`Result.learning` は読み・送りキー・
ひらがなの送り仮名・語幹の候補（注釈・出自を含む）を持ち、Enter、C-j、次の文字入力での
確定時に返す。取消、候補送り、かなの直接確定、未知語、リセットでは返さない。
`DoNotLearn` 指定と `GeneratedLong` の候補は対象外。abbrevのキーは大小文字を保つ。

これは保存前のイベントであり、Engine自身は辞書を更新しない。
ホストは本文への適用に成功してエンジンの新しい状態を採用した場合だけ配送する。
試行コピーの破棄や編集失敗で学習しないよう、TSFと補助プロセスの接続時にもこの条件を守る。
現時点ではTSFからの配送・ユーザー辞書保存は未接続で、CLIの `learning` フィールドと
コアの回帰テストでイベントを検証できる。フォーカス移動などホストによるcomposition終了も対象外。

## CLI

```powershell
# UTF-8 / EUC-JPを自動判別
.\out\build\windows-x64\Release\skk_trace.exe --dict 'C:\辞書\SKK-JISYO.L' 'Kanji<Space><Enter>'

# 最初の辞書を優先。重複した表記と注釈は最初のものを採用
.\out\build\windows-x64\Release\skk_trace.exe --dict 'C:\辞書\user.skk' --dict 'C:\辞書\SKK-JISYO.L' 'KaKu<Enter>'

# 必要な場合は明示指定（すべての --dict に適用）
.\out\build\windows-x64\Release\skk_trace.exe --encoding utf-8 --dict .\tests\fixtures\basic.utf8.skk 'Jisho<Space><Enter>'

# abbrev: 英字見出しをそのまま検索する
.\out\build\windows-x64\Release\skk_trace.exe --dict .\tests\fixtures\basic.utf8.skk '/skk<Space><Enter>'
```

`--encoding` は `auto` / `utf-8` / `euc-jp`。`--dict` 省略時は内蔵の小さなテスト用辞書を使う。
キー列はASCII、Windowsのパス引数はUnicodeに対応する。標準出力はUTF-8のJSON Linesで、
候補一覧の `text`・`annotation` と選択位置 `candidate_index` を含む。
読み込み件数・診断は標準エラーへ出力する。ファイルが開けない、文字コードが不正などの
致命的なエラーでは終了コード2を返す。

## TSFの保存設定

```powershell
# 指定順に優先する。すべて読み込めることを確認してから設定を保存する
.\out\build\windows-x64\Release\nskkzime_config.exe --set-dictionaries 'C:\辞書\personal.skk' 'C:\辞書\SKK-JISYO.L'
.\out\build\windows-x64\Release\nskkzime_config.exe --show
.\out\build\windows-x64\Release\nskkzime_config.exe --check

# 辞書変換を無効にし、かな入力だけにする
.\out\build\windows-x64\Release\nskkzime_config.exe --disable-dictionaries
```

保存先はユーザーの `%LOCALAPPDATA%\nskkzime\settings.ini`。管理者権限は不要。
設定ファイルはUTF-8で、以下の形式を使う。パスは絶対パスで、引用符を付けず、
並べた順に優先する。空白・日本語・`=`・`#` を含むファイル名にも対応する。

```ini
version=1
dictionary=C:\辞書\personal.skk
dictionary=C:\辞書\SKK-JISYO.L
```

行頭の `#` / `;` はコメント。UTF-8 BOMとCRLFにも対応する。
最大32辞書、設定全体は64 KiBまで。未知のキー、重複したversion、不正なUTF-8、
相対パス、制御文字を含むパスは拒否する。行の前後の空白は除く。
`version=1` だけのファイルは辞書なしを意味する。
保存は同じディレクトリの一時ファイルへ書き込み・フラッシュした後で置き換え、
書き込みや置換が失敗した場合は以前の設定を残す。

既定の設定ファイルが存在せず、上書き指定もない場合だけ内蔵テスト用辞書を使う。
設定が壊れている場合は空の辞書でかな入力を継続する。
複数辞書の一部だけが読めない場合は、読めた辞書を設定順に使う。
元の辞書ファイルは変更しない。GUIの設定画面は今後追加する。

検証アプリ下部には、**そのプロセスのDLLが読み込んだ**設定の場所、辞書パス、
件数、文字コード、スキップ数・行番号、読み込み失敗を表示する。
「状態を更新」は表示だけを更新し、辞書の再読み込みは行わない。
古い登録DLLが診断機能を持たない場合は、その旨を表示する。
`--check` は現在のファイルを読み込んで検査するため、起動済みアプリの状態とは異なる場合がある。
CLIの終了コードは正常・非致命的な行スキップが0、設定や辞書の読み込み失敗が2。

## 一時的な上書き設定

```powershell
$env:NSKKZIME_DICTIONARY = 'C:\辞書\SKK-JISYO.L'
# このPowerShellから検証対象アプリを新しく起動する
```

`NSKKZIME_DICTIONARY` には辞書1個の絶対パスを指定し、保存設定より優先する。
`NSKKZIME_CONFIG` には別の設定ファイルの絶対パスを指定できる。
明示指定した設定ファイルが存在しない場合はエラーとし、内蔵辞書には切り替えない。
設定CLIの `--config 'C:\設定\settings.ini'` は `NSKKZIME_CONFIG` より優先し、
そのファイルを表示・検査・保存する。`--check` 時の辞書上書きは引き続き有効。
環境変数はこのPowerShellと、その環境を継承した子プロセスだけに反映される。
既に動作しているアプリや、別の親プロセス経由で起動したアプリには反映されないことがある。
IMEの登録は別途 [TSFの検証](TSF_VALIDATION.md) の手順が必要。

サービスの最初のActivate時に同期で読み込み、キー処理中はメモリーだけを検索する。同じプロセス内で
別のサービスが使用中ならスナップショットを共有する。ファイルの変更は自動再読み込みしない。
設定変更後は対象アプリを終了して起動し直す。大きな辞書の初回読み込みによる待ち時間は残る。
TSF表示属性を問い合わせるためだけに作られたCOMインスタンスでは辞書を読み込まない。

読み込みエラーは検証アプリ・設定CLIで確認でき、`OutputDebugString` にも出す。

## 対応する形式

| 項目 | 現在の扱い |
|---|---|
| 文字コード | UTF-8、Windows上のEUC-JP（SS3を除く）。BOM、先頭行の `coding:`、内容の順で判別 |
| 改行 | LF、CRLF、末尾の改行なし |
| セクション | `;; okuri-ari entries.` / `;; okuri-nasi entries.` |
| セクションなし | ひらがなを含み末尾が英小文字の見出しを送りありと推定。それ以外は送りなし |
| 見出しと候補 | `読み /候補1;注釈/候補2/`。並び順を保持し、同じ見出しの複数行は統合 |
| 送りあり | `かk /書/描/`。検索キーは「か」＋`k` |
| 送り仮名別候補 | `おおk /大/多/[く/多/]/[き/大/]/`。一致する送りの候補を先に提示 |
| 特殊文字 | `(concat "I\057O")` など、引用文字列だけのconcat。八進エスケープ、`\\`、`\"` を解釈 |
| 括弧付きの日本語 | `(株)`・`(笑)` はそのままの表記として保持 |
| Lisp式 | 実行しない。未対応の候補式はスキップ。注釈の未対応式は文字列として保持 |
| 数値変換・動的候補 | `#` の展開、日付などの関数呼び出しは未対応 |

文字コードの自動判別ではUTF-8 BOMを最優先する。BOMがなければ先頭コメント行の
`coding: utf-8` / `utf-8-unix` / `euc-jp` / `euc-jp-unix` を使う。
指定がなければ厳密なUTF-8として読み、失敗した場合にEUC-JPを試す。
未対応のcoding指定はエラーとする。`--encoding` はcoding指定を上書きするが、BOMとの矛盾は拒否する。
EUC-JPはWindowsコードページ20932を使い、バイト列の妥当性と往復変換での一致を確認する。
EUC-JPのSS3（JIS X 0212補助漢字）、EUC-JIS-2004は未対応で、検出時はエラーとする。
該当する辞書は元の文字コードに対応したエディターなどでUTF-8として別名保存し、
先頭のcoding指定も `utf-8` に変更して使う。Windows以外でもUTF-8の辞書を使う。

候補内の改行・タブ・空文字列には未対応。辞書全体は128 MiB、1行は65,536コードポイントまで。
壊れた行は行単位でスキップし、未対応候補は候補単位でスキップする。
件数と先頭32件の行番号・理由を報告する。診断には見出し・候補本文を含めない。
候補数は送り仮名別ブロックを含む、格納した候補の延べ数である。

## 検証

自作fixtureと自動テストで文字コード、注釈、候補統合、送り仮名、欠損ファイルを検証する。
外部辞書での確認結果はその辞書の版に依存するため、自動テストの前提にはしない。
2026-10-04、手元の `SKK-JISYO.L`（EUC-JP、4,489,333 bytes）を読み取り専用で検証し、
175,770見出し・240,252候補を取り込んだ。不正行・未対応候補はともに0件で、
キー列から「辞書」「日本」「書く」の確定を確認した。
2026-10-05には、手元の `SKK-JISYO.utf8.L`（6,156,753 bytes）を保存設定に指定し、
175,779見出し・240,285候補、形式不正2行（38112・38113）・未対応候補0件を確認した。
ビルド版IMEで、環境変数指定なしの実入力「辞書」とabbrev「SKK」の変換・確定も確認した。

参照: [SKK辞書の書式](https://ddskk.readthedocs.io/ja/latest/06_apps.html)、
[SKK辞書プロジェクトの編集規則](https://github.com/skk-dev/dict/blob/master/committers.md)。
