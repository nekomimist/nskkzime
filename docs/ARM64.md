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
段階5はARM64X DLLとx86 companionを一時登録し、4種類の検証ホストで推論付きの実 HWND 入力まで確認した。
native ARM64のEdgeでも専用プロファイルを使った手入力を確認した。
他の一般アプリと段階6の導入・配布は未確認である。

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
転送用manifestで全75個のEXE/DLLと固定モデルを照合した後、nativeの50試験とホスト別の16試験を直列実行する。

この試験は登録済みの一般アプリへの入力を保証しない。
登録・解除・復帰とEDIT/RichEditの確認は、次の一時登録試験で行う。

## 一時登録と実 HWND への入力試験

実験用bundleには`run-arm64x-registration.ps1`と`run-arm64x-input-probes.ps1`を含める。
既存のnskkzime COM/TSF登録がある端末では登録を拒否し、置換や既存版へのrollbackは扱わない。
本番のpackage形式・installer・brokerのサインイン起動とは分けた、登録と解除だけの検証である。

```powershell
# SurfaceのARM64 PowerShell 7。Planは登録を変更しない。
.\run-arm64x-registration.ps1 -Action Plan -Name surface-01

# 同じユーザーの管理者PowerShellで実行する。
.\run-arm64x-registration.ps1 -Action Register -Name surface-01

# RDPの非昇格PowerShellで実行する。検証ウィンドウが順番に表示される。
.\run-arm64x-input-probes.ps1 -Name surface-01

# 成否にかかわらず、同じユーザーの管理者PowerShellで解除する。
.\run-arm64x-registration.ps1 -Action Unregister -Name surface-01
# 中断・解除失敗の場合も同じ名前を指定する。
.\run-arm64x-registration.ps1 -Action Recover -Name surface-01
```

公開バイナリー10個だけを`Program Files/nskkzime-arm64-registration-tests/<name>/`へコピーする。
コピー前後のhashを照合し、配置先とjournalはSYSTEM/Administratorsだけが変更できるACLにする。
設定・辞書・レポートはユーザーのbundle内に隔離する。検証DLLは解除完了まで保持する。
同じ名前の配置先やレポートがある場合は上書きせず、新しい名前を使う。

ARM64Xを一度登録し、ARM64/x64/ARM64ECのfresh processで`CoCreateInstance`した後に、
実際のDLLパスとdata exportのビルド情報を照合する。x86は別途登録・検証する。
`ime_probe_local`のmanifest経由ロードや、先にDLLを直接ロードする方式は登録確認に使わない。
通常版`ime_probe --self-test`は、RichEditでは本物のHWND/documentへ`ITfKeystrokeMgr`で入力し、
EDITでは前面・focus・物理修飾キーを確認して`SendInput`を送る。
RDPを接続したまま、ロック解除済みのデスクトップを表示して実行する。
前面化がWindowsに拒否された場合は、RDPで検証ウィンドウをクリックして開始する。
ARM64/x64/ARM64EC/x86のウィンドウは順番に起動するため、それぞれクリックが必要になる場合がある。
自動試験中は別のウィンドウへ切り替えず、終了まで待つ。
プロファイルの切り替え前後にRichEditのfocusを確認し、TSF document/contextが成立してから辞書を待つ。
辞書の診断exportはmessage pumpで遅れてロードされる場合に備えて再取得する。
前面待ちを含むprobe全体の期限は300秒、context・辞書・期待文字列の待機は各5秒以内とし、親は330秒で打ち切る。
各controlの開始前に前面とfocusを再確認し、入力途中で失った場合は試行を中断して元のIMEへ戻す。
レポートは`input/<RunName>/`へ残し、同じ登録で再実行するときも以前の試行を上書きしない。
各ホストで変換・確定・取消・送り仮名と、プロセス内で切り替えた元のIMEへの復帰を確認する。
両方で物理キーボードの全経路を検証したという意味ではない。

