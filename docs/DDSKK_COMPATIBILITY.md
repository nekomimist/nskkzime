# DDSKKとの操作比較

2026-10-06に、GNU Emacs 30.2上のDDSKKと `skk_trace` に同じ辞書・キー列を与えた。
比較対象は [DDSKK e8bf68b27021c29781dbfa6a8ef7b770565bdb5b](https://github.com/skk-dev/ddskk/tree/e8bf68b27021c29781dbfa6a8ef7b770565bdb5b)
（2026-03-29）。38例すべてで各実装の期待結果を確認し、28例が一致、10例は下表の差分だった。
入力エンジンの操作比較であり、Windows上のキー配送・画面描画やDDSKK全機能の互換性を保証するものではない。
比較する値は各キー列を実行した後の本文と未確定表示を合わせた文字列。内部状態全体やキーごとの全遷移を比較するものではない。

## 初版で採用する範囲

かな／カナ／英数切替、Shiftによる読み開始・送り境界、Space／xによる候補移動、C-jによる確定、
通常文字による暗黙の確定、C-gによる取消、abbrev、未知語の登録・空登録の取消・送りあり登録・再帰登録を対象にする。
この範囲の結果が一致することを自作の小さな辞書で確認した。送り仮名別の順位・注釈・保存・同期は既存の辞書／TSFテストで検証する。

Shiftを離す遅れについては `KAnji`、`KaKU`、`KOroNde`、`KOroNDE` が一致した。
比較用辞書の `ころn /転/` では後二者とも「転んで」となる。
基本操作の参照は [DDSKKの基本的な入力](https://ddskk.readthedocs.io/ja/latest/05_basic.html)。

## 確認した相違点

`▽` は読み、`▼` は候補を示す比較出力の記号。実IMEでは本文に挿入せず表示属性を使う。
`SPC` はSpace、`RET` はEnter、`BS` はBackspace。

| キー列・操作 | DDSKKの比較結果 | nskkzimeの採用仕様・未対応範囲 |
|---|---|---|
| `Kanji SPC RET` | 「漢字」と改行 | 「漢字」の確定のみ。次のEnterをアプリへ渡す |
| `Kanji SPC BS` | 「漢」にして確定 | `▽かんじ` に戻る |
| `ky BS a` | 「あ」 | 未完成ローマ字を1字ずつ削除するため「か」 |
| `hannou C-j` | 「はんおう」 | 「はんのう」。内蔵 `nn` は後続音節に2個目のnを使う |
| `Kanji SPC 2` | 「漢字2」 | 2番目の「感じ」を選んで確定 |
| `/p` の後にSPCを5回、`j` | 一覧の「候補09」を選んで確定 | 5番目を暗黙に確定し「候補05j」。常に7件表示し、選択は数字1〜7 |
| `fka` | 「か」 | 不成立のローマ字を捨てず「fか」 |
| 学習語を選び `X yes RET` | その入力を取り消す | 削除後は元の読みに戻る |
| `Q7gatu` | `▽7がつ` | 大文字Qによる読み開始は未対応。現在は `▽q7がつ` |
| `Labc` | 「ａｂｃ」 | 全英モードは未対応。現在は `▽lあbc` |

EnterはDDSKKの `skk-egg-like-newline` を有効にした場合に近いが、設定全体の互換を意味しない。
`nn` だけは [ローマ字ルール](ROMAJI.md) でDDSKKに合わせて変更できる。
`skk-delete-implies-kakutei`、候補選択キー・表示開始位置などのDDSKK側設定は既定値で比較した。
参照: [固定版の設定定義](https://github.com/skk-dev/ddskk/blob/e8bf68b27021c29781dbfa6a8ef7b770565bdb5b/skk-vars.el)、
[DDSKKの確定・候補選択・登録削除](https://ddskk.readthedocs.io/ja/latest/06_apps.html)。

Qによる変換開始位置の後付け、Lの全英、半角カナ、数値変換、接頭辞・接尾辞専用操作、補完、再変換、
Emacsバッファ内の移動を利用する編集操作、AZIK等の拡張、Lispでの任意設定は初版の保証範囲に含めない。
Shiftを押す位置が遅れた `ArukU` 等の補正も未対応。対応を広げる際には比較データと入力仕様を更新する。

## 再現方法

通常のCTestに含む `ddskk_contract` は、記録した38例のnskkzime側の結果を検証する。
GNU EmacsとDDSKKを実行する比較は別途行い、通常のビルド時に取得・インストールしない。

```powershell
# nskkzime側の回帰確認。GNU EmacsやDDSKKは不要。
.\scripts\compare-ddskk.ps1 -CoreOnly

# 一度だけ、参照用のDDSKKを取得して版を固定する。
git clone https://github.com/skk-dev/ddskk.git out/reference/ddskk
git -C out/reference/ddskk checkout e8bf68b27021c29781dbfa6a8ef7b770565bdb5b

# GNU Emacs 30.2のポータブル版を展開した例。
.\scripts\compare-ddskk.ps1 -Emacs .\out\deps\gnu-emacs-30.2\bin\emacs.exe
```

比較スクリプトはDDSKKのコミットと変更のない作業ツリーを確認する。
各例を新しいプロセス・空の個人辞書で実行し、学習順位を例間で引き継がない。
`-Q --batch` と子プロセスだけのHOME・SKK設定先を使い、ユーザーの設定・辞書を読まない。
検索対象を比較用辞書と空の個人辞書に限定し、サーバー検索・個人辞書共有・統計記録・注釈表示・自動括弧挿入を使わない。
入力コマンドの差し替えや模倣はせず、`execute-kbd-macro` でキー列を実行する。

辞書は [比較用辞書](../tests/fixtures/ddskk-comparison.utf8.skk)、キー列と両者の期待値は
[比較データ](../tests/fixtures/ddskk-comparison.json)、Emacs側は [ドライバー](../tests/ddskk_driver.el) にある。
結果は実行ごとに `out/ddskk-comparison-<ID>/report.json`、各例のCLI履歴とDDSKK結果に保存する。
`core_only=true` の記録はDDSKKを実行した証拠とは扱わない。
子プロセスには20秒の制限を設け、途中の失敗も記録して終了コードを失敗にする。

今回使用した [GNU Emacs 30.2のWindows配布物](https://ftp.gnu.org/gnu/emacs/windows/emacs-30/) は
`emacs-30.2.zip`、SHA256 `414d3a1a21147af257ebd98bdd15976fdcb5ed0563f6de89f76d4a4b5dad9c72`。
GNUミラーのチェックサムと照合した。Emacs・DDSKK本体は比較専用で、IMEには組み込まない。

## 検証結果

- GNU Emacs 30.2＋固定版DDSKKと通常Releaseの `skk_trace`: 38/38例で期待値を確認。
- `ddskk_contract`: 通常Debug、通常Release、zenz入りReleaseの3構成で成功。
- 初版で採用した操作以外、独自のDDSKK設定、OSごとのキー配送はこの比較の対象外。

現在の動作全体は [入力仕様](INPUT_SPEC.md)、Windowsの確認範囲は [TSFの検証](TSF_VALIDATION.md) を参照。
