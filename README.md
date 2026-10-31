# SuperMini ESP32-S3 Internet Radio

ESP32-S3 SuperMini (`ESP32-S3FH4R2`: 4MB Flash / 2MB Quad PSRAM) と PCM5102A DAC、KCX_BT_EMITTER (Bluetooth 5.3) を組み合わせた、小型ヘッドレス・インターネットラジオ端末です。

---

## 1. 主な機能

- **Wi-Fi マルチ AP 自動接続 & 最強電波（RSSI）優先**:
  - 最大 5 件の AP を登録可能。起動時・再接続時に周囲をスキャンし、最も電波の強い登録済み AP に自動接続。
  - 未検出または接続失敗時は、即座にフォールバック用 SoftAP (`ESP32-InetRadio` / `192.168.4.1`) を起動。
- **高音質ストリーミング & MP3 / AAC デコード**:
  - PSRAM 上に 256KB のリングバッファを確保し、ジッターやパケット揺らぎによる音途切れを防止。
  - ICY メタデータ（曲名・アーティスト名）のリアルタイム抽出対応。
  - PCM5102A I2S 出力（内部 PLL 動作、MCLK 不要）。
  - 人間の聴覚特性に合わせた 2 乗カーブによるデジタル音量調整（0〜100%）。
- **Web UI & REST API & mDNS**:
  - スマートフォン・PC ブラウザからアクセス可能なレスポンシブ Web 設定画面。
  - `http://inetradio.local` または取得した IP アドレスでアクセス。
  - 周囲の Wi-Fi スキャン機能、AP 設定、リアルタイム音量スライダー、ワンクリック選局、局の追加・編集。
  - デフォルトで **SomaFM** 6 局（Groove Salad, Drone Zone, Secret Agent, DEF CON Radio, Lush, Indie Pop Rocks!）をプリセット。
- **本体操作**:
  - オンボード BOOT ボタン（`GPIO 0`）のシングルクリックで、登録されているラジオ局をループ巡回切替。
- **Bluetooth 送信（KCX_BT_EMITTER）連携**:
  - PCM5102A のアナログ出力を Bluetooth スピーカーやイヤホンへ自動送信。
  - 起動時の Wi-Fi スキャンを妨害しないよう、`GPIO 4` とロードスイッチ IC による連動電源制御に対応。

---

## 2. ハードウェア仕様

詳細は [HARDWARE.md](HARDWARE.md) を参照してください。

| 信号 | ESP32-S3 ピン | 接続先 | 役割 |
| :--- | :--- | :--- | :--- |
| **I2S BCLK** | `GPIO 1` | PCM5102A `BCK` | ビットクロック |
| **I2S WS** | `GPIO 2` | PCM5102A `LCK` | ワードセレクト（L/R クロック） |
| **I2S DOUT** | `GPIO 3` | PCM5102A `DIN` | シリアルオーディオデータ |
| **BT PWR EN** | `GPIO 4` | ロードスイッチ IC `EN` | KCX_BT_EMITTER 5V 給電制御（Wi-Fi 連動） |
| **BOOT BTN** | `GPIO 0` | オンボードボタン | 局ループ切替（Active LOW） |
| **POWER** | `5V`, `GND` | 全モジュール並列 | USB Type-C 5V 給電 |

---

## 3. 実機動作検証ステータス (2026/11/01 時点)

- [x] **Wi-Fi スキャン・接続**: `Karibacks` (RSSI -53dBm) への自動接続、DHCP IP 取得、mDNS 解決完了。
- [x] **オーディオ再生**: PCM5102A 3.5mm ジャックからのクリアな音声出力を確認。
- [x] **ストリーミング安定性**: PSRAM 256KB バッファ（99〜100% 維持）、MP3/AAC デコード、ICY 曲名抽出確認。
- [x] **Web 設定画面 & REST API**: ステータス取得、選局、音量調整、Wi-Fi スキャン API 正常動作。
- [x] **KCX 共存テスト**: ストリーミング再生中に KCX へ 5V を通電しても途切れないことを確認。
- [ ] **保留事項（ハードウェア部品調達待ち）**:
  - KCX への 5V 供給ラインに挟む **ロードスイッチ IC**（またはフォトリレー）の調達と `GPIO 4` 配線。
  - ※ ソフトウェア側の連動ロジックは実装済みです。

---

## 4. ビルドと書き込み

ESP-IDF v5.3 以降を使用します：

```bash
# 環境変数の読み込み
source ~/esp/esp-idf/export.sh

# ビルド
idf.py build

# フラッシュ書き込みとシリアルモニタ
idf.py -p /dev/ttyACM0 flash monitor
```
