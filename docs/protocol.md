# 取得方式と通信仕様

確認日: 2026-09-22

この文書は、Windows PCで初回設定を行い、その後ESP32が公式HTTPSサービスから直接使用量を取得する構成を定義します。PCは認証情報を準備する初期設定端末です。通常運用の使用量取得にPC中継は使いません。

## PC側の公式データ取得

PCとESP32の `OpenCodeClient` は、コンソールの使用量API `GET /console/api/go/status` へ、ワークスペースIDを `x-org-id` ヘッダーに付けて取得します。認証はコンソールのセッションCookie（`__Host-console_session=`）を使い、旧来の `auth` Cookieでは取得できません（401）。固定の取得先IDやページ資産の解析は行いません。

公式応答の `access.meters` から次の3期間を読み取ります。

- `fiveHour`（5時間・表示名rolling）
- `week`（週間・表示名weekly）
- `month`（月間・表示名monthly）

各期間には `limitMicroCents`、`usedMicroCents` が含まれます。金額の単位は1セント=1,000,000 microCents、すなわち1ドル=100,000,000 microCentsです。使用率は `used/limit*100` で計算します。`fiveHour` と `week` のリセット時刻は各メーターの `resetsAt`（ISO8601 UTC）、`month` は `access.endsAt` から求めます。欠損、負値、非有限値、0の上限、不正な時刻は拒否し、0%として表示しません。

応答は `application/json` です。旧来の `/_server` へのPOSTやSerovalストリームは2026-09-22に公式コンソールから廃止されたため使いません。

## 初期設定フロー

1. PCで公式GitHub/Googleログインを完了し、コンソールのセッションCookieをWindows DPAPIで保存する。
2. PCが使用量APIから使用量を取得し、ワークスペースを確定する。
3. USBシリアルでESP32へ `config` フレームを送り、保存完了ACKを待つ。
4. ESP32が保存したセッションCookieとワークスペースを使って公式HTTPSサービスへ直接接続する。
5. ESP32が取得した使用量をLCDへ表示し、PC画面には直接更新確認を表示する。

2026-09-09に、正しいGTS Root R4＋GlobalSign Root CAの信頼束を使ったHTTPS取得、NTP同期、再起動後の認証保持、PC設定アプリ停止中の継続更新を実機確認しました。

## 設定フレーム

USBは115200 baud、1行につき1つのJSONです。設定フレームのフィールドは次の通りです。

| フィールド        | 型・上限        | 意味                                                       |
| ----------------- | --------------- | ---------------------------------------------------------- |
| `version`         | `1`             | プロトコルバージョン                                       |
| `type`            | `"config"`      | 設定フレームであることを示す                               |
| `enabled`         | boolean         | ESP32の直接取得を有効にするか                              |
| `ssid`            | 64バイト未満    | Wi-Fi SSID                                                 |
| `password`        | 65バイト未満    | Wi-Fiパスワード。空文字はオープンネットワーク用            |
| `authCookie`      | 4097バイト未満  | `__Host-console_session=`で始まるコンソールのセッションCookie |
| `workspace`       | 96バイト未満    | `wrk_`で始まるワークスペースID                             |
| `queryId`         | 65バイト未満    | 互換保持の予備欄。常に空文字を送り、本体は無視する         |
| `pollIntervalSec` | 15〜86400       | 取得間隔（秒）                                             |
| `requestId`       | 16桁の小文字hex | 保存ACKを要求する相関ID                                    |

設定保存後、ESP32は次の形式でACKを返します。認証CookieやWi-FiパスワードはACKに含めません。

```json
{
  "version": 1,
  "type": "ack",
  "accepted": "config",
  "requestId": "0123456789abcdef",
  "wifiEnabled": true,
  "pollIntervalSec": 60
}
```

保存失敗、形式不正、CRC不一致は `error` フレームで返し、PCは設定完了として扱いません。`enabled:false` と空文字列の設定を送ると、ESP32側のWi-Fi・認証情報・取得先情報を削除します。

## 使用量フレームとACK

USB表示用の使用量フレームは次の形です。

```json
{
  "version": 1,
  "type": "usage",
  "updatedAt": 1700000000,
  "rolling": { "used": 3, "limit": 12, "percent": 25, "resetInSec": 9000 },
  "weekly": { "used": 12, "limit": 30, "percent": 40, "resetInSec": 259200 },
  "monthly": { "used": 36, "limit": 60, "percent": 60, "resetInSec": 1728000 }
}
```

ESP32は3期間を一括検証してから表示へ反映します。一部の期間だけ更新しません。使用量ACKには `accepted:"usage"`、送信元、更新時刻、描画回数、期間数、各数値を返します。

ESP32が公式HTTPSサービスから直接取得して表示した場合、ACKの更新時刻をUSB worker経由でPCへ返します。操作画面はこの値を「ESP32の直接更新を確認」と表示し、PCを終了できる判断材料にします。

## USB workerプロトコル

