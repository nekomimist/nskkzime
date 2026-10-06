# zenz CPUの事前検査と対応範囲

配布構成はWindowsネイティブx64のzenz CPU版に固定する。
設定CLIはllama.cppへリンクせず、モデル・設定ファイル・辞書を開かずにCPUとOSの要件を検査できる。

```powershell
.\out\build\windows-x64-zenz\Release\nskkzime_config.exe --check-cpu
# 展開済みZIPでは .\bin\nskkzime_config.exe --check-cpu
```

標準出力は1行のJSON。`format=nskkzime-cpu-check`、`version=1`、
`contract=zenz-msvc-avx2-v1`、`supported`、各機能の有無、`missing`を返す。
終了コードは要件を満たす場合0、不足・検出不能の場合3、引数の誤りは2。
CPU要件の確認にはモデルを読み込むサーバーを起動する必要はない。

## 固定する要件

| 判定 | 必要な条件 |
|---|---|
| 実行環境 | `IsWow64Process2` でホストも実行プロセスもネイティブAMD64と確認できること |
| SIMD | SSE3、SSSE3、SSE4.1、SSE4.2、AVX、AVX2、FMA、F16C |
| 整数命令 | POPCNT、BMI1、BMI2、LZCNT |
| OSの状態管理 | XSAVE、OSXSAVE、およびXCR0のXMM・YMM状態が有効 |

これはMSVCの `/arch:AVX2` と固定したggmlバックエンドに合わせた保守的な製品条件であり、
全バイナリーの命令列から最小要件を証明したものではない。MSVCの `/arch` はベクトル命令だけでなく
関連する整数命令にも影響する。[Microsoftの `/arch` 説明](https://learn.microsoft.com/en-us/cpp/build/reference/arch-x64?view=msvc-170)を参照。

CPU機能はCPUIDの有効なleafを確認してから調べる。XGETBVはXSAVE/OSXSAVEの確認後だけ実行する。
AVXの実行可否にはCPUだけでなくOS側の状態管理も必要である。
[MicrosoftのCPUID説明](https://learn.microsoft.com/en-us/cpp/intrinsics/cpuid-cpuidex?view=msvc-170)、
[Intelの命令・機能検出仕様](https://www.intel.com/content/dam/develop/external/us/en/documents/319433-024-697869.pdf)を参照。

ARM64上のx64エミュレーションを対応済みとは判定しない。`GetNativeSystemInfo` はエミュレーション環境で
x64相当の情報を返すため、`IsWow64Process2` のホストアーキテクチャーを使う。取得できない場合も不可とする。
[MicrosoftのGetNativeSystemInfo説明](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getnativesysteminfo)、
[IsWow64Process2説明](https://learn.microsoft.com/en-us/windows/win32/api/wow64apiset/nf-wow64apiset-iswow64process2)を参照。

`cmake/zenz.cmake` はAVX2/BMI2を固定し、NATIVE、AVX-VNNI、AVX512、AMX、動的バックエンド、
CPU全variantの設定をオフに戻す。過去のCMakeキャッシュから、事前検査より強い要件を持ち込まない。
事前検査自体は基準のx64命令セットでビルドし、AVX系のコンパイル指定が混入するとコンパイルを拒否する。
手動で変更したコンパイラーオプション・別のllama.cpp・別のサーバーについて、この条件での動作は保証しない。

## 起動・設定への反映

- IMEの推論クライアント作成前と、作業スレッドでのサーバープロセス起動前に検査する。
  不足時は推論を開始せず、かな・辞書入力を継続する。検証アプリの診断にはCPU/OS要件不足を表示する。
- 設定GUIの検査、およびCLIの `--set-zenz` / `--check` でも、推論を有効にする設定なら検査する。
  要件不足時は保存せず、不足した項目を表示する。推論を無効にした設定は保持・編集できる。
- サーバーもモデル初期化前に同じ検査を行う。ただしサーバーにはAVX2の静的ライブラリーを含むため、
  その実行開始前の安全性をサーバー自身の `main` 内の検査だけには依存しない。
  直接CLI利用や導入前の判定には、先に基準命令セットの `nskkzime_config.exe --check-cpu` を使う。
- 設定の検査ではモデルの互換性や実際の推論性能までは分からない。要件を満たす環境でも、モデルの存在・
  ハッシュ・生成・採点・期限と同時利用を別途確認する。

## 2026-10-07の確認

Windows 11 x64（10.0.26300）、AMD Ryzen 9 7950X3D（Family 25 / Model 97 / Stepping 2）、
MSVC 19.44のReleaseで、上の全項目が有効、`supported=true`、`missing=[]`を確認した。
この端末での実モデル評価は [品質・遅延評価](EVALUATION.md) に記載している。
この変更後も標準zenz Releaseの全51テストが成功し、CLIとDLL経由の実モデル連携を確認した。

`cpu_requirements` は、AVXだけのCPU、FMA/F16C/BMI/LZCNT不足、OSXSAVE不足、XMM/YMMの片方だけの有効化、
CPUID情報なし、エミュレーション相当の入力を使って拒否と不足項目の表示を検査する。
これらは判定関数への模擬入力であり、未対応CPUの実機試験ではない。
`config_cli` は不正な保存設定パスがあってもCPUだけを検査できることと、JSON・終了コードの整合を確認する。
AVX512/NATIVE等を明示して再configureした場合も、CMakeキャッシュと生成対象が固定構成に戻ることを確認した。

Intel製CPU、古いAMD製CPU、低速CPU、仮想マシン、Windows 10での実測と、起動拒否の未対応実機確認は残る。
この事前検査の実装だけで、計画のCPU対応範囲の実測完了とはしない。