`-WithZenz`では、`かいとう`を含まない専用辞書と固定モデルを使う。
RichEditとEDITそれぞれで、同じ読みから「試験問題の解答」「冷凍食品を電子レンジで解凍」を生成し、
本文、生成を反映した診断status、確定後の追加入力を照合する。4ホストで計16件の生成を確認する。
接頭辞の挿入後は、明示的な末尾位置へcaretを動かし、`EM_GETSEL`でも確認する。
`EM_SETSEL(-1, -1)`は選択解除なので、末尾への移動として使わない。

```powershell
.\run-arm64x-input-probes.ps1 -Name surface-01 -WithZenz
```

サーバーとモデルのhashを確認し、native ARM64のサーバーだけを試行別ディレクトリへコピーする。
endpointはINIの場所では分離されないため、実行ごとに異なるサーバーパスを指定する。
学習・本文の入力ログ・候補採点はoff、生成の期限は2000 ms、サーバーのidle期限は10秒とする。
試行内にTSF metadataの診断ディレクトリを用意し、失敗時は既知のテスト本文と推論・文脈の状態も残す。
`server.json`には観測したPID・実行パス・hashとidle終了を記録する。未終了ならファイルを保持して試行を失敗扱いにする。

解除はx86→nativeの順で行い、x86だけの解除後も共有TSFプロファイルと64bit COM生成が保たれることを確認する。
全解除後はfresh processのCOM生成が`REGDB_E_CLASSNOTREG`になること、対象TSF processor/category/profileが消えたことを調べる。
機械とユーザー、32/64bit viewのTIP状態を別々に記録し、入力の既定値と普段のnskkzimeファイルのhashも前後で照合する。
ユーザー側にTIP設定だけが残る場合は、初期状態が不在で、所有者を確認できた時点のsnapshotと完全一致する対象だけを戻す。
他のIMEを含むCTF hive全体の復元は行わない。

各変更のintentを先にjournalへ永続化する。Recoverでは実際の両COM所有者とバイナリーhashを再確認し、
他の登録先や実行中のregistrarがある場合は停止する。診断失敗があっても所有する登録の解除を続け、結果を分けて記録する。
模擬的な部分失敗・中断・所有者変更・解除再試行は`tests/arm64_registration_tests.ps1`で検証できる。

## 専用Edgeでの手入力試験

一時登録中に、RDPの非昇格PowerShell 7から`run-arm64x-browser-input.ps1`を実行する。
開始前に普段のIME名を確認し、`OriginalImeName`へ指定する。

```powershell
.\run-arm64x-browser-input.ps1 -Action Start -Name surface-01 `
  -RunName browser-01 -OriginalImeName 'Microsoft IME'
