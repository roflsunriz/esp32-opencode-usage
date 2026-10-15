# findings.md

esp32-opencode-usage の実現手段、共通指針の適用条件と調査知見の正本。作業前に全文読む。利用者向けの導入・使用方法はREADME、更新の手順はhow-to-update.md、実施結果と未検証事項はverification.mdを参照する。移した記述の確認日・条件・留保は元のまま保持し、文書移動を再実測と解釈しない。

## 共通指針の適用条件と代替検証

- 対象の共通ルール: 設計と実装方針／検証／生成物とビルド成果物
- 理由: ESP32 C++とWindowsホストを組み合わせ、USBの連続送受信はCH340実測に基づきpySerial workerを使用する。Bunのみでは実機品質を代替できない。
- 適用範囲: ファームウェアとホストのビルド・テスト、USB通信
- 同等の品質を保つ手段・検証: PlatformIO nativeと実機、ホストの型・lint・テスト、全フラッシュ退避と照合を併用する。具体的な実測・未検証範囲は以下とverification.mdを確認する。

新たな免除や操作許可を追加するものではない。AGENTS.mdの禁止・承認条件と共通指針は引き続き有効。

<a id="agents-1"></a>

## 取得方式と認証（2026-09-22実測）

- Goコンソールは `https://opencode.ai/console/{workspace}/go` へ移行した。資産は `/console/assets/` 配下の分割チャンクで、旧来の `/_build/assets/` と `queryLiteSubscription_query` の取得先IDは廃止された。固定ハッシュを製品コードに埋め込まない。
- 使用量は `GET /console/api/go/status` に `x-org-id: {workspace}` ヘッダーを付けて取得する。`access.meters` の `fiveHour`/`week`/`month` に `limitMicroCents`、`usedMicroCents`（文字列または数値）がある。金額は1セント=1,000,000 microCents（1ドル=100,000,000）単位の制限計算用換算額であり、請求額と混同しない。使用率は `used/limit*100` で計算する。`fiveHour`/`week` のリセットは各メーターの `resetsAt`（ISO8601 UTC）、`month` は `access.endsAt` から求める。2026-09-27の未使用5時間枠は `startsAt:null`・`resetsAt:null`・`usedMicroCents:"0"` の明示的な組合せで返り、0%とリセット時刻なしで表示する。未知の形式、欠損、取得エラーを0%として表示しない。詳細は `host/api.ts`、`host/usage.ts`、`docs/protocol.md`。
- 認証はコンソールのHttpOnly `__Host-console_session` Cookieを使う。旧来の `auth` Cookieだけでは401になる。初回ログインはPCで行い、セッションCookie・Wi-Fi設定・ワークスペースをESP32のNVSへ保存する。通常運用はESP32のHTTPS直取得で、PCは不要、USBは給電だけにする。PC中継方式へ戻さない。`.private/` は認証、バックアップ、調査資産を置くGit管理外領域で、内容をログやコミットへ含めない。
- 旧形式の認証レコード（`auth=`）はサーバー側で無効のため、本体起動時に破棄して初期設定画面に戻す。更新後はPC操作画面から再ログインと再保存が1回必要になる。

移動元: `AGENTS.md`。

<a id="agents-2"></a>

## 実機検証と復旧

