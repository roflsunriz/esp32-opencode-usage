# 調査知見

- 現行consoleは `/console/{workspace}/go`。使用量は `GET /console/api/go/status` + `x-org-id` で取得する。asset hashや旧query IDを固定しない。
- meterの金額はmicroCents（1 cent = 1,000,000）によるlimit換算値で請求額ではない。5h/weekは各 `resetsAt`、monthは `access.endsAt`。未使用5h枠の日時nullは0%・resetなしとして扱い、未知/欠損/errorを0%に偽装しない。
- 認証はHttpOnly `__Host-console_session`。旧 `auth` recordは401になるため起動時に破棄し、再設定へ戻す。秘密はGit管理外 `.private/` だけに置く。公式ログイン後の `/console/workspace` はIDではなく一覧なので、`wrk_` 形式の現在URLまたは一意なリンクだけを採用する。再ログインは分離FirefoxをMozilla公式の `Marionette:Quit` で終了してプロファイルロックを残さず、ESP32へ送る前に `firmware=opencode-go-lcd` と認証のみ更新対応の `setupSchema=4` を確認する。
- 全flash backupは読み出しと照合の間にresetするとNVS自己更新でdigestが変わる。`after no_reset` → `before no_reset` で同じ状態を照合し、port・容量・esptool majorを毎回実測する。
- WindowsのCH340でNode/Bun `serialport` はopen成功後のwriteが停止した。製品USBはPython/pySerial workerを使い、open前にDTR/RTSを落として意図しないreset/GPIO0操作を防ぐ。互換性はready uptimeでなくping応答で確認する。
- Preferencesの空文字保存は成功でも0を返す。設定はversion+CRCの単一blobへ保存し、完了後にcorrelation ID付きACKを返す。NVS schema 4とUSB `setupSchema=4` は同じ番号でも別管理であり、後者は認証だけを更新するフレームを含む。
- TLS trust bundleは公式GTS Root R4とGlobalSign Root CA。手転記せず生成scriptと `--check` でbyte一致を確認する。認証recordを含む大bufferをDRAM/loop stackへ複製せず、64KiB応答をstream走査する。
- XPT2046のPENIRQはGPIO36 LOW。内部pull-upを使わず、PD0=0を維持する。基板ごとにaxis配線が異なるため3点校正で方向とthresholdをNVSへ保存し、固定座標を一般化しない。
- LCDは320×240 8-bit Spriteの完成frameを16行帯でdiff転送する。rotation/calibration後は全帯、Sprite確保失敗時だけ直接描画。旧経路の実機結果を新経路の証拠にしない。
- 使用量JSONのサイズを固定しない。未使用枠のnull日時をinvalidと誤判定した実例がある。401/404/too-large/short body/invalid JSONを分離し、診断へ本文やCookieを含めない。
- ローカルとReleaseのfirmwareは同sourceでもbyte不一致になった。ローカル実機検証を公開BINへ流用せず、release ZIPのBINをapp領域へ書いて設定保持とdigestを再確認する。
- Dependabotがlockfile v2を読めない間は製品実行Bun 1.4系とlock生成Bun 1.3.14を分ける。version番号だけ手編集せず、parser対応変更時に生成script・lock・互換testを一緒に更新する。
- Displayタブの時・分は2列の縦ドラムで選び、取得間隔だけ横スライダーを維持する。使用量変化点灯は初回取得を除外し、既存の有効な期間値が変わった時だけ10分タイマーを開始する。即時消灯は保存設定を変えず、同じ接触が復帰へ流れない入力ラッチを使う。
