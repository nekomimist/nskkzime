# 開発用パッケージと配布の残課題

`scripts/package.ps1` でzenz CPUを含む一種類のZIPを作る。zenzなし版の配布・維持は行わない。
実行ファイルと依存ライセンスを固定リストから梱包し、マニフェストとSHA-256で不足・破損を確認する。
ファイルの作成だけを行い、Windowsへの登録、インストーラーの起動、設定・辞書の変更は行わない。
現時点では開発用であり、公開リリースではない。

## 作成と検査

PowerShell 7、Build Tools 2022、Gitが必要。先にReleaseをビルド・テストする。
同じ標準ビルドディレクトリーの実モデルサーバーを必ず同梱する。
モデルの準備は [ZENZ.md](ZENZ.md)、ZIP内の利用者向け手順は [PACKAGE_README.md](PACKAGE_README.md)。

```powershell
.\scripts\build.ps1 -Configuration Release
.\scripts\package.ps1 -Output out/packages/nskkzime.zip
.\scripts\package.ps1 -Verify out/packages/nskkzime.zip
```

`-SourceDirectory` でビルド先を指定できるが、末尾が `Release` で、親の `CMakeCache.txt` がx64と
zenz有効（`NSKKZIME_WITH_ZENZ=ON`）である必要がある。軽量テスト専用ビルドは梱包を拒否する。
出力先が既にある場合は拒否し、上書きしない。
新規ZIPは隣接する一時ファイルへ書き込み、検査成功後に最終名へ変更する。旧ZIPや登録先を変更しない。
同じ内容・同じ記録条件なら、エントリー順とZIP時刻を固定して同じバイト列を生成する。

| 同梱物 | nskkzime（zenz CPU） |
|---|---|
| TSF DLL、辞書補助プロセス、設定GUI、辞書管理GUI | あり |
| 設定CLI、辞書管理CLI、登録済みIME用の入力検証アプリ | あり |
| `nkkz-ime-server.exe` | 実モデル対応版 |
| nlohmann/json MIT、Unicode License v3 | あり |
| skk-zenz MIT、azooKey llama.cpp MIT | あり |
| 利用手順、ZIP検査スクリプト、マニフェスト | あり |
| モデル、SKK辞書、ユーザー設定、学習データ | なし |

`manifest.json` を除くファイル数は14。外部データや個別評価結果を誤って梱包しない。
登録用DLLとその補助プロセス・GUIは全て `bin/` に置く。
従来の `-Zenz` オプションは互換用に受け付けるが、指定の有無で内容は変わらない。
過去のzenzなしZIPは検査だけ可能にしている。新規作成・更新・別版としてのリリース検証は行わない。

## ビルドと依存の検査

全バイナリーのPEヘッダーがAMD64 / PE32+であることを確認し、`dumpbin /dependents` で静的な
インポート一覧を取得する。既知のWindows DLLとRelease用Visual C++ランタイム以外の依存は拒否する。
Debugランタイム、梱包していない独自DLL、別アーキテクチャーの混入を検出する。
PEヘッダーの判定は [MicrosoftのPE仕様](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format) に基づく。
これは実行コードの全命令や動的に読み込むDLLまで検証する仕組みではない。

現在のMSVC 19.44ビルドは `MSVCP140.dll`、`MSVCP140_ATOMIC_WAIT.dll`、`VCRUNTIME140.dll`、
`VCRUNTIME140_1.dll` とWindowsのUCRTを使う。Visual C++ v14 Redistributable x64は別途必要。
ビルドに使ったツールセット以上のランタイムが必要なことは
[Microsoftのランタイム配布案内](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist/)を参照。
ランタイムのダウンロード・導入はパッケージ作成に含めない。

zenz CPUは現在のCMake構成で `/arch:AVX2`、FMA、F16C、BMI2を使う。OSのAVX対応も必要。
パッケージ作成時のPE確認だけでCPU互換性を判定しない。対応しないCPUやARM64・x86アプリは未検証。

マニフェストには梱包時のソースrevision、追跡対象ファイルの変更有無、各ファイルのハッシュ・長さ・
依存DLL・Authenticode検査結果を残す。未追跡ファイルの一覧やユーザーデータのパスは記録しない。
**梱包時のrevisionとバイナリーのビルド元は同一とは限らない**ので、`binary_build_revision` は未確定として残す。
公開用にはクリーンなcheckoutからのビルドと、その由来を保証する手順が必要。

`-Verify` は展開も実行もせず、既定のファイル集合・重複・パス・サイズ上限・ハッシュを検査する。
Build ToolsやGitを使わず、ZIPに含めた同じスクリプトでも実行できる。展開済みファイルの検査ではない。
署名を作成する処理はなく、自己記載したハッシュは提供者の認証にはならない。

## 確認済み範囲（2026-10-06）

- 標準のzenz入りReleaseで実ファイルの梱包・ZIP検査に成功。同じ入力から2度作成したZIPのSHA-256も一致。
- CTestの `package` で、既存ZIPの保全、バイナリー破損、ライセンス不足、マニフェストの重複、
  ZIP内の重複・大小文字だけ異なる重複・上位パス・余分な設定ファイル、推論サーバー不足、x86ヘッダー、未知の依存DLLを拒否。
  Debugとzenzなしのテスト専用Releaseでは梱包を拒否し、出力を作らないことを確認。
- 日本語と空白を含む別フォルダーへzenz ZIPを展開し、同梱検査スクリプトと設定／辞書管理CLIのhelpを確認。
  展開先の実モデルサーバーで `こうこうのきょうし` → `高校の教師` の候補生成を確認。
- これらの確認ではOS登録とユーザーの設定・学習データは変更していない。

端末にはBuild Toolsとランタイムがあるため、展開先での起動成功をクリーン環境への導入成功とは扱わない。
ZIPによる登録・更新・解除・旧版への復帰は手順を用意した段階。これまでの開発用登録スクリプトでの
実測範囲は [TSFの検証](TSF_VALIDATION.md) に記載する。

登録処理は、別のパスにあるDLLからの解除を拒否し、カテゴリーの解除失敗を呼び出し元へ返すようにした。
途中の登録／解除と後始末の失敗は専用レジストリで自動検証する。失敗時に残した登録先から解除を
再試行できるが、インストーラーによる自動更新・旧版復元や、クリーンOSでの実測はまだ含まない。

## 公開・日常利用前に残る項目

- クリーンなWindows x64環境での前提条件、導入・更新・解除・復帰と設定保全の確認。
- 本体の公開ライセンスの確定、署名の証明書と署名・検証・配布経路の整備。
- インストーラー、登録失敗時の復旧、複数ユーザーや管理者資格情報が異なる場合の扱い。
- ブラウザ・VS Code・Windows Terminal、DPIと複数ディスプレイ、数日間の通常利用。
- 低速CPUでの推論期限と同時利用、対応CPU範囲の実測。モデル同梱を行う場合のモデルライセンス表示。

この段階では既存の登録先・旧版ファイル・設定・辞書を自動更新しない。
ユーザー辞書とモデルは、梱包処理の入力にも含めない。