- USB機器を書き換える前に、全フラッシュを読み取り、実機とのdigest一致を確認する。検証後は全フラッシュを書き戻して再照合する。アプリ領域だけの退避ではNVSや設定を復旧できない。
- Wi-Fi設定済みの実機は再起動のたびにDHCP取得IPなどをNVSへ書き戻すため、読み出しと照合の間にリセットを挟むとdigestが合わない。退避は `--after no_reset` で読み出し後に待機させ、そのまま `--before no_reset` で照合する（2026-09-22にCOM7で確認）。esptoolはv4.11.0（`.platformio` の同梱版、コマンドは `_` 表記）を使い、システムのv5系とは使い分ける。
- 今回接続されたESP32は4MBフラッシュ、COM3だった。ポートと容量は毎回実測し、固定の機器識別情報を一般仕様にしない。
- 2026-09-16のCOM6基板（ESP32-D0WD-V3・4MB）では `pio run -e esp32dev -t upload` が `Wrong boot mode detected (0x13)` で失敗し、BOOT押下保持中に再実行して成功した。書き込み後はBOOTを離してRSTを押さないと書込待機のまま応答しない。全フラッシュ退避の `verify-flash` も460800では同エラーが出ることがあり、115200での再実行でdigest一致を確認した（`verification.md`）。
- PlatformIOのキャッシュは必要に応じ `PLATFORMIO_CORE_DIR` でリポジトリ内 `.platformio` に設定する。Bunの一時領域が権限制限に当たる環境では `TEMP`/`TMP` と `BUN_INSTALL_CACHE_DIR` を `.private/` 内へ設定して実行する。
- `.platformio/packages/tool-esptoolpy/_contrib` のDACLに許可がなく読取不可になることがある（所有者は自分のまま）。2026-09-22は `icacls`/`takeown` での修復が効かず、パッケージ全体を `.private/tool-esptoolpy.broken` へ退避して `pio run` で再取得した。壊れたパッケージを中途半端に削除するとesptool本体が欠けて `_main` なしエラーになるため、修復後に不足時はディレクトリ全体を消して `pio run` で再取得する。

移動元: `AGENTS.md`。

<a id="agents-3"></a>

## 実装上の注意（実機検証から判明）

- WindowsのCH340実機でserialport 13はBunでもNode.js単独でもwriteが停止した。import・列挙・openだけでは検出できない。製品のUSB通信は `host/serial-worker.py` をPython/pySerialで起動して行う。実機で連続送受信を検証せずネイティブNode bindingへ戻さない。
- pySerialはopen時のDTRパルスでCH340経由の自動リセットやGPIO0押下を起こす。GPIO0の押下・解放は画面反転の保存付き切替になるため、製品workerも診断スクリプトもDTR/RTSをopen前に落として開く。互換確認は再起動後のreadyではなくping応答で行う。2026-09-23にCOM7で新旧workerのuptime差（修正版は再起動なし、旧版はuptime約500msへ戻る）で確認。
- Bunの子プロセスstdinは `flush()` のPromiseを処理する。未処理の非同期EPIPEはホスト全体を終了させる。workerのcloseはシリアルを閉じて終了し、親もEOFと終了タイムアウトを扱う。退行テストは `host/device.test.ts`。
- Preferencesの `putString("")` は成功時も0を返すため、単純な `>0` 判定ではWi-Fi削除が失敗する。現在はバージョンとCRC付きの単一blobへ保存し、相関ID付きACKを保存完了後に返す。
- 2026-09-09の旧版ではAdafruitの公開SPI低レベルAPIでGRAMを読み出した。2026-09-14以降の`screenshot`はTFT_eSPIの`readRectRGB`で従来の2MHz・1行960バイトずつ読み、実機の画像一致は未再確認（`platformio.ini`、`display_controller.cpp`、`verification.md`）。
- PlatformIO nativeのWindows LLVM対応は `firmware/test/native-toolchain.py` のビルドmiddlewareで適用する。通常のpreスクリプトでCCを置換するだけではnative builderがGCC設定へ戻す。UnityのnativeテストにはC++の `main` と `UNITY_INCLUDE_DOUBLE` が必要。

