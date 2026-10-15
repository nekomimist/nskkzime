# ARM64対応のロードマップ

Windows 11 ProのSurface実機を検証先とし、既存のx64版を維持しながら段階的に進める。
初期段階は専用フォルダーで実行し、IME登録・普段の設定・学習辞書を変更しない。

## 段階と合格条件

| 段階 | 作業 | 合格条件 |
|---|---|---|
| 1. 依存の比較 | upstream llama.cpp v0.6.0へzenzのtokenizer差分を移植し、既存forkと並べる | 同じモデルのtoken IDとtoken pieceが一致し、x64のモデル・IPC・取消テストが通る |
| 2. クロスビルド | MSVC ARM64で推論なしの本体とテストを生成する | DLL・EXEがARM64で、ビルドが完了する。実行確認とは区別する |
| 3. Surfaceで実行 | SSHで成果物を送り、隔離した設定でCTestを実行する | core・辞書・TSF・IPC・AppContainerの結果と失敗ログを回収する |
| 4. ARM推論 | Clang系でportableなCPU backendをビルドし、呼び出し側とserverのCPU契約を分離する | 同じGGUFで生成・採点・取消・復帰が動き、実測した遅延で期限を判断する |
| 5. IMEとして統合 | ARM64X TSF DLL、ARM64のserver/broker、x86 companionを組み合わせる | ARM64・x64・ARM64EC・x86ホストから登録経由で同じIMEを使える |
| 6. 導入と配布 | 登録・更新・復帰・解除、Scoop、ランタイム、マニフェスト検査、ARM CIを整える | 実機の導入試験とアーキテクチャ別の回帰試験が通る |

段階1〜4の未登録プロトタイプを実装し、Surfaceで実機検証した。
ARM64成果物とupstream移植構成は実験用であり、既存の配布経路では拒否する。
ARM64Xの未登録プロトタイプも追加した。登録経由の実入力と導入・配布は引き続き別の合格条件とする。

## ツールチェーン

既存のVisual Studio 2022 Build Toolsに次を追加する。

- `Microsoft.VisualStudio.Component.VC.Tools.ARM64`
- `Microsoft.VisualStudio.Component.VC.Tools.ARM64EC`
- `Microsoft.VisualStudio.Component.VC.Llvm.Clang`
- `Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset`

TSFはMSVC ARM64/ARM64EC、ARM向け推論はClangCLを使う。
LLVMの版とWindows SDK/CRTとの組み合わせは実際のビルドで確認する。
別プロセスの推論serverまでARM64ECにする必要はない。

upstream v0.6.0にはMSVC ARM対応があるが、`GGML_NATIVE=ON`とCPU機能を実行して調べる
configure試験を要求する。x64上のクロスビルドや、他のARM端末への配布にそのまま採用しない。
SurfaceでのMSVC native buildを比較実験に使うことは可能。x64版のAVX2 baselineは維持する。

## 依存比較の再現

実験用ソースは、upstream v0.6.0のコミット`8345f333951c661d166b00e6f9362e553768f292`に固定する。
取得したZIPのSHA-256は`ea153f18923e18f47d8c52c55cde0fbc446102cccfa83356964fe7620b7c8898`。
`third_party/patches/llama-v0.6.0-zenz.patch`を適用し、既存forkとは別のフォルダーでビルドする。
Metal固有の環境変数対応は移植しない。新しいpre-typeのenum値はupstreamと衝突しない61を使う。

```powershell
.\scripts\prepare-zenz.ps1 -UpstreamExperimental -SkipModel
cmake --preset windows-x64-upstream-experimental
cmake --build --preset release-upstream-experimental --parallel 8
ctest --preset release-upstream-experimental
```

`zenz_tokenizer`は、同じ固定モデルで既存forkから採取したfixtureを使う。
空白・改行・タブ・空文字列、日本語・ASCII・全角・絵文字・結合文字、特殊トークン、
実際のpromptとspecial-tokenフラグの組み合わせについて、token IDとpieceのバイト列を比較する。
実モデルの生成・採点・取消・IPCは既存テストで確認する。数値誤差とtokenizer互換性は別に評価する。
通常CIではモデルがない試験をスキップするため、モデルを置いた実行結果も必須とする。

## ARM64のクロスビルド

```powershell
# x64の開発PCで実行する。ARM64 EXEはここでは実行しない。
.\scripts\build.ps1 -Architecture ARM64 -TestOnly -BuildOnly -Configuration Release
```

出力は`out/build/windows-arm64-test-only/Release/`。
推論を含む実験構成は次のコマンドを使う。

