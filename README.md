# nskkzime

nskkzime（**nekomimist's skk zenz ime**）は、DDSKK風の操作で入力するWindows用の日本語IME。
[zenz](https://huggingface.co/Miwa-Keita/zenz-v3.2-small-gguf)による候補の採点と生成を組み合わせる。
zenzの処理は、同じ作者の[skk-zenz](https://github.com/nekomimist/skk-zenz)（Emacs用）から移植している。個人用の開発版。

- SKK辞書（UTF-8／EUC-JP、複数可）による変換、送り仮名、abbrev、単語登録、学習と削除
- zenzによる候補の生成（辞書にない読み・長い読み）と、文脈に応じた辞書候補の採点
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

Scoopから導入する場合は、使用する本人が非パッケージ版のPowerShellを
Explorer／スタートメニューから直接起動し、次のコマンドを実行する。通常権限でよい。
アプリの内蔵端末など、データが仮想化される実行環境では、起動登録が別の場所へ保存されるのを防ぐために導入処理を停止する。

```powershell
scoop bucket add extras
scoop bucket add nekomimist https://github.com/nekomimist/scoop-bucket
scoop install nekomimist/nskkzime
```

Visual C++ランタイム（x64・x86）も導入する。IME登録は専用EXEが行い、PowerShell 7の追加導入は不要。
登録時のUACは同じ本人のアカウントで承認する。配布物は無署名で、EDRでの許可は保証しない。

IME本体は `%ProgramFiles%\nskkzime` に配置し、Scoopのフォルダーにも
取得したパッケージを保持する。初版は一人の管理ユーザーが管理し、Scoopのglobal installには対応しない。

更新時は使用中のアプリを閉じて `scoop update`、`scoop update nskkzime` を実行する。
旧版の本体は復旧用に残す。

`scoop uninstall nskkzime` はIME登録とbrokerの起動登録を解除し、
本体・設定・学習辞書は保持する。本体も削除する場合は [完全削除](docs/PACKAGE_README.md#完全削除) を参照。

ZIPで導入する場合は [GitHub Releases](https://github.com/nekomimist/nskkzime/releases) から取得し、
展開先の `README.md` に従う。

ビルドしたReleaseを導入する場合は、スタートメニューからPowerShell 7を管理者として起動し、
次のコマンドを実行する。

```powershell
.\scripts\register-dev.ps1
```

導入後、Windowsの入力方式からnskkzimeを選び、C-jでかな入力にする。
入力モード「あ／ア／A／Ａ」を右クリックし、メニューの「設定…」でSKK辞書とzenzのモデルを指定する。
設定の変更は、利用するアプリを起動し直すと反映される。

## 文書

- [USAGE.md](docs/USAGE.md): 操作、設定、辞書、zenz、DDSKKとの違い
- [ARCHITECTURE.md](docs/ARCHITECTURE.md): プロセス構成、辞書と学習、推論、AppContainer、保存場所
- [DEVELOPMENT.md](docs/DEVELOPMENT.md): ビルド、テスト、導入スクリプト、評価
- [TODO.md](docs/TODO.md): 残りの課題

## ライセンス

同梱の依存ライブラリ・データのライセンスは [third_party](third_party/README.md) を参照。
本体は [MIT License](LICENSE)。