- TLS信頼束はGoogle公式のGTS Root R4 (`gtsr4.pem`) とGlobalSign Root CA (`gsr1.pem`)。GlobalSign ECC R4 (`gsr4.pem`) は今回のサーバーchainとは異なる。PEMを手転記すると一文字の差でもTLS接続が失敗するため、`scripts/update-ca.py` で生成し `--check` で公式ファイルと完全一致を確認する。
- ESP32のDRAMには大きな固定HTML/JSバッファを置かない。64KiBの転送上限を逐次走査し、使用量応答だけ4KiBへ保持する。認証レコードは約4KiBあり、設定解析や保存時の複製をloopタスクのstackへ置かない。
- ESP32へ認証を送る前に `firmware=opencode-go-lcd` と `setupSchema=3` のready/ping応答を確認する。別ファームウェアが動くUSBポートへ秘密情報を送信しない。
- ESP32-2432S028の回路図ではXPT2046の`/PENIRQ`が外部10kΩプルアップ付きでGPIO36へ接続され、LOWがタッチを示す。GPIO36は入力専用なので内部プルアップを指定しない。XPT2046は直前の制御byteでPD0=1のままだとPENIRQを無効化するため、`display_controller.cpp`は起動時にPD0=0を送り、以後は別VSPI（25/33/32/39）上の通知版`SensitiveXpt2046`ドライバーでPD0=0を維持する。座標は3回近接取得し、GPIO36がHIGHで20ms安定するまで一度だけ受理する。PENIRQのグリッチ（指なしでLOWにならない割込み）は`pollTouch`で無音破棄し、読み取り成功後のみバックライト期限を更新する。根拠: ESP32-2432S028回路図のU3部、XPT2046 datasheet Table 8/PENIRQ Output、`lib/sensitive-xpt2046`。
- ILI9341 `rotation(1)`での実機DisplayタブタップはrawX約500..800、rawY約2500..2900だった。2026-09-09時点のこの基板では画面変換をaxes swap・非反転（`screenX`はrawY、`screenY`はrawX）とする。未調整時はrawX=280..3860、rawY=340..3860を維持する。2026-09-16のCOM6基板は逆に非入替配線（左タップrawX約540..630・右タップ約3430..3520、rawYはどちらも約450..630）で、2点対角の校正では直せなかった。以後はBOOT長押しで3点（左上・右上・左下）の位置・押圧値をNVS `opencode-touch`（schema v2、v1は入替として移行）へ保存し、同一行・同一列の測定差から軸方向を自動判定して次回起動時に適用する。校正の押下は普段の強さで行い、`ping`応答の`touchCalibrated`/`touchThreshold`と`type:"touch"`のraw/mapped診断で基板差を再確認する。

- BOOT(GPIO0)は通常入力として扱い、10msのesp_timerで両エッジ30msのデバウンスを行う。同期HTTPS/USB画像転送中もイベントを保持し、短押しでNVS保存とrotation 1/3切替、1.5秒以上の長押しで位置・押圧感度調整をmainで処理する。2026-09-10のCH340実機ではRTS=falseのままDTR=true→falseでGPIO0押下/解放を再現できたが、新しい長押し経路は実機未検証。`screenshot`はrotation 1の固定座標で読出して元のrotationへ戻す。
- NVS schemaは4だがUSBの`setupSchema`は互換形式の3を維持する。変更時に両者を混同してホストの認証送信判定を壊さない。旧schema 2/3から認証と消灯設定を保って移行する（`config_store.cpp`とnativeテスト参照）。配布物の`firmware-full.bin`を0へ書くとNVSも初期化するため、設定を保つ更新は個別イメージまたはPlatformIO uploadを使う。
- LCDにfillScreen/fillRectで背景を消してから文字を描くと、実機の操作ごとにちらついた。2026-09-14以降は通知版のTFT_eSPIで320×240の8-bit Sprite（約75KiB）へ完成画面を描き、16行帯の差分だけを連続帯にまとめて転送する（`include/display-diff.h`、`display_controller.cpp`）。回転・校正後は全帯を再転送する。Spriteを作成できない場合は直接描画へフォールバックするため、専用基板でTLS中のメモリとちらつきの有無を再確認する。2026-09-10の10KiB帯canvasでのACK中央値とGRAM一致は旧経路の履歴であり新経路の結果ではない。
- LCDの `OpenCode response invalid` は取得失敗表示で、PC側に同文言はない（2026-09-26に特定）。2026-09-27のCOM7再現ではidentity要求のJSON応答が745バイト・宣言長と受信長一致・HTTP 200で、未使用5時間枠のnull日時を旧パーサーが拒否していた。以前の有効期間中は796バイトだったため、応答サイズを固定仕様とみなさない。不正Cookieは401、不正ワークスペースは404になる。超過は `OpenCode response too large` に分離し、応答不正・超過時は内容を含まない診断（`component":"opencode_response`、HTTP状態・宣言長・受信バイト・内容種別）をUSBへ送る。宣言長より短い本体は途中切断として `OpenCode HTTPS failed` で報告する。詳細は `firmware/src/opencode_status.cpp`、`firmware/test/test_response_status`、`docs/protocol.md`。
- USB診断は製品の `DeviceConnection` 経路（`host/device.ts`）を使う。自作PowerShell経路ではworkerが `error` を返し続け、製品経路では1秒でping互換を確認した（2026-09-26にCOM7で確認）。原因は自作側とみなし、断定しない。
- v0.4.2公開時（2026-09-27）、WindowsローカルとUbuntu Releaseワークフローの `firmware.bin` は同じソース・依存版でもバイト一致しなかった（998,928対998,832バイト。差の詳細原因は未確定）。ローカルBINの実機検証だけで公開BINも検証済みとみなさず、公開ZIPから取り出したBINをCOM7へアプリ領域のみ書き込み、全フラッシュ事前退避・digest照合、書き込み後のdigest照合、再起動後の直接取得と設定保持を確認した。詳細は `verification.md` のv0.4.2公開後検証。

