# zenz CPUサーバー

`skk-zenz` のprompt/modelを移植し、Windows / MSVCで `convert` と `score` を実行する。
推論は別プロセスの `nkkz-ime-server.exe` に置き、IME DLLにはllama.cppをリンクしない。
固定した参照版とライセンスは [third_party](../third_party/README.md) を参照。

## 準備とビルド

PowerShell 7、Build Tools 2022、Hugging Faceの `hf` CLIを使う。
準備スクリプトだけがネットワークへ接続し、固定版のソースとモデルをSHA-256で検査する。
生成物・モデルは `out/` に置く。

```powershell
.\scripts\prepare-zenz.ps1
.\scripts\build.ps1 -Configuration Release -Zenz
```

出力は `out/build/windows-x64-zenz/Release/`。通常版と別のビルドディレクトリーを使う。
現構成はx64 CPU専用で、AVX2/FMA/F16C/BMI2を使う。GPU対応や別CPUへの配布は未検証。
標準ビルドはモデルなしのダミーサーバーを引き続き生成する。

## 単体での実行

```powershell
$server = '.\out\build\windows-x64-zenz\Release\nkkz-ime-server.exe'
$model = (Resolve-Path '.\out\models\zenz-v3.2-small\ggml-model-Q5_K_M.gguf').Path
& $server --model $model --convert 'かいとう' --left '試験問題の' -n 5
& $server --model $model --convert 'かいとう' --left '冷凍食品を電子レンジで' --score '回答' --score '解答' --score '解凍'
& $server --model $model --stdio --threads 4 --timeout-ms 1000
```

変換結果は候補とスコア、採点結果は入力順のスコアをUTF-8 JSONで返す。スコアは大きいほど優先。
標準入出力モードは起動時にhelloを返し、以後1行につき1要求を処理する。
不正な行には本文を含まないerrorを返して次の行へ進む。標準エラーは診断用で、入力本文を保存しない。

```json
{"id":1,"op":"convert","kana":"かいとう","left":"試験問題の","right":"","n":5}
{"id":2,"op":"score","kana":"かいとう","left":"試験問題の","right":"","candidates":["回答","解答","解凍"]}
```

文脈は前後各40 Unicode文字、変換候補数は1〜8、採点候補数は0〜64、読み・候補は1024文字まで。
実際にはモデルのコンテキスト長による制限もあり、上限内の入力すべての成功を保証するものではない。
モデル入力のかな・空白・文脈は移植元と同じ正規化を行う。
行長等の契約は [推論IPC](INFERENCE.md) を参照。

名前付きパイプでも同じ要求を使える。

```powershell
& $server --model $model --pipe-token local-zenz --threads 4 --timeout-ms 1000 --idle-ms 10000
```

同一ユーザーの4接続を受け付け、1個のモデルコンテキストへの推論を直列化する。
待機中も取消と期限を確認し、計算中はllamaのabort callbackでも停止する。
切断・取消後には次の要求で再利用できる。接続受付、モデル待ちと計算に上限があり、無通信時は自動終了する。

## 検証と計測

2026-10-06に、モデル入りRelease版のCTest 38件をスキップなしで通過した。
移植元のprompt/modelテストに加え、実パイプの並行採点、切断後の復帰、取消可能なモデル待ち、
CLIと標準入出力のJSON Lines、異常行の後の復帰、日本語・空白を含むモデルパスを確認した。
`tsf_zenz` は保存設定からビルドしたDLLを実際に読み込み、自動起動・文脈付き変換・確定後の入力・
複数サービスの共有・取消・文脈変更後の破棄・private属性・停止後の再起動・欠損モデルからの復帰を検査する。
実モデルによる辞書候補のpromoteと、長い読みの生成優先も同じDLL経由で確認した。
`inference_policy` は11ケースで候補統合・採点・学習の規則を、`tsf_inference_policy` は遅延する実パイプで
キー／クリック操作・期限切れ・編集拒否後の候補保持・送り仮名と文脈・短い生成語の永続化を検査する。
これはテスト用TSFホストによる検証で、実アプリのキー配送や表示をすべて再現するものではない。
通常版Debug／Releaseの33件も成功している。

```powershell
.\scripts\bench-zenz.ps1 -Rounds 30 -Output out/zenz-benchmark.json
```

固定3例を各30回、変換90要求・採点90要求で測った初期値。Release、4スレッド、
AMD64 Family 25 Model 97、32論理プロセッサ。時間にはPowerShell/JSON/標準入出力の処理を含む。

| 処理 | p50 | p95 |
|---|---:|---:|
| 5候補の生成 | 45.240 ms | 69.514 ms |
| 3候補の採点 | 27.267 ms | 28.762 ms |

プロセス起動からhelloまでは61.779 ms。OSファイルキャッシュを消去していないため、
初回ディスク読み込みの測定ではない。3例とも期待する第一候補になったが、固定した少数例の確認であり、
一般的な変換精度・長文性能・実アプリでの入力遅延は別途評価する。

## IMEとの接続

ビルド版DLLのプローブだけで試すには次を実行する。初回のIME登録は済んでいる必要がある。

```powershell
.\scripts\probe-zenz.ps1
```

「nskkzimeを選ぶ」→ Ctrl+j → `Koukounokyoushi` → Spaceで「高校の教師」への変換を試せる。
専用の一時設定を使い、辞書は空、学習はオフ。プローブを閉じると専用設定を削除する。
日常の辞書と設定は読まず、登録先DLLも変更しない。実モデルを使ったプローブの画面操作は未検証。

