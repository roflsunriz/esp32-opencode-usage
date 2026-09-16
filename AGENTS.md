# AGENTS.md

## 作業開始前の必須手順（最優先・例外なし）

1. エージェントは、調査、計画、コマンド実行、スキル利用、ファイル編集、コミット、プッシュを始める前に、必ずリポジトリ直下の `.\COMMON-AGENTS.md` を開き、先頭から末尾まで全文を読む。
2. `COMMON-AGENTS.md` はGit管理外のシンボリックリンクである。`git`や既定のignore設定が有効な`rg --files`の検索結果だけで、ファイルが存在しないと判断してはならない。PowerShellでは最初に次を実行する。

```powershell
Get-Content -Raw -LiteralPath .\COMMON-AGENTS.md
```

3. 読み取りに失敗した場合、出力が省略された場合、または末尾まで読めたことを確認できない場合は、一切の作業を開始せず、パスとシンボリックリンク先を確認して全文を再取得する。必要なら分割して末尾まで読む。
4. 全文を読了するまで、ローカル `AGENTS.md` だけを根拠に作業を続けてはならない。読了後は `COMMON-AGENTS.md` を最優先の指針とし、読了直後の最初の進捗報告で全文を読了したことを明示する。
   このファイルでは `esp32-opencode-usage` 固有の補足だけを記載する。

## 目的

- esp32-2432s028r ili9341 esp-wroom-32 tft lcd 開発ボード上のLCDモニタに OpenCode Go の5時間制限のプログレスバーとパーセンテージ、使用済みドル表記/最大制限ドル表記、週間制限のプログレスバーとパーセンテージ、使用済みドル表記/最大制限ドル表記、1ヶ月制限のプログレスバーとパーセンテージ、使用済みドル表記/最大制限ドル表記を表示する
- OpenCode Go ページ https://opencode.ai/workspace/wrk_01M1J5EPMB84QKX30P1A3ZJTAQ/go （ログイン済みページ）の公式資産をde-minify, または実リクエストのキャプチャ解析などを通して実装する
- ログインを自前実装し、認証情報を保持する

## 取得方式と認証（2026-09-09実測）

- Goコンソールの `queryLiteSubscription_query` は `/_server` のサーバー関数を呼ぶ。関数IDはデプロイごとに変わるため、認証済みGoページが参照する公式JavaScriptから検出する。固定ハッシュを製品コードに埋め込まない。
- `rollingUsage` / `weeklyUsage` / `monthlyUsage` に `usage`、`limit`、`usagePercent`、`resetInSec` がある。金額は1ドル=100,000,000単位。モデルの倍率を反映した制限計算用の換算額であり、請求額と混同しない。割合は公式の丸め値を使い、丸めた割合から金額を逆算しない。詳細は `host/api.ts`、`host/usage.ts`。
- 現在のレスポンスはSerovalのJavaScriptストリーム。外部コードをevalせず、必要な3期間の数値だけを厳格に読み取る。未知の形式、欠損、取得エラーを0%として表示しない。
- 公式ログインはGitHub/Google認証を利用し、コンソールのHttpOnly `auth` Cookieで使用量を取得する。初回ログインはPCで行い、認証Cookie・Wi-Fi設定・ワークスペースをESP32のNVSへ保存する。通常運用はESP32のHTTPS直取得で、PCは不要、USBは給電だけにする。PC中継方式へ戻さない。`.private/` は認証、バックアップ、調査資産を置くGit管理外領域で、内容をログやコミットへ含めない。

## 実機検証と復旧

- USB機器を書き換える前に、全フラッシュを読み取り、実機とのdigest一致を確認する。検証後は全フラッシュを書き戻して再照合する。アプリ領域だけの退避ではNVSや設定を復旧できない。
- 今回接続されたESP32は4MBフラッシュ、COM3だった。ポートと容量は毎回実測し、固定の機器識別情報を一般仕様にしない。
- 2026-09-16のCOM6基板（ESP32-D0WD-V3・4MB）では `pio run -e esp32dev -t upload` が `Wrong boot mode detected (0x13)` で失敗し、BOOT押下保持中に再実行して成功した。書き込み後はBOOTを離してRSTを押さないと書込待機のまま応答しない。全フラッシュ退避の `verify-flash` も460800では同エラーが出ることがあり、115200での再実行でdigest一致を確認した（`verification.md`）。
- PlatformIOのキャッシュは必要に応じ `PLATFORMIO_CORE_DIR` でリポジトリ内 `.platformio` に設定する。Bunの一時領域が権限制限に当たる環境では `TEMP`/`TMP` と `BUN_INSTALL_CACHE_DIR` を `.private/` 内へ設定して実行する。
- `.platformio/packages/tool-esptoolpy/_contrib` のDACLに許可がなく読取不可になることがある（所有者は自分のまま）。`icacls <path> /grant <user>:(OI)(CI)F /t` で修復できる。権限不足のまま `pio run` するとbootloader生成でPermissionErrorになる。壊れたパッケージを中途半端に削除するとesptool本体が欠けて `_main` なしエラーになるため、修復後に不足時はディレクトリ全体を消して `pio run` で再取得する。

## 実装上の注意（実機検証から判明）