```powershell
.\scripts\prepare-zenz.ps1 -UpstreamExperimental
.\scripts\build.ps1 -Architecture ARM64 -UpstreamExperimental -BuildOnly -Configuration Release
```

出力は`out/build/windows-arm64-upstream-experimental/Release/`。
本体とadapterはMSVC、llama.cppだけを独立したClangCLプロジェクトでビルドする。
Release `/MD`、`armv8-a`、OpenMP/KleidiAI/追加CPU variants/LTO無効に固定する。
生成した4本の静的ライブラリーとchild cacheのhash、Clangの版、CPU契約を本体のビルド情報に記録する。
実験構成はRelease専用とし、Debugへ流用しない。

ARM64 Windows上でのテストは`ctest --preset release-arm64-test-only`を使う。
現在のARM presetはx64専用のpackage/installation/Scoopテストを除外する。
それらのARM対応は段階6で追加し、既存x64 CIでは引き続き実行する。
未導入環境へバイナリーだけを送る場合はMSVC ARM64ランタイムと依存DLLも確認する。
CTestはビルド先などの絶対パスを保持するため、フォルダーをコピーするだけでは移動できない。
次のコマンドで、実行ファイル・fixture・私用のCMake/CTestを含む移送用フォルダーを作る。
推論付き構成では固定モデルも含む。出力先は新しいフォルダーを指定する。

```powershell
.\scripts\prepare-arm64-tests.ps1 `
  -BuildDirectory out/build/windows-arm64-upstream-experimental `
  -OutputDirectory out/tmp/arm64-runtime
```

Surfaceの専用フォルダーへコピーし、RDP上のPowerShell 7から
`.\run-arm64-tests.ps1`を実行する。`-Name '^(zenz_tokenizer|zenz_model)$'`で対象を絞れる。
結果は`reports/<日時と識別子>/`に保存され、既存の結果を上書きしない。
portable CTestのドライバーはx64エミュレーションで動くが、検査対象のEXE/DLLはすべてARM64であることを転送前に確認する。

CPU検査は、x64の`zenz-msvc-avx2-v1`とARM64の`zenz-arm64-armv8a-v1`を区別する。
設定ファイルの構文・ファイル存在検査と、実行可能なCPUかの判定を分ける。
起動処理では実際のserverのPE形式を読み、対応する契約を検査してからプロセスを起動する。
開発用serverの上書きも、PEのアーキテクチャごとにこのビルド契約を満たす必要がある。
PEヘッダーから任意のコンパイラフラグまでは推定できない。
`--check-cpu`は設定・モデルを読まず、そのconfigツールと同梱する版の契約を報告する。
x64 serverをARM64上のエミュレーションで起動する経路は引き続き拒否する。
短時間の同期候補採点でも、PE検査・起動・IPCは独立したworker/brokerが行う。
入力スレッドは残りの採点予算だけ待ち、期限内ならcold startの結果も最初の候補表示に使う。
期限切れは取消を通知して非同期採点へ戻る。callbackは要求・handle・DLL参照を保持してから終了し、
同じclientの同期用callbackは最大1つとする。起動前後の期限・取消を再確認するが、開始済みのOS呼出し自体は中断できない。

## ARM64Xとホスト別の未登録試験

MSVCのARM64とARM64ECでTSFを別々にビルドし、Microsoftの2段階リンク方式で
1本のARM64X DLLへ結合する。推論serverとbrokerは純粋なARM64の別プロセスに保つ。
ARM64・x64・ARM64ECのテストホストをDLLと同じフォルダーに置き、x86 companionは`x86/`へ置く。
ARM64ECをx86 companionとして扱わず、共有TSFプロファイルの登録規則とビルド範囲を分離する。

```powershell
# VS 2022 17.11以降、MSVC ARM64/ARM64EC、ClangCLが必要。Release /MD専用。
.\scripts\build-arm64x.ps1 -Jobs 4
.\scripts\prepare-arm64x-tests.ps1 -OutputDirectory out/tmp/arm64x-tests
```

成果物は`out/arm64x/Release/`、転送用フォルダーは指定した出力先に生成する。
準備スクリプトは既存の出力フォルダーを上書きしない。Surfaceの専用フォルダーへ転送し、
RDPセッションで`.\run-arm64x-tests.ps1`を実行する。
`-Name '^arm64x_'`でホスト別の追加試験だけを実行できる。
テストは設定・辞書を隔離してDLLを直接読み込み、`DllRegisterServer`と`DllUnregisterServer`を呼ばない。

