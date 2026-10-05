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

2026-10-05に、モデル入りRelease版のCTest 31件をスキップなしで通過した。
移植元のprompt/modelテストに加え、実パイプの並行採点、切断後の復帰、取消可能なモデル待ち、
CLIと標準入出力のJSON Lines、異常行の後の復帰、日本語・空白を含むモデルパスを確認した。
`tsf_zenz` は保存設定からビルドしたDLLを実際に読み込み、自動起動・文脈付き変換・確定後の入力・
複数サービスの共有・取消・文脈変更後の破棄・private属性・停止後の再起動・欠損モデルからの復帰を検査する。
これはテスト用TSFホストによる検証で、実アプリのキー配送や表示をすべて再現するものではない。
通常版Debug／Releaseの26件も成功している。

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
既存サーバーが終了するまで旧モデルが残る。生成候補は現段階ではすべて学習しない。
送りあり・abbrev・登録欄内の生成、長い読みの生成優先、辞書候補の採点による並べ替えは未接続。
保護・属性不明の入力欄では要求しない。古い結果やサーバー障害を処理する条件は [推論IPC](INFERENCE.md) を参照。
