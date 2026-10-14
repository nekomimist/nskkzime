# nskkzime

nskkzime（**nekomimist's skk zenz ime**）は、DDSKK風の操作で入力し、[zenz](https://huggingface.co/Miwa-Keita/zenz-v3.2-small-gguf)
による候補の採点と生成を組み合わせるWindows用の日本語IME。zenzの扱いは同じ作者の
[skk-zenz](https://github.com/nekomimist/skk-zenz)（Emacs用）を移植している。個人用の開発版。

- SKK辞書（UTF-8／EUC-JP、複数可）による変換、送り仮名、abbrev、単語登録、学習と削除
- 辞書にない読み・長い読みのzenzによる生成、辞書候補の文脈による採点
- x64アプリと32bitアプリ（x86 DLL）、Windows検索・Microsoft Store（AppContainer）に対応
- 推論は別プロセスで行い、止まっても辞書入力は続けられる

## ビルド

PowerShell 7、Visual Studio 2022 Build Tools（C++、Windows SDK、C++ CMake tools）、`hf` CLIが必要。

```powershell
.\scripts\prepare-zenz.ps1
.\scripts\build.ps1 -Configuration Release
```

出力は `out/build/windows-x64-zenz/Release/`。詳しくは [DEVELOPMENT.md](docs/DEVELOPMENT.md)。

## 導入

Scoopから導入する場合は、使用する本人の通常権限のPowerShellで実行する。

```powershell
scoop bucket add extras
scoop bucket add nekomimist https://github.com/nekomimist/scoop-bucket
scoop install nekomimist/nskkzime
```

PowerShell 7とVisual C++ランタイム（x64・x86）も導入する。IME登録時のUACは同じ本人の
アカウントで承認する。IME本体は `%ProgramFiles%\nskkzime` に配置し、Scoopのフォルダーにも
取得したパッケージを保持する。初版は一人の管理ユーザーが管理し、Scoopのglobal installには対応しない。

更新時は使用中のアプリを閉じて `scoop update`、`scoop update nskkzime` を実行する。
旧版の本体は復旧用に残す。`scoop uninstall nskkzime` はIME登録とbrokerの起動登録を解除し、
本体・設定・学習辞書は保持する。本体も削除する場合は [完全削除](docs/PACKAGE_README.md#完全削除) を参照。

ZIPで導入する場合は [GitHub Releases](https://github.com/nekomimist/nskkzime/releases) から取得し、
展開先の `README.md` に従う。ビルドしたReleaseを導入する場合は、管理者のPowerShell 7
（スタートメニューから起動したもの）で実行する。

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
本体は [MIT License](LICENSE)。