native側の`/LINKREPROFULLPATHRSP`から絶対パスのOBJ・LIB・DEFを取り込み、
ソース、コンパイラ、SDK、CRT、C++フラグ、cacheと全入力のSHA-256を照合してからEC側へ渡す。
SDKのARM64 import libraryに含まれるEC用メンバーは許容し、プロジェクトの静的ライブラリーはARM64に限定する。
入力が古い場合はEC単独ビルドを拒否し、上のスクリプトでnativeから再ビルドする。
最終DLLのリンクは`/MACHINE:ARM64X`だけを指定する。CMakeがEC用に追加する
`/machine:ARM64EC`をDLLのリンクフラグから除かない場合、このツールチェーンではnative CRTの初期化シンボルが未解決になる。
`-TraceLink`で実際のリンク引数とライブラリー選択を記録できる。

DLL内にはnative/ECそれぞれのビルド情報を保持し、`arm64x-build.json`に両方の情報とDLLのhashを記録する。
各ホストの`tsf_architecture_tests`は、同じDLLパスと対応するdata export、11個の関数export、
アイコンresource、class factory生成・参照解放を確認する。x64/ARM64ECはECの実装、ARM64はnativeの実装を使う。
PEのmachine値だけではx64とARM64ECのホストを区別できないため、ホスト自身のコンパイル時ABIも報告する。
転送用manifestで全71個のEXE/DLLと固定モデルを照合した後、nativeの50試験とホスト別の16試験を直列実行する。

この試験は登録済みの一般アプリへの入力を保証しない。
次に登録・解除・復帰の手順を整え、EDIT/RichEditと実際のアプリで入力経路を確認する。

## Surfaceへの接続

画面操作はWindows 11 ProのRDP、コマンド実行と転送はWindows標準OpenSSH Serverを使う。
TailscaleのDNS名/IPで接続する。RDP・SSHをインターネットへ直接公開する必要はない。
接続用のホスト名、ユーザー名、鍵はリポジトリーに保存しない。

Surfaceの管理者Windows PowerShell 5.1（`powershell.exe`）でOpenSSH Serverを有効にする。
PowerShell 7でNetSecurityモジュールの`System.Runtime`読み込みエラーが出る場合も、
このOS管理手順はWindows PowerShell 5.1の`-NoProfile`環境で実行して切り分ける。

```powershell
Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0
Set-Service sshd -StartupType Automatic
Start-Service sshd
whoami
ssh-keygen -lf "$env:ProgramData\ssh\ssh_host_ed25519_key.pub" -E sha256
```

接続前にホスト鍵の指紋を照合する。認証には公開鍵を設定する。
通常ユーザーは`~/.ssh/authorized_keys`、管理者グループのユーザーは既定設定では
`%ProgramData%/ssh/administrators_authorized_keys`を使い、Microsoftの手順に従ってACLを設定する。
秘密鍵はSurfaceにもリポジトリーにもコピーしない。SSHの受信許可は検証元のTailscale IPに絞る。

テスト配置先は専用フォルダーとし、設定・辞書・接続tokenも隔離する。
SSHからの試験と対話デスクトップが必要な試験を切り分け、後者はRDPセッションで実行する。
SSHはsession 0で動作し、TSF thread managerや候補ウィンドウの試験には適さない。
自動実行する場合は、既にログイン済みの同じユーザーへ`TASK_LOGON_INTERACTIVE_TOKEN`で
一時的なオンデマンドタスクを登録し、完了後にそのタスクだけを削除する。パスワードの保存やIMEの登録は不要。
RDPではSurface側のnskkzimeがキーを処理していることを確認する。
接続元のIMEが変換済みの文字を送るだけの経路はIME試験として扱わない。

## 実入力と導入の確認

- 各アーキテクチャの`ime_probe`でEDIT/RichEdit、切替、未確定表示、候補、確定・取消、フォーカス移動。
- ARM64アプリ、x64/ARM64ECアプリ、x86アプリを実際のプロセスのアーキテクチャで識別。
- ブラウザー、VS Code、Windows検索・Store、利用するWord/Excelで入力・登録・学習。
- 共有COM登録からの生成、実際にロードしたDLLパス、x86解除後の共有TSFプロファイル保持。
- brokerのサインイン起動、推論停止後の辞書入力、設定保存、更新・復帰・解除。
- Surface本体での入力遅延、zenzの初回/連続実行、DPIと候補位置、キーボード、スリープ復帰。

## 2026-10-16の検証結果

