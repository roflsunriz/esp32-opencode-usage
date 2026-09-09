# 取得方式と通信仕様

確認日: 2026-09-09

この文書は、Windows PCで初回設定を行い、その後ESP32が公式HTTPSサービスから直接使用量を取得する構成を定義します。PCは認証情報と動的な取得先を準備する初期設定端末です。通常運用の使用量取得にPC中継は使いません。

## PC側の公式データ取得

PCの `OpenCodeClient` は、認証済みの公式Goページが読み込むJavaScriptから、デプロイごとに変わる `queryLiteSubscription_query` の取得先IDを検出します。固定された関数IDを埋め込まず、取得できない形式は成功扱いにしません。

公式応答から次の3期間を読み取ります。

- `rollingUsage`
- `weeklyUsage`
- `monthlyUsage`

各期間には `usage`、`limit`、`usagePercent`、`resetInSec` が含まれます。金額の単位は1ドル=100,000,000単位です。`usagePercent` は公式の丸め値をそのまま使い、丸めた割合から金額を逆算しません。欠損、負値、非有限値、0の上限、金額と割合の不整合は拒否します。

現在の応答はJavaScript形式のSerovalストリームです。外部コードを `eval` せず、必要な数値レコードだけを厳格に読み取ります。

## 初期設定フロー

1. PCで公式GitHub/Googleログインを完了し、認証CookieをWindows DPAPIで保存する。
2. PCが公式ページから使用量を取得し、ワークスペースと動的な取得先IDを確定する。
3. USBシリアルでESP32へ `config` フレームを送り、保存完了ACKを待つ。
4. ESP32が保存した認証Cookie、ワークスペース、取得先IDを使って公式HTTPSサービスへ直接接続する。
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
| `authCookie`      | 4097バイト未満  | `auth=`で始まるOpenCode認証Cookie                          |
| `workspace`       | 96バイト未満    | `wrk_`で始まるワークスペースID                             |
| `queryId`         | 65バイト未満    | 公式ページから検出した64桁の取得先ID。再検出時は空にできる |
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

ESP32のNVSはスキーマ2とCRC付きの単一レコードで設定を保存します。認証Cookie、Wi-Fiパスワード、ワークスペース、取得先IDはNVSに残ります。通常のESP32フラッシュは暗号化されていないため、物理機器と全フラッシュバックアップを秘密情報として扱います。

PC側の認証CookieはWindows DPAPIで保護します。PCのlogoutはPC側セッションだけを削除し、ESP32の保存情報は消しません。ESP32側を消去する場合はUSB接続した初期設定画面の削除操作を使います。

## ボードとLCD

MOSI=13、MISO=12、SCLK=14、CS=15、DC=2、BL=21、RST=-1。ILI9341の描画はAdafruitライブラリを使います。診断用のスクリーンショット読み出しはUSB専用で、通常の直接取得経路やネットワークから開始しません。

一次資料:

- [公式Go画面](https://opencode.ai/go)
- [pySerial公式リポジトリ](https://github.com/pyserial/pyserial)
- [pySerial公式ドキュメント](https://pyserial.readthedocs.io/en/latest/)
- [Adafruit ILI9341](https://github.com/adafruit/Adafruit_ILI9341)
