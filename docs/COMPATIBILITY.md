# 対応範囲とOS・アプリの制約

2026-10-07時点。配布対象は **Windows x64のデスクトップアプリ向け、zenz CPU入りの一種類**。
以下はソース・成果物・実測の区別を付けた対応表であり、未検証環境で使えるという宣言ではない。
zenzのCPU/OS要件は起動前に検査する。検査項目と、この端末以外の未実測範囲は [CPU対応](CPU_SUPPORT.md) を参照。

| 環境 | 現状 | 根拠・残作業 |
|---|---|---|
| Windows 11 x64、標準EDIT／TSF対応RichEdit | 基本入力、辞書変換、実モデル連携を確認済み | ビルド版の親context対応で標準EDITの矢印・クリック・ドラッグと文脈を確認。標準EDITの読みは補助欄に表示し、ホスト側本文変更後の入力も確認した。OS登録更新は残る。[TSF検証](TSF_VALIDATION.md) を参照。全機能・全入力操作の確認ではない |
| Firefox／VS Code／Windows Terminal | 未検証 | 実アプリでのキー配送、文脈、入力属性、遅延候補を個別に確認する |
| Windows 10 x64 | 未検証 | Windows SDKのターゲット指定だけでは実OSでの動作確認にならない |
| x86アプリ | 現在の配布対象外 | x86用TSF DLLと32ビット側登録を提供していない |
| Windows on Arm／ARM64アプリ | 現在の配布対象外 | ARM64／ARM64EC成果物を提供せず、x64エミュレーション上のIME・推論も未検証 |
| AppContainer／UWPの入力先 | 現在の配布対象外 | 互換性カテゴリー未登録。ファイル・IPC・UIの対応は下記のとおり未実装／未検証 |
| 低整合性のデスクトッププロセス | 未検証 | 同一ユーザーSIDでもIPC接続・サーバー起動・保存の可否は別に確認が必要 |
| 管理者として実行する入力先／別ユーザー | 未検証 | 通常権限の補助プロセスとの接続、プロファイル、保存先を個別に確認する |
| DPI変更・複数ディスプレイ・日本語／英語配列 | 実測未完了 | レイアウト・フォーカスの単体確認だけでは代替できない |

## x86とARM64

TSF DLLは入力先プロセスへ読み込まれる。32ビットプロセスは64ビットDLLを読み込めないため、
現在のAMD64 DLLを置いただけではx86アプリ向けの実装にはならない。
別プロセスのIPCはビット数をまたげるが、TSF DLLの制約は別である。
[Microsoftのプロセス相互運用仕様](https://learn.microsoft.com/en-us/windows/win32/winprog64/process-interoperability)を参照。

Windows on Armにx64エミュレーションがあっても、それだけでネイティブARM64の入力先へこのDLLを
組み込めるとは判断しない。DLLの構成、登録先、zenzのCPU命令と実モデルの性能を実機で確認する必要がある。
[MicrosoftのArmエミュレーション説明](https://learn.microsoft.com/en-us/windows/arm/apps-on-arm-x86-emulation)を参照。

## AppContainer

MicrosoftのIME要件では、IMEは読み込み先アプリのAppContainer制約を受け、互換性の宣言には
`GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT` を使う。nskkzimeの `registration_registry.cpp` は現在、
キーボード、表示属性、入力モード、システムトレイの4カテゴリーだけを登録している。
カテゴリー追加だけで対応済みにはできない。
[MicrosoftのIME要件](https://learn.microsoft.com/en-us/windows/apps/develop/input/input-method-editor-requirements)を参照。

| 対象 | 現在のコード | 対応前に確認する点 |
|---|---|---|
| DLL・辞書・設定 | インストール先は管理者のみ変更可能。辞書・モデルは同梱せず、ユーザーが指定したパスを使う | AppContainerから各ファイルを読めるか。Program Filesへ置くだけで、明示したDACLや外部ファイルまでアクセス可能にはならない |
| 辞書キャッシュ | `dictionary_cache.cpp` はユーザーSID向け読み取りACLの共有メモリーを使う | コンテナーの名前空間・共有可否・ファイルアクセス |
| 学習・推論IPC | `learning_ipc.cpp`／`inference_ipc.cpp` はデスクトップ向けパイプ名とユーザーSID向けACLを使う | 接続の認証、コンテナーの制約、サーバーの起動方法 |
| 候補UI・設定／辞書GUI | デスクトップの候補ウィンドウと補助プロセス起動を使う | ホストのUI制約、タッチキーボード、GUIの起動経路 |

UWP向け名前付きパイプの記述には、同一アプリのAppContainer内に限定した利用と `LOCAL` 名前空間の要件がある。
現在のデスクトップIPCがそのまま使えるという根拠にはしない。
[CreateNamedPipeの注記](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createnamedpipea)を参照。
対応を検討する際も、任意のコンテナーへ辞書や読みを公開するACL変更は行わず、必要な境界と経路を設計して検証する。

## 入力属性と外部アプリの検証

パスワード・private・PIN等の属性や属性不明の入力欄では、文脈取得・推論・学習を抑止する。
これはコア／TSFテストで確認しているが、各アプリが属性をどう提供するかは実測が必要。
外部アプリの検証では、通常入力と保護入力を分け、変換、取消、マウス／矢印移動、貼り付け、
別欄移動、サーバーの遅延／停止、確定後の学習を確認する。
確認結果は [TSF検証の機能別表](TSF_VALIDATION.md) に追記する。
