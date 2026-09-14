# 依存関係の採用記録

確認日: 2026-09-14

正確な解決バージョンは `package.json`、`bun.lock`、`requirements*.txt`、`platformio.ini` を正本とします。ここでは採用理由、ライセンス、保守状況、代替候補を記録します。

## ホスト側

| 依存関係                                                                                                                                                                    | 宣言                                                                 | 採用理由・保守状況                                                                                                                                                                                            | ライセンス                  |
| --------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------- |
| [Bun](https://bun.sh/)                                                                                                                                                      | `engines.bun >=1.4`                                                  | WindowsでTypeScriptの起動、ビルド、テスト、依存管理を行う。製品USBはBunのnative addonを使わずPython workerへ委譲する。                                                                                        | MIT                         |
| [TypeScript](https://github.com/microsoft/TypeScript)                                                                                                                       | `^5.9.0`                                                             | ホストのAPI応答、認証状態、設定フレームを静的型で扱う。                                                                                                                                                       | Apache-2.0                  |
| [ESLint](https://github.com/eslint/eslint)、[`@eslint/js`](https://github.com/eslint/eslint)、[`typescript-eslint`](https://github.com/typescript-eslint/typescript-eslint) | ESLint `^10.10.0`、`@eslint/js ^10.0.1`、`typescript-eslint ^8.46.0` | ESLint 9.xのEOLを避け、現行10.xで型付き解析を行う。                                                                                                                                                           | MIT                         |
| [Prettier](https://github.com/prettier/prettier)                                                                                                                            | `^3.6.0`                                                             | TypeScript、設定、ドキュメントの整形を統一する。                                                                                                                                                              | MIT                         |
| [pySerial](https://github.com/pyserial/pyserial)                                                                                                                            | `requirements.txt` の `3.5`                                          | `host/serial-worker.py` の実USB通信を担当する。CH340実機でread/writeと切断を確認でき、Bun/Nodeのserialport直接経路より安定したため採用した。Windows、macOS、Linuxに対応する成熟した純Pythonライブラリである。 | BSD-3-Clause                |
| [mypy](https://github.com/python/mypy)                                                                                                                                      | `requirements-dev.txt` の `2.3.1`                                    | workerを `--strict` で検査する開発用ツール。                                                                                                                                                                  | MIT                         |
| [types-pyserial](https://github.com/python/typeshed)                                                                                                                        | `requirements-dev.txt` の `3.5.0.20260712`                           | pySerialの型検査を補う開発用stub。                                                                                                                                                                            | MIT相当のtypeshedライセンス |

pySerialの公式GitHubで公開されている最新タグ表示はv3.5で、PyPIの3.5は2020-11-23公開です。上流の開発ブランチではPython 3.10以上への対応が示されているため、Python 3.12以上を前提にし、3.5の固定は実機で安定した現在の解決値として維持します。上流の新しい安定版が公開された場合は、CH340の連続read/write、切断、再接続を再検証してから更新します。

`serialport@13.0.0` は `package.json` から削除しました。公式のMITライブラリで成熟していますが、この実機ではBun実行時とNode.js単独実行時のCH340 write停止を確認したため、製品経路へ採用しません。`@serialport/bindings-cpp`、Windows API直接呼び出し、Web Serial APIへ切り替えるより、pySerial workerで実測済みの挙動とPython標準の保守性を優先します。

Node.js 24は開発用fake fixtureテストだけで使います。製品のホスト起動とUSB通信の実行時依存ではありません。

## ファームウェアと書き込み

| 依存関係                                                                    | 宣言                                    | 採用理由・保守状況                                                            | ライセンス           |
| --------------------------------------------------------------------------- | --------------------------------------- | ----------------------------------------------------------------------------- | -------------------- |
| [PlatformIO Core](https://github.com/platformio/platformio-core)            | `requirements-dev.txt` の `6.1.19`      | ESP32ビルド、nativeテスト、依存ライブラリ解決を `platformio.ini` で再現する。 | Apache-2.0           |
| [Espressif 32 platform](https://github.com/platformio/platform-espressif32) | `platformio.ini` の `espressif32@7.1.2` | ESP32ボード定義、Arduino framework、ツールチェーンを固定する。                | Apache-2.0           |
| [arduino-esp32](https://github.com/espressif/arduino-esp32)                 | PlatformIO経由                          | Wi-Fi、NVS、SPI、TLS、時刻同期などESP32実装の基盤。                           | LGPL-2.1             |
| [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI)                              | `2.5.43`                                | 通知版と同じILI9341描画に統一し、8-bit Spriteから変化した16行帯だけ転送する。旧Adafruit GFX/ILI9341を置き換えた。 | MIT/BSD/FreeBSD（同梱license.txt） |
| [XPT2046_Touchscreen由来のドライバー](https://github.com/PaulStoffregen/XPT2046_Touchscreen) | `lib/sensitive-xpt2046` | 通知版と同じ押圧値取得とPENIRQ復帰を使い、軽いタッチペンの2点調整に対応する。 | MIT（原著作権表示を保持） |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson)                     | `7.4.3`                                 | USB設定フレーム、使用量フレーム、ACKを型とサイズを確認しながら扱う。          | MIT                  |
| [esptool](https://github.com/espressif/esptool)                             | `requirements-dev.txt` の `5.4.0`       | 全フラッシュの退避、SHA-256照合、書き戻し、検証に使う。                       | GPL-2.0-or-later     |
| [Pillow](https://github.com/python-pillow/Pillow)                           | `requirements-dev.txt` の `12.3.0`      | USB診断スクリーンショットのRGBデータを検証する開発用ツール。                  | HPND / PILライセンス |

PlatformIOの解決結果は `platformio.ini` の固定値を基準に更新します。ESP32のHTTPS、Google Trust Services CA、NTPの実装が変更された場合は、CA期限、時刻未同期、再起動後の直接取得を実機で確認してから案内を更新します。

2026-09-14時点で[TFT_eSPIの公開Security Advisories](https://github.com/Bodmer/TFT_eSPI/security/advisories)は0件と確認した。旧Adafruit構成は通知版と異なる描画・入力方式だったため置き換えた。8-bit Spriteは旧16-bit全画面バッファより少ない約75KiBだが、TLS併用時の実機メモリと画面のちらつきは未検証である。

## 更新と監査

初回の実行時依存関係:

```powershell
python -m pip install -r requirements.txt
bun install --frozen-lockfile
```

ファームウェアや開発検証を行う場合:

```powershell
python -m pip install -r requirements-dev.txt
```

更新後の検証:

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

## 秘密情報とライセンス

ESP32のNVSにはWi-FiパスワードとOpenCode認証Cookieが残ります。通常のESP32フラッシュは暗号化されていないため、全フラッシュバックアップと実機を秘密情報として管理します。PC側のCookieはWindows DPAPIで保護され、`.private/` はGit管理外です。

XPT2046_Touchscreen由来の調整済みドライバーを`lib/sensitive-xpt2046`へ同梱し、元のMIT著作権表示をソース先頭へ保持しています。配布時は各上流ライセンス、著作権表示、esptoolのGPL条件、ESP32 Arduino coreのLGPL条件を確認します。

## 公式一次資料

- [pySerial公式リポジトリ](https://github.com/pyserial/pyserial)
- [pySerialリリース](https://github.com/pyserial/pyserial/releases)
- [pySerial PyPI metadata](https://pypi.org/project/pyserial/)
- [pySerial公式ドキュメント](https://pyserial.readthedocs.io/en/latest/)
- [esptool公式リポジトリ](https://github.com/espressif/esptool)
- [PlatformIO Core](https://github.com/platformio/platformio-core)
- [Arduino ESP32](https://github.com/espressif/arduino-esp32)
- [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI)
- [XPT2046_Touchscreen](https://github.com/PaulStoffregen/XPT2046_Touchscreen)
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson)