- 開発PCへMSVC ARM64/ARM64ECとClang 19.1.5を追加した。
- 推論付きARM64 Releaseの50個のEXE/DLLがARM64形式であることを確認した。ClangCLの外部ライブラリーを必要とするテストの単独ビルドと、変更なしの増分ビルドも成功した。
- SurfaceのSSH鍵認証・専用フォルダーへの転送・portable CTestの実行が成功した。RDPのsession 1で、ARM64のPowerShellから実行した。

| 構成 | 結果 | 主な検証範囲 |
|---|---|---|
| Surface / ARM64 / upstream + zenz patch | 50/50成功 | 96通りのtokenizer互換性、実モデルの生成・採点・取消・復帰、TSF、AppContainer、cold start、同期期限切れ後のclient破棄 |
| Surface / ARM64X / ARM64・x64・ARM64EC・x86 hosts | 66/66成功 | nativeの50試験とホスト別の16試験。同じARM64X DLLから対応する実装をロードし、辞書・学習・native ARM64 serverとの実モデル連携を確認。x86は別DLL |
| 既存x64 / azooKey fork | 53/53成功 | 最新の共通修正、実モデル、TSF、AppContainer、配布パッケージの検査。installationのみ除外 |
| 既存x86 companion | 16/16成功 | TSF、CPU検査、x64 helperとの連携 |
| 依存移植時のx64 / upstream + zenz patch | 53/53成功 | 既存forkとのtokenizer比較と実モデルの回帰 |

ARM64のpackage/installation/Scoopは未対応のため除外した。
初回のSSH session 0では画面試験4件が失敗したが、同じバイナリーをRDPで実行すると4/4成功した。
従来のx64専用CPU判定を分離した後、同期採点のcold start経路も修正して全50件が成功した。
一時的なオンデマンドタスクは削除済み。IME登録と普段の設定・辞書は変更していない。
比較構成を同時にテストした初回にはTSF/AppContainerの失敗が出た。直列では再現していないが、原因は未確定のため構成間のCTestは直列で実行する。

ARM64X追加後にも既存x64の53/53、x86の16/16が成功した。
ARM64Xのdata exportはARM64ホストから`arm64x-native`、x64/ARM64ECホストから`arm64x-ec`を返し、
3種類のホストが同一パス・同一hashのDLLを使うことを確認した。全試験でskipはなかった。
検証用コピーでは、古いソース、変更したcache、変更したOBJ、異なるEC側CRT設定をそれぞれ拒否した。
変更なしの増分ビルドではnative DLLとARM64X DLLの更新時刻が変わらなかった。

参考値として、4 threads・固定の短い読み「かいとう」・3候補で測定した。
テスト実行後の暖まったファイルキャッシュで各10要求を送り、stdioの往復時間を含む。

| 測定 | 結果 |
|---|---|
| 新規process起動からモデル準備完了 | 約70 ms |
| 変換 | 平均30.2 ms、最大53.0 ms |
| 3候補の採点 | 平均32.4 ms、最大32.8 ms |

この測定は実際の打鍵から画面表示までの時間ではない。
既定100 msの同期採点予算は暖機後の短文では満たすが、cold startや長い文・多数候補・省電力時は追加測定が必要である。
期限超過時の非同期fallbackを維持し、この1回の測定だけで既定値を変更しない。

ローカルの検証ログは`out/reports/`に置く。通常の配布用llama.cpp pinは変更していない。
Surfaceの最終JUnit結果は`out/reports/surface-arm64-zenz-02/reports/interactive-full-02/results.xml`、
参考測定は`out/reports/surface-arm64-timing.json`、全体の要約は`out/reports/arm64-validation.json`。
ARM64XのJUnit結果は`out/reports/surface-arm64x-01/reports/interactive-full-01/results.xml`、
ホスト別のロード情報は`out/reports/arm64x-architecture-views.json`、入力検査の結果は`out/reports/arm64x-input-guards.json`。
登録経由の実入力と導入・配布は今後の段階とする。

## 参照

- [llama.cpp v0.6.0](https://github.com/ggml-org/llama.cpp/releases/tag/v0.6.0)
- [MSVC ARMのビルド条件](https://github.com/ggml-org/llama.cpp/blob/v0.6.0/ggml/src/ggml-cpu/CMakeLists.txt)
- [Arm64X](https://learn.microsoft.com/en-us/windows/arm/arm64x-pe)
- [Arm64X DLLのビルド](https://learn.microsoft.com/en-us/windows/arm/arm64x-build)
- [OpenSSH Server](https://learn.microsoft.com/en-us/windows-server/administration/openssh/openssh_install_firstuse)
- [Windows OpenSSHの鍵認証](https://learn.microsoft.com/en-us/windows-server/administration/openssh/openssh_keymanagement)
