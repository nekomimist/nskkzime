# 辞書の利用

現在はSKK辞書を読み取り専用で利用する。元ファイルへの書き込み、単語登録、学習による
候補順の保存はまだ行わない。外部の共有辞書はリポジトリに同梱していない。

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

## TSFの開発用設定

```powershell
$env:NSKKZIME_DICTIONARY = 'C:\辞書\SKK-JISYO.L'
# このPowerShellから検証対象アプリを新しく起動する
```

現時点では辞書1個の絶対パスを指定する。未設定時は内蔵テスト用辞書を使う。
環境変数はこのPowerShellと、その環境を継承した子プロセスだけに反映される。
既に動作しているアプリや、別の親プロセス経由で起動したアプリには反映されないことがある。
IMEの登録は別途 [TSFの検証](TSF_VALIDATION.md) の手順が必要。

サービスの最初のActivate時に同期で読み込み、キー処理中はメモリーだけを検索する。同じプロセス内で
別のサービスが使用中ならスナップショットを共有する。ファイルの変更は自動再読み込みしない。
設定変更後は対象アプリを終了して起動し直す。大きな辞書の初回読み込みによる待ち時間は残る。
TSF表示属性を問い合わせるためだけに作られたCOMインスタンスでは辞書を読み込まない。

設定したファイルが読めない場合は空の辞書を使い、かな入力を継続する。
エラーは `OutputDebugString` へ出す。詳細確認にはCLIを使う。

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

参照: [SKK辞書の書式](https://ddskk.readthedocs.io/ja/latest/06_apps.html)、
[SKK辞書プロジェクトの編集規則](https://github.com/skk-dev/dict/blob/master/committers.md)。
