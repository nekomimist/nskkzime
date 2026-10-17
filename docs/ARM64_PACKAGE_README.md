# nskkzime ARM64 experimental package

Windows 11 ARM64向けの実験版。ARM64X TSF DLL、native ARM64の推論サーバー・broker・設定ツール、
x86 companionを含む。モデル・SKK辞書・設定・学習データは含まない。署名とクリーン環境での導入確認は未実施。

## 前提と検査

- 非パッケージ版のnative ARM64 PowerShell 7を使用する。x64エミュレーションのPowerShellは使用しない。
- Visual C++ v14 RedistributableがARM64・x64/EC・x86それぞれに必要。ビルド時のMSVC以上の版を使用する。
  Microsoftのx64 RedistributableはARM64版も含むため、Windows ARM64ではx64とx86のパッケージで用意できる。
  [Microsoftのダウンロードと要件](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170)
- 推論CPU契約は`zenz-arm64-armv8a-v1`。`bin/nskkzime_config.exe --check-cpu`で確認する。
- 想定モデルはzenz-v3.2-smallの`ggml-model-Q5_K_M.gguf`。
  SHA-256は`29c223d4c23327b80fd13ebb5ab2555057a46317997d5da391584ffbef0db673`。

```powershell
.\tools\package-arm64x.ps1 -Verify 'C:\Downloads\nskkzime-arm64-experimental.zip'
.\tools\package-arm64x.ps1 -VerifyDirectory .
```

実験形式7専用の入口を使う。通常のpackage/install/Scoopはこのパッケージを扱わない。
ファイルhash・PE形式・ARM64X構造・役割別のビルド情報を検査する。
hashと埋め込み情報は整合性検査であり、発行者の署名やロード・入力試験の代わりにはならない。

## 導入・更新・復旧

使用する本人のnative ARM64 PowerShell 7を管理者として起動する。
既存のnskkzimeや限定登録試験を先にその手順で解除する。同じCLSID/TIPを使うため並行導入は扱わない。
管理外のbroker起動登録がある場合も変更を拒否する。

```powershell
.\tools\install-arm64x.ps1 -Action Status
.\tools\install-arm64x.ps1 -Action Plan    # 配置・登録・起動設定を変更しない事前検査
.\tools\install-arm64x.ps1 -Action Install
.\tools\install-arm64x.ps1 -Action Rollback -Version '<Statusの64桁版ID>'
.\tools\install-arm64x.ps1 -Action Recover
.\tools\install-arm64x.ps1 -Action Unregister
```

初回から専用設定のbrokerを使う場合は、上のInstallを次のコマンドに置き換える。
設定ファイルは先に用意し、既存rootへの設定パスの追加・変更は行わない。

```powershell
.\tools\install-arm64x.ps1 -Action Plan -BrokerConfig 'C:\Users\user\ime-test\settings.ini'
.\tools\install-arm64x.ps1 -Action Install -BrokerConfig 'C:\Users\user\ime-test\settings.ini'
```

配置先は`%ProgramFiles%\nskkzime-arm64-experimental\versions\<版ID>`。
実験専用rootの識別情報と所有ユーザーを検査し、通常版との移行は扱わない。
ARM64Xはnative System32、x86はSysWOW64のregsvr32で登録する。
更新前後とRollback/Recoverでも実験版の固定検査を使う。
brokerのサインイン起動も設定する。起動中のアプリは旧DLLを保持するので再起動する。
モデル・辞書の設定は通常権限の設定GUIから行い、`bin/ime_probe-arm64.exe`で入力を確認する。
この実験版も通常の`%LOCALAPPDATA%\nskkzime`を既定に使う。
検証では初回の`-BrokerConfig`で専用brokerの設定を固定し、検証アプリ自身にも同じ`NSKKZIME_CONFIG`を渡す。
`BrokerConfig`だけでは通常起動したIMEクライアントの設定は切り替わらない。
設定パスは保護されたroot markerに保存し、更新・Rollback・Recover・Unregisterでも再利用する。
設定の内容はユーザーが変更できるが、既存rootの設定パスの追加・変更・解除は扱わない。
設定ファイルがなくなってもUnregisterと登録の復旧は可能で、通常設定へ切り替えてbrokerを起動しない。

Unregisterは登録と本人のbroker起動登録を解除し、プログラムと設定を残す。
Windowsが残すユーザー側のTIP有効設定も保持する。
native COMの所有中に保護rootへ保存した記録と、SID・root・所有版・設定内容を照合し、
一致する残留設定だけを再導入時に受け入れる。Statusの`UserTipSettingsRetained`で確認できる。
所有記録がない設定、変更された設定、machine TIPが残る状態は管理外として拒否する。
本体を削除するときは解除成功を確認し、別IMEへ切り替えてアプリを閉じ、サインアウト・サインイン後に
専用rootの`install-root.json`を確認してそのrootだけを削除する。設定・辞書・モデルは別途扱う。

## ライセンス

本体はMIT（`LICENSE`）。依存物は`licenses/`を参照。llama.cppはupstream v0.6.0へzenz対応差分を適用したもの。
モデルと辞書は別途取得し、それぞれのライセンスに従う。
