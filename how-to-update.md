# 更新と復旧

この手順は、Windows PCを初回設定に使い、以後ESP32単独でWi-Fiから使用量を取得する構成を対象にします。更新前に、書き込み対象の全フラッシュを退避してください。

## 更新前

1. ESP32へ給電しているUSBを外し、PCの操作画面とUSB接続を停止する。
2. `git status --short` で自分の変更を確認する。
3. Pythonの実行環境を確認する。

```powershell
python --version
python -m pip install -r requirements.txt
python -m pip install -r requirements-dev.txt
```

4. 書き込む機器の全フラッシュを退避し、実機との一致を確認する。

```powershell
powershell -ExecutionPolicy Bypass -File scripts/backup-firmware.ps1 -Port COM3 -OutputPath .private/backups/before-update.bin
```

既存の元ファームウェアは上書きせず、機器ごとに別の出力先を指定します。

## ホストの更新

```powershell
bun install --frozen-lockfile
bun run lint
bun run format:check
bun run type-check
python -m mypy --strict host/serial-worker.py
bun run build
bun run test
bun run audit
```

`start.cmd` を起動し、公式ログイン、USBポートの列挙、接続、初期設定の保存完了を確認します。初期設定の保存では、暗号化済みPCセッションから認証Cookie、ワークスペース、動的な取得先IDをESP32へ送り、ESP32の保存ACKを待ちます。

## ファームウェアの更新

```powershell
$env:PYTHONUTF8 = '1'
pio test -e native
pio run -e esp32dev
pio run -e esp32dev -t upload --upload-port COM3
```

書き込み後にPCを起動し、USBで接続して3期間のプレビューを確認します。初期設定が消えた場合は、ログイン済みのPC画面からESP32初期設定を再保存します。ESP32の直接HTTPS、CA証明書、NTPの実装や実機検証が更新対象に含まれる場合は、検証完了まで通常運用へ戻したと断言しません。

## ロールバック

USBを切断し、更新前の全フラッシュを復元します。

```powershell
powershell -ExecutionPolicy Bypass -File scripts/restore-firmware.ps1 -Port COM3 -BackupPath .private/backups/before-update.bin
```

復元の成功メッセージだけでなく、最後の `verify-flash` が成功したことを確認します。NVSも含めて戻るため、更新前にESP32へ保存していたWi-Fiと認証Cookieも復元されます。別の機器のバックアップを使わないでください。

ホスト側の問題は以前のリリースまたはコミットへ戻し、対応するlockfileで依存関係を再インストールします。PC側のlogoutはPCのセッションだけを削除し、ESP32の保存情報を消す操作ではありません。ESP32側を消去する場合は、USB接続した初期設定画面から**ESP32の設定・認証を削除**を実行します。

## 公式サイトの形式変更

`docs/protocol.md` と `host/api.ts` を確認し、認証済み公式ページが参照する動的な取得先、引数、レスポンスを実測します。認証済みレスポンスを公開テストへ直接コピーせず、必要な数値構造だけを匿名化したフィクスチャにします。未知の形式を成功扱いにするフォールバックは追加しません。

## リリース

`package.json` のバージョンを更新し、`CHANGELOG.md` の `Unreleased` を `## [X.Y.Z] - YYYY-MM-DD` に移します。検証を通してから `vX.Y.Z` タグを公開すると、ワークフローが該当変更履歴を抽出し、ホストとファームウェアのアーカイブを作成します。

公開前にアーカイブを展開し、`start.cmd`、`host/serial-worker.py`、依存設定、ファームウェアのイメージ、復旧手順、第三者ライセンスを確認します。ESP32単独のHTTPS取得を配布物の機能として案内する場合は、CA証明書、時刻同期、認証Cookie保存、再起動後の直接更新を実機で確認してから公開します。タグやリリースの公開操作はこの手順では行いません。

## 消灯設定の移行

schema 2から3への更新時はWi-Fi・認証を保持し、消灯時間を初期値1分に設定します。更新後はLCDのDisplayタブ、またはPCの消灯設定から変更できます。選択値の保存、消灯、タッチによる起床を確認してください。
