# nskkzime

nskkzime（**nekomimist's skk zenz ime**）は、DDSKK風の操作で入力し、[zenz](https://huggingface.co/Miwa-Keita/zenz-v3.2-small-gguf)
による候補の採点と生成を組み合わせるWindows用の日本語IME。zenzの扱いは同じ作者の
[skk-zenz](https://github.com/nekomimist/skk-zenz)（Emacs用）を移植している。個人用の開発版。

- SKK辞書（UTF-8／EUC-JP、複数可）による変換、送り仮名、abbrev、単語登録、学習と削除
- 辞書にない読み・長い読みのzenzによる生成、辞書候補の文脈による採点
- x64アプリと32bitアプリ（x86 DLL）、Windows検索・Microsoft Store（AppContainer）に対応
- 推論は別プロセスで行い、止まっても辞書入力は続けられる

## ビルド

PowerShell 7、Visual Studio 2022 Build Tools（C++、Windows SDK、C++ CMake tools）、Python 3.11以上、`hf` CLIが必要。

```powershell
.\scripts\prepare-zenz.ps1
.\scripts\build.ps1 -Configuration Release
```

出力は `out/build/windows-x64-zenz/Release/`。詳しくは [DEVELOPMENT.md](docs/DEVELOPMENT.md)。

## 導入

管理者のPowerShell 7（スタートメニューから起動したもの）で、ビルドしたReleaseを梱包して導入する。

```powershell
.\scripts\register-dev.ps1
```

導入後、Windowsの入力方式からnskkzimeを選び、C-jでかな入力にする。
入力モード「あ／ア／A」の右クリックメニューの「設定…」で、SKK辞書とzenzのモデルを指定する。
設定の変更は、利用するアプリを起動し直すと反映される。

## 文書

- [USAGE.md](docs/USAGE.md): 操作、設定、辞書、zenz、DDSKKとの違い
- [ARCHITECTURE.md](docs/ARCHITECTURE.md): プロセス構成、辞書と学習、推論、AppContainer、保存場所
- [DEVELOPMENT.md](docs/DEVELOPMENT.md): ビルド、テスト、導入スクリプト、評価
- [TODO.md](docs/TODO.md): 残りの課題

## ライセンス

同梱の依存ライブラリ・データのライセンスは [third_party](third_party/README.md) を参照。
本体のライセンスは未定。
