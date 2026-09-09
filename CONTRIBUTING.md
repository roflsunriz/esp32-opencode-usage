# 貢献ガイド

不具合報告、使用方法の相談、ドキュメント修正、ホスト側 TypeScript、ESP32 ファームウェアの改善を歓迎します。変更を提出する前に、対象の issue や Discussion がないか確認し、目的と影響範囲を短く説明してください。

## 開発環境

ホスト側は Windows と Bun、ファームウェア側は PlatformIO と `platformio.ini` の `esp32dev` 環境を基準にします。認証 Cookie、Bearer token、Wi-Fi パスワード、`.private/` のファイルはリポジトリへ追加しないでください。

```powershell
bun install --frozen-lockfile
bun run lint
bun run format:check
bun run type-check
bun run build
bun run test
bun run audit
pio run -e esp32dev
pio test -e native
```

実機を使う変更では、LCD表示、USBシリアル通信、通信失敗時の表示、再接続を確認し、確認できない項目と理由を Pull Request に書きます。認証や外部サービスを使うテストでは、実アカウントの秘密をログやテストデータへ残さず、可能な範囲でモックを使います。

## Pull Request

Pull Request には次を記載してください。

- 何を成立させる変更か、利用者から見た結果
- 変更したホスト、ファームウェア、設定、文書の範囲
- 実行した検証コマンドと結果。実機や外部通信を未検証ならその理由
- 互換性、認証情報、依存関係、ライセンスへの影響

小さく責務を分けた変更にし、フォーマットだけの差分を混ぜないでください。新しい依存関係を追加する場合は、[`docs/dependencies.md`](docs/dependencies.md) に採用理由、代替候補、ライセンス、保守状況を追記し、lockfileの差分を確認します。

## Issue と Discussion

不具合は再現手順、期待結果、実際の結果、環境、関連ログを添えて報告します。設定や表示を改善する提案には、用途と現在困っている点を添えます。Cookie、トークン、パスワード、Wi-Fi情報、個人情報は必ず伏せてください。