通常利用では、新しいDLLへ登録を更新した後に、設定CLIから絶対パスを保存する。
旧DLLは新しい設定キーを認識できないため、登録更新を先に行う。

```powershell
$build = (Resolve-Path '.\out\build\windows-x64-zenz\Release').Path
$model = (Resolve-Path '.\out\models\zenz-v3.2-small\ggml-model-Q5_K_M.gguf').Path
& "$build\nskkzime_config.exe" --set-zenz $model "$build\nkkz-ime-server.exe"
& "$build\nskkzime_config.exe" --check
# 無効化する場合
& "$build\nskkzime_config.exe" --disable-inference
```

`--set-zenz` はファイルの存在・パスを検査して保存する。モデル形式やサーバーの実行可否を検査する操作ではない。
通常の辞書設定と同じ `settings.ini` を使い、辞書・学習・ローマ字の既存設定を保持する。
設定変更後は対象アプリを起動し直す。必要なら `--config 絶対パス` で別設定へ保存できる。

```ini
inference=zenz
zenz-model=C:\models\ggml-model-Q5_K_M.gguf
zenz-server=C:\nskkzime\nkkz-ime-server.exe
inference-threads=4
inference-timeout-ms=1000
```

スレッド数は1〜64、期限は1〜10000ms。IMEでは接続・モデルの起動待ち・計算・編集待ちを含む絶対期限を使う。
初回起動が期限に間に合わない場合も、次の要求では起動済みサーバーへ接続できる。
同じ設定のアプリは1サーバーを共有し、無通信10秒後に終了する。同じパスのモデルを置換した場合は
既存サーバーが終了するまで旧モデルが残る。生成候補は既定では学習しない。
送りあり・abbrev・登録欄内の生成は行わない。送りありは既存候補の採点に対応する。
保護・属性不明の入力欄では要求しない。古い結果やサーバー障害を処理する条件は [推論IPC](INFERENCE.md) を参照。

## 生成の優先と候補の採点

zenzを有効にしたIMEでは、10文字以上の読みを初めて変換すると生成を開始する。
辞書候補はすぐ表示でき、応答が届くまでそのまま操作できる。まだ候補を操作していなければ、
生成候補を先頭へ追加して選ぶ。同じ表記は生成側を残して重複を除き、残りの辞書候補の順序を保つ。
生成に失敗した場合や、候補の操作を始めた場合は辞書候補で継続する。
辞書候補がないときは読みを保ち、失敗時には通常の登録へ進む。
生成を試した一覧では、後で候補を使い切っても同じ要求を繰り返さない。

短い読みは辞書を優先する。辞書にない読みや候補を使い切った場合に生成し、末尾へ追加する。
読みと同じかな表記、重複、不正な文字、最良候補からスコアが8を超えて低い生成候補は除外する。
対象の読みは移植元と同じく、ひらがな・反復記号・長音と `、。・！？` に限る。

採点による順位調整は既定でオフ。有効にすると、初回の辞書候補から先頭20件までを採点する。
最良候補のスコアが元の第一候補を1.0より大きく上回る場合だけ、その候補を先頭へ移す。
残りの順序・ID・注釈・学習方針は保持する。保存済みの生成語もユーザー辞書の候補として採点する。
送りありでは読みと候補に送り仮名を付けて採点し、保存する候補は元の語幹のままにする。
長い読みの生成を行う場合は、その変換で採点を重ねて要求しない。

採点の既定期限は300ms。期限切れや異常応答は、現在の辞書順を保つ。
Space、x、ページ移動、数字選択、確定、取消、追加入力、候補上でのマウス押下で待機中の順位変更を無効にする。
応答が既に届いて編集ロック待ちの場合も同じ条件で破棄する。

```powershell
# 0なら長い読みの生成優先を無効化（候補枯渇後の生成は継続）
& "$build\nskkzime_config.exe" --set-zenz-min-length 10
& "$build\nskkzime_config.exe" --set-zenz-rerank on
# 通常の学習設定もonである場合に限り、短い追加生成語を保存
& "$build\nskkzime_config.exe" --set-zenz-learning on
```

生成語の学習は既定でオフ。明示的に有効にした場合も、候補枯渇後の生成で、読み・表記とも9文字以下に限る。
長い読みを優先して生成した候補は長さに関係なく学習しない。本文への確定が成功し、
入力属性で学習を許可された場合だけ保存する。ダミー候補は常に学習しない。

細かい調整は保存設定の次のキーで行う。設定変更は新しく起動した対象アプリへ反映する。

| キー | 既定値 | 範囲・意味 |
|---|---:|---|
| `zenz-min-length` | 10 | 0〜1024。0は長い読みの優先を無効化 |
| `zenz-rerank` | off | on / off。promoteによる順位調整 |
| `zenz-rerank-limit` | 20 | 2〜64。先頭から採点する候補数 |
| `zenz-score-timeout-ms` | 300 | 1〜10000。接続から編集適用までの採点期限 |
| `zenz-promote-threshold` | 1.0 | 0〜100。第一候補を上回る必要のある差 |
| `zenz-max-score-gap` | 8.0 | 0〜100。生成候補を残す最大スコア差 |
| `zenz-learn-fallback` | off | on / off。短い追加生成語の学習 |

サーバーの計算上限は生成／採点の長い方の期限に合わせ、各クライアントは要求ごとの短い期限で接続を取り消す。
全面的な並べ替えを行うmix方式、送りありの新規生成、入力中の先読みは未対応。