.\run-arm64x-browser-input.ps1 -Action Status -Name surface-01 -RunName browser-01
# 別の同じユーザーのPowerShellで継続観測する場合。
.\run-arm64x-browser-input.ps1 -Action Watch -Name surface-01 -RunName browser-01
```

native ARM64のEdgeを新しい`--user-data-dir`で起動し、プロセスの環境へ専用設定を渡す。
`UserDataDir`や`DownloadDirectory`のEdge管理ポリシーがある場合は開始を拒否する。
サーバー・辞書・ダウンロード先も試行別に分離し、学習と入力ログは無効にする。
最初に専用辞書の候補`NSKKZIME-ARM64-ISOLATED`を手入力で確認してから、
textareaの2つの文脈とcontenteditableの入力へ進む。
RDP接続元は直接入力にし、Surface側でnskkzimeを選ぶ。

各欄の本文とcompositionイベント、ペーストの有無をページで確認する。
Surface側を元のIMEへ戻し、復帰チェックを入れて保存する。
未完了の項目も`passed: false`を含むJSONとして保存し、ページ内にも同じJSONを表示する。
保存後は`browser/<RunName>/browser-result.json`の存在と内容を確認する。
ページの合否は手入力の結果であり、DLL・サーバーの観測や復旧確認と合わせて評価する。

```powershell
# 元のIMEへの復帰と結果ファイルを確認してから実行する。
.\run-arm64x-browser-input.ps1 -Action Stop -Name surface-01 -RunName browser-01
.\run-arm64x-registration.ps1 -Action Recover -Name surface-01
```

Status/Watchは起動したEdgeのPID・開始時刻・session・パス・専用プロファイルを照合し、
その子プロセスと、実際にロードしたTSF DLLのパス、専用サーバーをsnapshotへ記録する。
Stopは所有を確認したEdgeを閉じ、観測した子プロセスの終了とサーバーのidle終了を確認する。
識別情報が変わった場合やプロセスが残った場合は失敗として記録を保持する。
ブラウザーを終了してから登録を解除し、普段の設定・辞書と入力の既定値が変わっていないことを確認する。

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
| Surface / 一時登録したARM64・x64・ARM64EC・x86 hosts | 4/4成功（8入力欄） | RichEditとEDITで変換・確定・取消・送り仮名、各processの元IMEへの復帰。RichEditはTSF直接配送、EDITはSendInput |
| 既存x64 / azooKey fork | 54/54成功 | 共通登録検証コードの変更後。実モデル、TSF、AppContainer、配布パッケージ、installation模擬試験 |
| 既存x86 companion | 16/16成功 | TSF、CPU検査、x64 helperとの連携 |
| 依存移植時のx64 / upstream + zenz patch | 53/53成功 | 既存forkとのtokenizer比較と実モデルの回帰 |

ARM64のpackage/installation/Scoopは未対応のため除外した。
初回のSSH session 0では画面試験4件が失敗したが、同じバイナリーをRDPで実行すると4/4成功した。
従来のx64専用CPU判定を分離した後、同期採点のcold start経路も修正して全50件が成功した。
この未登録試験ではIME登録と普段の設定・辞書を変更していない。一時的なオンデマンドタスクは削除済み。
比較構成を同時にテストした初回にはTSF/AppContainerの失敗が出た。直列では再現していないが、原因は未確定のため構成間のCTestは直列で実行する。

ARM64X追加後にも既存x64の53/53、x86の16/16が成功した。
ARM64Xのdata exportはARM64ホストから`arm64x-native`、x64/ARM64ECホストから`arm64x-ec`を返し、
3種類のホストが同一パス・同一hashのDLLを使うことを確認した。全試験でskipはなかった。
検証用コピーでは、古いソース、変更したcache、変更したOBJ、異なるEC側CRT設定をそれぞれ拒否した。
変更なしの増分ビルドではnative DLLとARM64X DLLの更新時刻が変わらなかった。

一時登録の実機試験では、ARM64/x64/ARM64EC/x86の4種類で登録経由のCOM生成が成功した。
x86だけの解除後にも3種類の64bitホストから生成でき、最後の全解除後は4種類とも
fresh processで`REGDB_E_CLASSNOTREG`となった。Recoverのjournalは`complete`で、
対象のCOM/TSF processor/category/profileは残っておらず、入力の既定値と普段のファイルのhashも一致した。
このOSでは`IsEnabledLanguageProfile`が未登録でもtrueを返したため、その値だけでは登録の有無を判断しない。
一時タスクはすべて削除済み。登録試験直前の66/66、共通変更後の既存x64 54/54（installation模擬試験を含む）とx86 16/16も成功した。

登録された実 HWND への入力試験では、RDPのクリックによる前面化を待ち、
ARM64/x64/ARM64EC/x86すべてでRichEditとEDITへの変換・確定・取消・送り仮名が成功した。
各processで元のIMEへの復帰も確認した。推論・学習を無効にした専用設定と8見出しの固定辞書を使用した。
初回は前面待ちで中断し、その後も辞書待機とfocusの問題が出たため、
診断exportの再取得、RichEditのTSF context待機、両経路のfocus確認を検証コードへ追加した。
最終試行は4/4成功したが、x64の初回timeoutの原因を単独の変更に特定したわけではない。
試行ごとのログを保存し、各回で登録を解除した。最終Recoverは`complete`で、COM/TIPは不在、
入力の既定値と普段のファイルのhashは検証前と一致し、一時タスクも0件だった。

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
一時登録・解除の実機ログは`out/reports/surface-arm64x-registration-03/`、
要約は`out/reports/arm64-registration-validation.json`。
再試行用の最終版でも4種類の未登録ロード確認と、一時登録・fresh COM生成・Recoverの一巡を再確認した。
その実機ログは`out/reports/surface-arm64x-registration-04/`に残し、登録と一時タスクは解除済みである。
登録経由の入力ログは`out/reports/surface-arm64x-input-07/`、
4ホストの要約はその下の`registration-surface-input-07/input/interactive-01/results.json`に残した。
この時点では一般アプリと推論を有効にした実入力は未確認だった。次の検証で追加した。

## 2026-10-17の検証結果

MSVCのARM64・x64・ARM64EC・x86のprobeを再ビルドし、一時登録した実機で推論付き入力を確認した。
`かいとう`を含まない辞書を使い、RichEditとEDITの両方で「解答」「解凍」の文脈による生成、
確定後の「です」の追加入力が4ホスト計16件すべて成功した。
従来の変換・確定・取消・送り仮名と、各processの元IMEへの復帰も成功した。
生成を反映した診断status、native ARM64サーバーの実行パス・hash、固定モデルのhashを照合した。
サーバーはidle終了し、Recover後はCOM/TIP不在、通常状態のhash一致、一時タスク0件を確認した。

EDITの初回試行では接頭辞の後へcaretが移動せず、`解答試験問題の`となった。
検証コードの`EM_SETSEL(-1, -1)`を明示的な末尾位置へ変更し、`EM_GETSEL`でも位置を確認した。
別の再試行は前面を失ったため中断した。各試行の記録と復旧結果を保持し、最終試行で全件成功した。

native ARM64のEdge 155.0.4283.45では、専用候補・2つの文脈での生成・contenteditableの
計4項目をユーザーが手入力し、ページの全項目PASSとCorvusSKKへの復帰を確認した。
保護された登録先のDLLをEdgeがロードしたsnapshotが51件、専用nativeサーバーの観測が7件ある。
本試験のJSONは保存されなかったため、この4項目はユーザーの確認と実行観測による結果として記録し、
構造化された各欄のイベント数を取得済みとは扱わない。
保存処理を未完了の結果も出力するよう修正し、別試行で未入力の4項目が`passed: false`として
JSONに保存されることを確認した。この保存確認は入力成功の件数に含めない。

専用Edgeと観測した子プロセスは終了し、サーバーもidle終了した。
最後のRecoverは`complete`でvalidation errorなし、COM/TIP不在、通常状態のhash一致、一時タスク0件だった。
ブラウザー観測時のJSON日時比較もUTC ticksで照合するよう修正し、修正後の観測とStopが成功した。

推論付き入力の実機ログは`out/reports/surface-arm64x-zenz-input-11/`、
要約は`out/reports/arm64-zenz-input-validation.json`。
Edgeのログは`out/reports/surface-arm64x-edge-input-12/`、要約は`out/reports/arm64-browser-input-validation.json`。
この回は4 ABIのビルドと上記の登録入力・Edgeを検証した。前日のCTest 66/66を同じソースで再実行したという意味ではない。
実機バイナリーのソースhashと、後から追加したブラウザー用スクリプト・ページのhashは別に記録した。

残る段階は他の一般アプリ、実機での継続利用・省電力・スリープ復帰、導入・更新・解除と配布、ARM CIである。

## 参照

- [llama.cpp v0.6.0](https://github.com/ggml-org/llama.cpp/releases/tag/v0.6.0)
- [MSVC ARMのビルド条件](https://github.com/ggml-org/llama.cpp/blob/v0.6.0/ggml/src/ggml-cpu/CMakeLists.txt)
- [Arm64X](https://learn.microsoft.com/en-us/windows/arm/arm64x-pe)
- [Arm64X DLLのビルド](https://learn.microsoft.com/en-us/windows/arm/arm64x-build)
- [EM_SETSEL](https://learn.microsoft.com/en-us/windows/win32/controls/em-setsel)
- [Edge UserDataDirポリシー](https://learn.microsoft.com/en-us/deployedge/microsoft-edge-policies/userdatadir)
- [OpenSSH Server](https://learn.microsoft.com/en-us/windows-server/administration/openssh/openssh_install_firstuse)
- [Windows OpenSSHの鍵認証](https://learn.microsoft.com/en-us/windows-server/administration/openssh/openssh_keymanagement)
