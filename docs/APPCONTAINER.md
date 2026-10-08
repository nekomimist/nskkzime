# AppContainerからの入力

Windows検索とMicrosoft Storeは、この端末では実際にAppContainerのトークンで動作する。
Windows Terminalのimmersive/XAML互換性登録だけでは、私用辞書やデスクトップ用IPCにアクセスできない。

## 構成

`nskkzime_broker.exe` は本人の通常権限のデスクトッププロセスとして常駐する。
インストーラーはHKCUのRunに起動を登録し、更新時はExplorer経由で `--replace` を実行する。
本人・セッション単位のmutexで多重起動を防ぎ、`--stop` で終了する。
モデルのロード・待機終了は従来どおり `nkkz-ime-server.exe` が担当する。

許可するパッケージは次の2つに限定する。

- `MicrosoftWindows.Client.CBS_cw5n1h2txyewy`（Windows検索）
- `Microsoft.WindowsStore_8wekyb3d8bbwe`（Microsoft Store）

IMEは最初に設定、ローマ字規則、コンパイル済み辞書イメージを非同期取得する。
かな入力とキー処理は取得・推論の完了を待たない。学習とzenz要求もワーカー経由で中継する。
表示回数・ASDFJKL・文脈利用等の既存設定をそのまま適用する。
AppContainerから設定GUI・辞書管理GUIを起動する機能は提供しない。

## 接続とデータの境界

- Program Filesの公開プログラムだけをALL APPLICATION PACKAGESに読み取り・実行許可する。
  書き込みはSYSTEMとAdministratorsのみ。設定・辞書・学習・モデル・入力ログのファイルACLは変更しない。
- パイプはコンテナーSIDとWindowsセッションごとの名前空間に作成し、
  本人SIDと許可したコンテナーSIDにだけ通信権限を与える。コンテナーにpipe instance作成権は渡さない。
- サーバーはクライアントの実プロセストークンからユーザー・セッション・コンテナーSIDを照合する。
  クライアントも同一ユーザー・セッションと、IME DLLと同じフォルダーのbroker実行ファイルを確認する。
  このためbrokerのプロセスとトークンには、許可したSID向けの照会権だけを追加する。
- 通信は長さ上限、期限、取消付き。任意ファイルパス・コマンド・辞書インポートの要求は受け付けない。
  通常の候補学習／削除・スナップショット・推論・明示有効化された入力ログだけを中継する。
- 設定の内容ハッシュを要求に付ける。ワーカーは作成時のハッシュを保持し、設定変更後の古い要求は拒否する。
  取得中の古い初期化が、新しい初期化の設定を上書きすることも防ぐ。
- パスワード等への推論・学習抑止、属性不明での学習・ログ抑止は従来のTSF判定を通す。
  入力ログはアプリ側で現在の文脈と属性を再検証した後だけ要求する。

デスクトップ側がコンテナーの名前空間にオブジェクトを作る方式については、
[Microsoftの名前付きオブジェクト共有資料](https://learn.microsoft.com/en-us/windows/apps/develop/communication/sharing-named-objects)を参照。
私用ファイルを全AppContainerへ公開したり、ネットワークのloopback exemptionを追加したりしない。

## テストと制約

`app_container` は一時的なAppContainerプロファイルを2つ作り、実際にそのトークンで子プロセスを起動する。
テスト用実行ファイルにだけ一時的な読み取り・実行ACEを追加し、終了時にACLとプロファイルを戻す。
辞書・設定は生成したfixtureを使い、私用ファイルを直接読めない状態でTSF・学習・推論を試す。
未許可コンテナー、別の実行ファイル、古い設定、任意ファイル要求の拒否も検証する。
`app_container_zenz` は設定済みの実モデルで文脈による採点と辞書にない読みの生成を確認する。

brokerの接続不良や初期化失敗では通常のかな入力は継続できる。
broker起動後、設定変更後、版更新後は入力先アプリを起動し直す。
任意のUWPアプリや別OS版のパッケージ名を自動的に許可しない。
実際のアプリでの確認範囲は [TSF検証](TSF_VALIDATION.md) に記録する。