移動元: `AGENTS.md`。

<a id="agents-4"></a>

## Dependabotラベルの運用

- 明示指定のラベルはDependabotが自動初期化するとは限らない。既定ブランチの設定を正本とする専用workflowで不足分だけ維持する。検証と復旧は `how-to-update.md` のDependabotラベル節、回帰は `.github/tests/test-dependabot-labels.py`。既存の自動マージ・承認は別のworkflowであり、ラベルの変更で回避しない。
- 2026-10-05、Bun更新run `37279496981` はラベル不足でなく `Unsupported bun.lock 'lockfileVersion' 2`（最大1）で停止。実行版は `.bun-version` の1.4系、生成版は `bun run deps:lock` の公式1.3.14に分ける。1.3.14で製品テストを実行するとWindowsのMarionette fixture開始時に終了コード9となったため、実行版を下げない。lockの形式番号だけ変更しない。既存のdynamic更新は `gh run rerun` が再試行不可を返した（run_attemptは1）。詳細は `verification.md`。

移動元: `AGENTS.md`。

<a id="readme-1"></a>

## 開発と検証

```powershell
bun run lint
bun run format:check
bun run type-check
python -m mypy --strict host/serial-worker.py
bun run build
bun run test
bun run audit
pio test -e native
pio run -e esp32dev
```

Python側の実行時USBはpySerialです。Node.js 24は開発用のfake fixtureテストだけで使い、製品のUSB通信経路には使いません。

- [更新と復旧](how-to-update.md)
- [取得方式・通信仕様](docs/protocol.md)
- [依存関係とライセンス](docs/dependencies.md)
- [セキュリティ](SECURITY.md)

本プロジェクトはOpenCode非公式です。公式コンソール、認証方式、HTTPS証明書、使用量応答の変更で更新が必要になる場合があります。

移動元: `README.md`。

<a id="readme-2"></a>

## 依存更新の自動処理

Dependabot は pip／Bun の依存関係を毎週、GitHub Actionsを毎月確認します。依存更新は PR のチェック（CI）が成功した後に自動で squash merge されます。CI の失敗ジョブは 1 回だけ再実行し、設定されたlockfileの修復も試みます。再失敗した PR は残して手動で修正します。