製品のUSB通信は `host/serial-worker.py` をPython/pySerialで起動して行います。Bunはworkerの標準入出力だけを使い、ネイティブNode bindingへ依存しません。

ポート一覧:

```text
python -u host/serial-worker.py --list
```

接続:

```text
python -u host/serial-worker.py --port COM3
```

workerの入力は改行JSONです。

```json
{"type":"write","id":1,"data":"{\"version\":1,\"type\":\"ping\"}\n"}
{"type":"close"}
```

workerは `open`、`data`、`written`、`error`、`close` を返します。書き込み、設定ACK、worker終了にはタイムアウトを設け、終了時にシリアルポートを閉じます。認証Cookieや設定値をworkerのログへ出力しません。

## 保存とセキュリティ

ESP32のNVSはスキーマ4とCRC付きの単一レコードで設定を保存します。認証Cookie、Wi-Fiパスワード、ワークスペースはNVSに残ります。通常のESP32フラッシュは暗号化されていないため、物理機器と全フラッシュバックアップを秘密情報として扱います。

PC側の認証CookieはWindows DPAPIで保護します。PCのlogoutはPC側セッションだけを削除し、ESP32の保存情報は消しません。ESP32側を消去する場合はUSB接続した初期設定画面の削除操作を使います。

## ボードとLCD

MOSI=13、MISO=12、SCLK=14、CS=15、DC=2、BL=21、RST=-1。ILI9341の描画はAdafruitライブラリを使います。診断用のスクリーンショット読み出しはUSB専用で、通常の直接取得経路やネットワークから開始しません。

一次資料:

- [公式コンソール](https://opencode.ai/console)（ログイン済みであること）
- [pySerial公式リポジトリ](https://github.com/pyserial/pyserial)
- [pySerial公式ドキュメント](https://pyserial.readthedocs.io/en/latest/)
- [Adafruit ILI9341](https://github.com/adafruit/Adafruit_ILI9341)

## 自動消灯の設定

`backlightTimeoutSec` は15、30、60、120、300、600、1800、3600、7200だけを受理する。初期値60。`config`にも含められ、`display`では既存のWi-Fiと認証を保持して時間だけを保存する。

```json
{
  "version": 1,
  "type": "display",
  "backlightTimeoutSec": 30,
  "requestId": "0123456789abcdef"
}
```

`wake` は点灯して期限を再設定する。両操作とも同じ `requestId` と、操作名の `accepted` を持つACKを返す。状態には `backlightOn`、`backlightTimeoutSec` が含まれ、pingには残り時間とGPIO21読戻し値も含む。通信や再描画自体は期限を延長しない。

消灯判定とBOOTの検出はHTTPS通信とは独立した10ms周期のタイマーで行う。タッチの起床はGPIO36の割り込みで受け付け、実際の画面設定はLCD上のUsage/Displayタブから行う。

NVS schema 2/3のCRCが正常ならWi-Fi・認証などを保持してschema 4へ移行する。schema 2では消灯時間60秒を補い、schema 3の消灯時間は保持する。画面の向きは通常向きを初期値とする。USBの設定形式は変わらないため、認証送信前の `setupSchema` は3を維持する。

## BOOTによる画面反転

GPIO0のLOW/HIGHがそれぞれ30ms安定した押下・解放を1クリックとして受け付け、保存成功後にILI9341のrotation 1/3を切り替える。タイマーはクリックを記録して点灯し、保存・再描画はメイン処理で行うため、HTTPS通信中の反転は通信完了後に反映される。長押しは繰り返さず、起動時に押されていたボタンの解放もクリックと数えない。

タッチ座標は反転時に `x=319-x, y=239-y` を適用する。BOOT操作は消灯期限を再設定して点灯する。Usage/Displayタブ、表示データ、状態メッセージ、消灯設定は維持する。

保存成功時は `accepted:"display"` のACKを返し、`screenFlipped` に保存済みの向きを示す（BOOT操作にはrequestIdなし）。ready、ping、config/display/wake ACKにも同じ値を含める。保存失敗時は向きを変更せずerrorを返す。PCからの初期設定・認証削除・消灯時間変更は向きを保持する。

配線の根拠: [Apache NuttXの本ボードのBOOT説明](https://nuttx.apache.org/docs/12.8.0/platforms/xtensa/esp32/boards/esp32-2432S028/index.html#board-buttons)。

`screenshot` は基板の通常向きを固定した座標でGRAMを読み出し、反転中なら上下逆の実表示を返す。読み出し後に設定済みのrotationへ戻す。

## ちらつきを抑える描画

RGB565の320×16画素（10KiB）の作業領域に完成済みの帯を描いてからLCDへ転送する。LCDを背景色で消してから文字・ボタンを逐次描画する処理は使わない。全画面は15本の帯で構成し、状態メッセージだけの変更は最下部の帯だけを転送する。同じタブ・消灯時間・表示値・状態メッセージの再描画を省き、画面に出ない取得時刻だけが変わっても転送しない。作業領域は固定長で、描画ごとの動的確保や全面フレームバッファを避ける。
