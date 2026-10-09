# 開発

## ビルドとテスト

PowerShell 7、Visual Studio 2022 Build Tools（C++、Windows SDK、C++ CMake tools）、
モデル取得用のHugging Face `hf` CLIを使う。

```powershell
.\scripts\prepare-zenz.ps1                 # 固定版のllama.cppとモデルを out/ へ取得・検査
.\scripts\build.ps1 -Configuration Release # x64とx86をビルドしてCTestを実行
```

- 出力は `out/build/windows-x64-zenz/Release/`、x86のDLLと検証アプリはその `x86/`。
- `-X86` はx86側だけ、`-TestOnly` はllama.cppなしの軽量構成（配布不可）、`-Jobs N` は並列数。
- テストはzenz入りReleaseを基準にする。実モデルを使うテストは、モデルがなければスキップする。

テストが本人の設定や学習データを読まないよう、CTestはすべてのテストに `NSKKZIME_CONFIG` として
存在しないファイルを渡す。各テストは自分のフォルダーに `settings.ini` を書いて `NSKKZIME_CONFIG` で指定する
（`tests/isolated_settings.h`）。そのフォルダーにユーザー辞書と辞書キャッシュも置かれ、
推論の接続先とbrokerの接続先も設定ファイルから決まるため、インストール済みのIMEとは混ざらない。
偽の推論サーバーは `current_inference_config()->token` のパイプで待ち受ける。

| 範囲 | 主なテスト |
|---|---|
| 入力コア | `core`, `romaji`, `registration`, `forget`, `ddskk_contract`（DDSKKとの比較例） |
| 辞書 | `dictionary`, `compiled_dictionary`, `dictionary_cache`, `runtime_dictionary` |
| 学習 | `learning`, `user_dictionary`, `user_dictionary_cli`, `dictionary_manager`, `tsf_learning` |
| 設定 | `settings`, `settings_editor`, `config_cli`, `runtime_configuration`, `tool_launcher` |
| TSF | `tsf_smoke`, `tsf_candidate_click`, `input_mode`, `context_text`, `candidate_ui`, `tsf_romaji`, `tsf_diagnostics`, `tsf_dictionary_startup` |
| 推論 | `inference`, `inference_policy`, `tsf_inference`, `tsf_inference_policy`, `zenz_backend`, `tsf_zenz` |
| AppContainer | `app_container`（一時的なAppContainerプロファイルで実際に子プロセスを起動）, `broker_snapshot` |
| 配布 | `package`（梱包と検査）, `installation`（登録は模擬。導入・更新・復帰・解除） |

TSFのテストは自作のテスト用ホスト（`tests/tsf_test_host.h`）で動かす。実アプリのキー配送や描画の再現ではない。
デスクトップ用TSFテストは通常プロセスとして実行する。AppContainer内では辞書がbroker経由になり、
テスト専用設定のローカル辞書を待つ試験が初期化タイムアウトになる。
`tsf_diagnostics` はログ有効・無効で本文・キーの消費・ホスト呼び出し数が変わらないこと、
終了理由、再入・非同期通知、本文を保存しないこと、保存上限・キューの欠落を検証する。
`tsf_smoke` は一時的なフォーカス先なし・GetTop失敗・空のtopからの復帰、次キーが先行する場合、
継続するフォーカス離脱、別入力先への移動、古い非同期予約とGetTop中の再入による入力状態の保護も検証する。

## outの配置

`out/` はGitの管理外。直下には用途別のフォルダーだけを置く。

| フォルダー | 内容 |
|---|---|
| `build/` | CMakeのビルド先。配置を変える場合は再configureが必要 |
| `deps/` | llama.cppなどの依存ソースと取得したアーカイブ |
| `models/` | zenzモデル。IMEの設定からも参照する |
| `reference/` | DDSKKやAJIMEE-Benchなどの比較資料 |
| `huggingface/` | モデル取得時のhfキャッシュ（必要時に作成） |
| `packages/` | 明示的に作成した配布用ZIPとその展開先 |
| `reports/` | 評価・比較・ベンチマークの結果と保存する検証ログ |
| `tmp/` | 一時的な検証・導入作業。作業ごとにサブフォルダーを作る |
| `archive/` | 過去の作業ファイルの退避先 |

手動の検証ログは `out/reports/<検証名>/`、使い捨ての設定や作業ファイルは `out/tmp/<作業名>/` に置く。
`register-dev.ps1` は `tmp/` 内で梱包・展開し、導入成功時に作業フォルダーを削除する。
失敗時は調査用に残し、場所を表示する。AppContainerテストも成功時は自分の作業フォルダーを削除する。
退避した古いビルドや設定には元の絶対パスが含まれるため、そのままの再実行は想定しない。

## 手元での確認