Dependabot設定が参照するラベルは、設定変更時・ラベルの編集/削除時・毎日の検査で不足分だけ自動作成します。既存ラベルの色・説明は変更しません。開発・CI・Releaseは `.bun-version` の版を使用し、Dependabotが読める `bun.lock` 形式1の生成には `bun run deps:lock` を使います。手順は [依存更新](findings.md#update-1) を参照してください。

移動元: `README.md`。

<a id="update-1"></a>

## Dependabot PR の更新

前提は `.github/dependabot.yml` と PR 用 CI（CI）です。更新 PR の head SHA と `gh pr checks <PR番号>` の結果を確認してください。patch／minor は全チェック成功後に自動取り込みされます。初回 CI 失敗は failed jobs のみを 1 回再実行し、再失敗した PR は残して手動で修正します。

設定を変えたときは `actionlint .github/workflows/dependabot-automation.yml` と実際の PR の Actions 結果を確認します。問題があれば呼び出し先の共通 workflow SHA を直前の検証済み値へ戻すコミットを push します。取り込まれた依存更新に問題があれば通常の revert コミットで復旧します。

CI 完了より Dependabot の分類が遅れる場合は、`callback_workflow_file` が指す呼び出し側 workflow を `workflow_dispatch` し、同じ PR 番号・head SHA・全チェックを再確認する。呼び出し側のファイル名を変える際はこの入力も一緒に更新する。

移動元: `how-to-update.md`。

<a id="update-2"></a>

## Dependabotラベルの維持

`.github/workflows/dependabot-labels.yml` は既定ブランチのDependabot設定を読み、`updates` と `multi-ecosystem-groups` が明示指定するラベルの不足分だけ作成します。設定変更、ラベルの編集・削除、毎日04:17 UTC、手動実行で検査します。既存ラベルの色・説明やPRへの付与状態は変更しません。書込権限は専用ジョブの `issues: write` に限定し、PRの検証ジョブは読み取り専用です。

ラベルが不足する場合、Actionsの「Maintain Dependabot labels」を既定ブランチで手動実行し、`All configured Dependabot labels exist` を確認します。権限エラーやarchivedラベルとの競合は失敗として通知されます。Dependabot設定のラベル名を別名へ付け替えたり、CI・承認条件を無効化して回避しないでください。

変更時は次を実行し、Actions上でも検証してください。

```powershell
python -m pip install PyYAML==6.0.3
python .github/tests/test-dependabot-labels.py
actionlint .github/workflows/dependabot-labels.yml
```

Dependabot更新そのものの成功は、更新ジョブのログと結果で確認します。指定ラベルが無い場合はGitHubの仕様では無視されるため、実際のジョブ失敗をラベル不足だけと決めつけないでください。更新チェックの手動実行はGitHubのDependabot更新画面で提供される場合に使用し、Actions APIが再試行不可と返す実行は成功扱いしません。


### Bun lock形式の互換性

2026-10-05のDependabot Bun更新は `lockfileVersion: 2` を拒否し、形式1までの対応と報告した。製品実行はBun 1.4系を維持し、lock生成だけ `bun run deps:lock` で公式Bun 1.3.14を使用する。`npx` が必要なためNode.jsを用意する。CI・Releaseは `.bun-version`（1.4.2）を参照し、配布ホストへ同梱する。新しいBunでlockを生成してからversion数値だけ手書きで戻してはいけない。

Bun更新時は実際のDependabotログ/パーサーで対応形式を確認し、`deps:lock` の生成版、lockfile、`.github/tests/test-dependabot-labels.py` の互換検査を一緒に変更する。生成版の更新を `.bun-version` の実行版と混同しない。依存の追加・更新後は `bun run deps:lock`、`bun install --frozen-lockfile`、lint、format:check、type-check、build、test、auditを通す。既存の自動修復も同じ生成コマンドを使用する。

移動元: `how-to-update.md`。

<a id="verification-1"></a>

## Dependabot 自動処理（2026-09-23）

`.github/workflows/dependabot-automation.yml` を actionlint で検査し、PR 用 workflow 名（CI）と一致することを確認する。Dependabot の patch／minor かつ全 PR チェック成功の場合だけ取り込み、major・古い SHA・再失敗は残す。

実際の Dependabot PR がまだない場合、動作経路は未検証として扱う。実 PR 発生後に自動化ジョブ、CI の再試行、マージ結果を確認する。

大量の Dependabot PR により CI 完了より分類が遅れる場合でも、分類後の `workflow_dispatch` が現在の PR 番号と head SHA を照合して再評価する。別の作成者、古い SHA、未完了の CI はマージしない。

移動元: `verification.md`。

## 関連文書

- [利用者向けREADME](README.md)
- [更新と復旧](how-to-update.md)
- [検証結果と未検証範囲](verification.md)
- [変更履歴](CHANGELOG.md)