- WindowsのCH340実機でserialport 13はBunでもNode.js単独でもwriteが停止した。import・列挙・openだけでは検出できない。製品のUSB通信は `host/serial-worker.py` をPython/pySerialで起動して行う。実機で連続送受信を検証せずネイティブNode bindingへ戻さない。
- Bunの子プロセスstdinは `flush()` のPromiseを処理する。未処理の非同期EPIPEはホスト全体を終了させる。workerのcloseはシリアルを閉じて終了し、親もEOFと終了タイムアウトを扱う。退行テストは `host/device.test.ts`。
- Preferencesの `putString("")` は成功時も0を返すため、単純な `>0` 判定ではWi-Fi削除が失敗する。現在はバージョンとCRC付きの単一blobへ保存し、相関ID付きACKを保存完了後に返す。
- 2026-09-09の旧版ではAdafruitの公開SPI低レベルAPIでGRAMを読み出した。2026-09-14以降の`screenshot`はTFT_eSPIの`readRectRGB`で従来の2MHz・1行960バイトずつ読み、実機の画像一致は未再確認（`platformio.ini`、`display_controller.cpp`、`verification.md`）。
- PlatformIO nativeのWindows LLVM対応は `firmware/test/native-toolchain.py` のビルドmiddlewareで適用する。通常のpreスクリプトでCCを置換するだけではnative builderがGCC設定へ戻す。UnityのnativeテストにはC++の `main` と `UNITY_INCLUDE_DOUBLE` が必要。

- TLS信頼束はGoogle公式のGTS Root R4 (`gtsr4.pem`) とGlobalSign Root CA (`gsr1.pem`)。GlobalSign ECC R4 (`gsr4.pem`) は今回のサーバーchainとは異なる。PEMを手転記すると一文字の差でもTLS接続が失敗するため、`scripts/update-ca.py` で生成し `--check` で公式ファイルと完全一致を確認する。
- ESP32のDRAMには大きな固定HTML/JSバッファを置かない。64KiBの転送上限を逐次走査し、使用量応答だけ4KiBへ保持する。認証レコードは約4KiBあり、設定解析や保存時の複製をloopタスクのstackへ置かない。
- ESP32へ認証を送る前に `firmware=opencode-go-lcd` と `setupSchema=3` のready/ping応答を確認する。別ファームウェアが動くUSBポートへ秘密情報を送信しない。
- ESP32-2432S028の回路図ではXPT2046の`/PENIRQ`が外部10kΩプルアップ付きでGPIO36へ接続され、LOWがタッチを示す。GPIO36は入力専用なので内部プルアップを指定しない。XPT2046は直前の制御byteでPD0=1のままだとPENIRQを無効化するため、`display_controller.cpp`は起動時にPD0=0を送り、以後は別VSPI（25/33/32/39）上の通知版`SensitiveXpt2046`ドライバーでPD0=0を維持する。座標は3回近接取得し、GPIO36がHIGHで20ms安定するまで一度だけ受理する。根拠: ESP32-2432S028回路図のU3部、XPT2046 datasheet Table 8/PENIRQ Output、`lib/sensitive-xpt2046`。
- ILI9341 `rotation(1)`での実機DisplayタブタップはrawX約500..800、rawY約2500..2900だった。2026-09-09時点のこの基板では画面変換をaxes swap・非反転（`screenX`はrawY、`screenY`はrawX）とする。未調整時はrawX=280..3860、rawY=340..3860を維持し、BOOT長押しで取得した2点の位置・押圧値をNVS `opencode-touch` へ保存して次回起動時に適用する。`type:"touch"`のraw/mapped診断で基板差を再確認する。

- BOOT(GPIO0)は通常入力として扱い、10msのesp_timerで両エッジ30msのデバウンスを行う。同期HTTPS/USB画像転送中もイベントを保持し、短押しでNVS保存とrotation 1/3切替、1.5秒以上の長押しで位置・押圧感度調整をmainで処理する。2026-09-10のCH340実機ではRTS=falseのままDTR=true→falseでGPIO0押下/解放を再現できたが、新しい長押し経路は実機未検証。`screenshot`はrotation 1の固定座標で読出して元のrotationへ戻す。
- NVS schemaは4だがUSBの`setupSchema`は互換形式の3を維持する。変更時に両者を混同してホストの認証送信判定を壊さない。旧schema 2/3から認証と消灯設定を保って移行する（`config_store.cpp`とnativeテスト参照）。配布物の`firmware-full.bin`を0へ書くとNVSも初期化するため、設定を保つ更新は個別イメージまたはPlatformIO uploadを使う。
- LCDにfillScreen/fillRectで背景を消してから文字を描くと、実機の操作ごとにちらついた。2026-09-14以降は通知版のTFT_eSPIで320×240の8-bit Sprite（約75KiB）へ完成画面を描き、16行帯の差分だけを連続帯にまとめて転送する（`include/display-diff.h`、`display_controller.cpp`）。回転・校正後は全帯を再転送する。Spriteを作成できない場合は直接描画へフォールバックするため、専用基板でTLS中のメモリとちらつきの有無を再確認する。2026-09-10の10KiB帯canvasでのACK中央値とGRAM一致は旧経路の履歴であり新経路の結果ではない。