| ツール | 用途 |
|---|---|
| `skk_trace.exe` | キー列を入力コアで再生し、未確定・確定の遷移をJSON Linesで出力（`--dict`、`--romaji`） |
| `ime_probe.exe` | 登録済みのIMEで標準EDIT／RichEditの入力を試す。辞書・推論・学習の状態と、遅延計測（本文なし）を表示 |
| `ime_probe_local.exe` | ビルド先のDLLを読み込む同じ検証アプリ（IMEの登録自体は必要） |
| `scripts/probe-zenz.ps1` | 一時的な設定でzenzを有効にして `ime_probe_local` を起動 |
| `nkkz-ime-server.exe --model M --convert かな [--score 候補]...` | モデル単体の生成・採点 |

隔離した設定で試すには、別の `settings.ini` を用意して `NSKKZIME_CONFIG` に絶対パスを指定し、
その環境から検証アプリを起動する。

```powershell
skk_trace.exe --dict 'C:\辞書\SKK-JISYO.L' 'Kanji<Space><Enter>'
```

## 導入

`scripts/register-dev.ps1` は現在のReleaseを梱包して導入する（管理者のPowerShell 7で実行）。
配布用ZIPを使う場合は次のとおり。

```powershell
.\scripts\package.ps1 -Output out\packages\nskkzime.zip
.\scripts\package.ps1 -Verify out\packages\nskkzime.zip
# 展開したフォルダーで、管理者のPowerShell 7から
.\tools\install.ps1 -Action Install
.\tools\install.ps1 -Action Status
.\tools\install.ps1 -Action Rollback -Version <版ID>
```

- 各版を `%ProgramFiles%\nskkzime\versions\<マニフェストのSHA-256>` に置き、x64・x86のDLLを登録する。
  旧版のファイルは残す。失敗や中断は `-Action Recover` で変更前に戻す。
- brokerをHKCUのRunに登録し、通常権限のExplorer経由で起動し直す。
  データが仮想化されたアプリ（パッケージ化されたターミナルなど）から実行すると、HKCUへの書き込みが
  別の場所に行くため拒否する。スタートメニューから起動したPowerShellを使う。
- 起動中のアプリは古いDLLを使い続ける。全体を切り替えるにはサインアウト・サインインする。
- ZIPにはマニフェスト（ハッシュ・依存DLL・ビルド情報）と依存ライセンスを含める。
  モデル・辞書・設定は含めない。各バイナリーにはビルド元のrevisionと内容ハッシュを埋め込み、
  別のビルドが混ざったZIPは作らない。署名はしていない。

## 評価と比較

### zenzの品質・遅延（2026-10-06）

```powershell
.\scripts\bench-zenz.ps1 -Rounds 30 -Output out\reports\zenz-benchmark.json
git clone https://github.com/azooKey/AJIMEE-Bench.git out\reference\AJIMEE-Bench
git -C out\reference\AJIMEE-Bench checkout --detach 401666cd56d1a570c2021798b64b6da4396bfd45
python -B scripts\evaluate.py --dictionary SKK-JISYO.utf8.L --ajimee out\reference\AJIMEE-Bench\JWTD_v2\v1\evaluation_items.json --rounds 3 --stress-rounds 5 --key-rounds 1000 --output out\reports\evaluation-new
```

- 単体の計測（Ryzen 9 7950X3D、4スレッド）: 5候補の生成 p50 45ms／p95 70ms、3候補の採点 p50 27ms／p95 29ms、
  サーバー起動からhelloまで約62ms（OSのファイルキャッシュあり）。
- AJIMEE-Bench 200例（左文脈あり）: 生成の第一候補が173例で正解、IMEの候補統合規則まで適用すると145例。
- 自作の診断28例: 辞書候補の採点で第一候補の正解が11例から27例へ（文脈なしでは10例に悪化）。
- 128文字以上の長い読みの生成は失敗するか、内容が欠ける。
- 評価スクリプトにはPython 3.11以上（標準ライブラリのみ）が必要。結果は既定で `out/reports/` に出力し、本文を含むのでリポジトリには入れない。

### DDSKKとの比較

`tests/fixtures/ddskk-comparison.json` に、GNU Emacs 30.2＋DDSKK `e8bf68b` と同じ辞書・キー列で
取った38例の結果を記録している（一致28例、相違10例。相違は [USAGE.md](USAGE.md#ddskkとの違い)）。
CTestの `ddskk_contract` はnskkzime側の結果だけを検証する。DDSKK側を実行し直すには次を使う。

```powershell
git clone https://github.com/skk-dev/ddskk.git out\reference\ddskk
git -C out\reference\ddskk checkout e8bf68b27021c29781dbfa6a8ef7b770565bdb5b
.\scripts\compare-ddskk.ps1 -Emacs <emacs.exeのパス>
```

### 入力の遅延

`ime_probe.exe` の「遅延計測（本文なし）」で、TSFのキー処理と候補の描画までのp50/p95を表示できる。
OSのキー配送や画面への表示までは含まない。RichEditでの試行ではキー処理のp95が約16msで、
目標の5msには届いていない（大半はホスト側の本文編集）。
