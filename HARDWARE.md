# ハードウェア構成およびピンアサイン定義

本プロジェクトは以下のハードウェアコンポーネントを組み合わせて構成されています。

---

## 1. 使用コンポーネント一覧

1. **メインMCU**: ESP32-S3 SuperMini
   - チップ型番: ESP32-S3FH4R2（Flash 4MB / Quad PSRAM 2MB 内蔵）
   - 給電: オンボード USB Type-C コネクタ（5V）より給電
   - オンボードボタン: BOOT ボタン（タクトスイッチ / Active LOW, `GPIO 0`）
   - ディスプレイ: なし（ヘッドレス構成、Web UI から操作）

2. **DAC モジュール**: PCM5102A ミニ基板
   - 通信インターフェース: I2S デジタルオーディオ入力
   - 基板裏面ジャンパ設定:
     - `H1L (FLT)`  : `L`（Normal latency FIR フィルタ）
     - `H2L (DEMP)` : `L`（ディエンファシス OFF）
     - `H3L (XSMT)` : **`H`（ソフトミュート解除 / Unmute）**
       - ※注意: PCM5102A の仕様上、XSMT は Low でハードウェアミュート、High (3.3V) でアンミュートとなります。
     - `H4L (FMT)`  : `L`（標準 I2S フォーマット）
   - クロック設定: 内部 PLL 動作（SCK ピンは未接続・オープン）
   - 出力: アナログステレオライン出力（3.5mm ジャックおよび L/R/AGND 端子）

3. **Bluetooth 送信モジュール**: KCX_BT_EMITTER（Bluetooth 5.3 版）
   - 動作モード: Transmitter（送信モード / オンボード TX/RX ジャンパはオープンのまま）
   - 音声入力: アナログオーディオ入力（L / R / AGND）
   - 電源制御: ロードスイッチ IC（ハイサイドスイッチ）を介して `GPIO 4` で 5V 通電を制御（保留事項参照）

---

## 2. ピン結線・インターフェース定義

### (1) 電源ライン
- `ESP32-S3 SuperMini [5V]`  <--> `PCM5102A [VIN]` <--> `[ロードスイッチ IC VIN]`
- `[ロードスイッチ IC VOUT]` <--> `KCX_BT_EMITTER [Pin 1: +5V]`
- `ESP32-S3 SuperMini [GND]` <--> `PCM5102A [GND]` <--> `KCX_BT_EMITTER [Pin 2: PGND]`

### (2) I2S デジタルオーディオ（ESP32-S3 -> PCM5102A）
- `GPIO 1` : I2S Bit Clock (`BCLK` / `BCK`)
- `GPIO 2` : I2S Word Select / Left-Right Clock (`WS` / `LRCK` / `LCK`)
- `GPIO 3` : I2S Serial Data Out (`DOUT` / `DIN`)
- *(※ PCM5102A の `SCK` は内部 PLL 動作のため未接続)*

### (3) KCX 電源制御（ESP32-S3 -> ロードスイッチ IC）
- `GPIO 4` : ロードスイッチ IC の `EN`（Enable）ピンに接続
  - `HIGH (3.3V)` : KCX への 5V 給電 ON
  - `LOW (0V)`    : KCX への 5V 給電 OFF

### (4) アナログオーディオ（PCM5102A -> KCX_BT_EMITTER）
- `PCM5102A [L]`    <--> `KCX_BT_EMITTER [Pin 6: AUDIO_L]`
- `PCM5102A [R]`    <--> `KCX_BT_EMITTER [Pin 7: AUDIO_R]`
- `PCM5102A [AGND]` <--> `KCX_BT_EMITTER [Pin 8: AGND]`

### (5) ユーザー入力 / オンボードインターフェース
- `GPIO 0` : オンボード BOOT ボタン（押下時に LOW、シングルクリックで局巡回切替）

---

## 3. 実機検証実績と保留事項（KCX 電源スイッチング）

### (1) 検証実績（実機動作確認済み）
- **Wi-Fi AP スキャン・接続**: KCX 非通電時、`Karibacks` (RSSI -53dBm) に即座に接続し IP 取得、mDNS (`http://inetradio.local`) 解決を確認。
- **オーディオ再生**: PCM5102A の 3.5mm ジャックからの有線イヤホンにて、SomaFM（MP3 44.1kHz 2ch）のクリアな再生を確認。
- **ICY 曲名メタデータ抽出**: Web UI およびステータス API にリアルタイムに曲名が反映されることを確認。
- **共存確認**: Wi-Fi 接続完了（ストリーミング再生中）に KCX へ 5V を通電しても、**音楽再生が途切れることなく継続動作**することを確認。

### (2) 判明した課題（KCX による初期スキャン妨害）
- KCX_BT_EMITTER は電源投入直後、周囲の BT 受信機を探すため **`625 µs` 周期の強力な 2.4 GHz 送信バースト**を放射します。
- これが無シールドの SuperMini ESP32-S3 アンテナ直近にあると、ルーターから送られてくる **`3 ms` 近く連続する 1 Mbps DSSS ビーコンフレームが途中で破壊され、起動時の AP スキャンが 100% 失敗**します。
- 一方、接続後のストリーミングは **802.11n HT20（高速 OFDM、数十〜数百 µs の短フレーム ＋ MAC 層自動再送 ＋ PSRAM 256KB バッファ）** のため、KCX のバーストがあっても全く途切れずに共存できます。

### (3) 保留事項・今後の作業（部品調達待ち）
- **調達対象**: 5V ハイサイド用 **ロードスイッチ IC**（またはフォトリレー TLP222A 等）
  - 例: AP22802, AP2141, TPS22919, BD2222G 等（3.3V ロジック入力で 5V を ON/OFF できるもの）
- **配線作業**:
  - SuperMini の `5V` をロードスイッチ IC の入力（VIN）へ配線
  - ロードスイッチ IC の出力（VOUT）を KCX の `Pin 1: +5V` へ配線
  - SuperMini の `GPIO 4` をロードスイッチ IC の `EN` ピンへ配線
- **ソフトウェア側の対応**:
  - `main.c` にて、起動時・未接続時は `GPIO 4 = LOW`、Wi-Fi 接続完了時に `GPIO 4 = HIGH`、切断・再スキャン時に `GPIO 4 = LOW` とする制御ロジックは実装済みです。部品取り付け後にそのまま連動動作します。
