# ryuw122

6.5GHz/8GHz帯UWB無線モジュール RYUW122 ファームウェア（ESP32-C6 / ESP-IDF）

REYAX RYUW122（秋月電子 通販コード 132078）を ESP32-C6 から UART の AT コマンドで
制御し、ANCHOR - TAG 間の測距（Two Way Ranging）と最大 12 バイトのデータ交換を行う
ファームウェアです。

- `components/ryuw122` … RYUW122 ドライバ（ESP-IDF コンポーネント）
- `main/app_main.c` … ANCHOR / TAG のアプリケーション
- `docs/AT_COMMANDS.md` … 使用している AT コマンドの一覧
- `test/host` … PC 上で動かせるパーサの単体テストとコンパイルチェック

## 必要なもの

- ESP32-C6 ボード（ESP32-C6-DevKitC-1 などを想定）× 2
- RYUW122 モジュール × 2（ANCHOR 用・TAG 用）
- ESP-IDF v5.1 以降（ESP32-C6 対応版）

測距には必ず 2 台必要です。片方を ANCHOR、もう片方を TAG に設定してください。

## 配線

RYUW122 は **3.3V 専用** です。5V を加えないでください。

| RYUW122 | ESP32-C6 | 備考 |
| --- | --- | --- |
| VCC | 3V3 | 3.3V 電源。UWB 送信時に電流が跳ねるため電源は余裕を持たせる |
| GND | GND | 共通グランド |
| TXD | GPIO11 | ESP32 の RX に入れる |
| RXD | GPIO10 | ESP32 の TX から出す |
| NRST | GPIO18 | 負論理リセット。省略可だが接続を強く推奨 |

GPIO の選定理由: ESP32-C6 では GPIO4/5/8/9/15 がストラッピングピン、GPIO12/13 が
USB-Serial/JTAG、GPIO16/17 が UART0（コンソール）、GPIO24〜30 が内蔵フラッシュに
割り当てられているため、これらを避けて GPIO10 / 11 / 18 を既定値にしています。
別のピンを使う場合は `idf.py menuconfig` → *RYUW122 UWB firmware* → *Wiring* で
変更できます。

NRST を接続しておくと、書き込み直後などにモジュールの状態が不定でも起動時に
ハードウェアリセットして復帰できます（`ryuw122_hw_reset()`）。

## ビルドと書き込み

```bash
idf.py set-target esp32c6
idf.py menuconfig          # "RYUW122 UWB firmware" で役割とピンを設定
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

### 1 台目（ANCHOR）

*Role of this node* → **ANCHOR**、*Address of this node* → `ANCHOR01`、
*Address of the tag to range against* → `TAGT0001`

### 2 台目（TAG）

*Role of this node* → **TAG**、*Address of this node* → `TAGT0001`

**ネットワーク ID（既定 `REYAX123`）、チャネル、帯域、暗号鍵は 2 台で必ず一致させて
ください。** 一致しないと測距要求に応答せず、ANCHOR 側はタイムアウトし続けます。

### 実行例（ANCHOR 側のログ）

```
I (312) uwb: RYUW122 UWB firmware, role ANCHOR
I (712) ryuw122: initialised on UART1 (tx=10 rx=11 rst=18, 115200 bps)
I (1020) uwb: firmware     : RYUW122_V1.0.0
I (1030) uwb: address      : ANCHOR01
I (1040) uwb: network id   : REYAX123
I (1050) uwb: mode         : ANCHOR
I (1060) uwb: channel      : 5 (6489.6 MHz)
I (1570) uwb: #0 distance 214 cm (2.14 m, avg 2.14 m), rssi -78 dBm, tag payload "ACK00001"
I (2070) uwb: #1 distance 219 cm (2.19 m, avg 2.16 m), rssi -79 dBm, tag payload "ACK00002"
```

## ドライバの使い方

```c
ryuw122_config_t config = RYUW122_DEFAULT_CONFIG();
config.tx_gpio = 10;
config.rx_gpio = 11;
config.rst_gpio = 18;

ryuw122_handle_t dev;
ESP_ERROR_CHECK(ryuw122_init(&config, &dev));
ESP_ERROR_CHECK(ryuw122_set_mode(dev, RYUW122_MODE_ANCHOR));
ESP_ERROR_CHECK(ryuw122_set_network_id(dev, "REYAX123"));
ESP_ERROR_CHECK(ryuw122_set_address(dev, "ANCHOR01"));

ryuw122_anchor_rcv_t result;
if (ryuw122_anchor_range(dev, "TAGT0001", "PING", 4, &result, 1000) == ESP_OK) {
    printf("%.2f m\n", ryuw122_cm_to_m(result.distance_cm));
}
```

- 受信タスクが UART を行単位で読み、`+ANCHOR_RCV=` / `+TAG_RCV=` の非同期通知と
  コマンド応答を分離します。通知は `ryuw122_wait_event()` で受け取るか、
  `ryuw122_set_event_cb()` でコールバック登録できます（コールバックは受信タスク上で
  呼ばれるのでブロックしないこと）。
- AT コマンドは再帰ミューテックスで直列化されるので、複数タスクから呼べます。
- 未対応のコマンドを直接叩きたい場合は `ryuw122_cmd()` を使います。

距離は cm 単位の整数で返ります。ANCHOR 側アプリでは指数移動平均（α=0.3）も併せて
表示しています。設置後の系統的な誤差は `AT+CAL`（*Radio* メニューの
*Apply a distance calibration offset*）で補正できます。

## テスト

ESP-IDF が無い環境でも、プロトコル解析部分の単体テストとコンパイルチェックを
実行できます。

```bash
./test/host/run_tests.sh          # パーサの単体テスト（ASan/UBSan 付き）
./test/host/run_compile_check.sh  # スタブヘッダでドライバ・アプリをコンパイル
```

## 検証状況

- パーサ（`ryuw122_parse.c`）は上記の単体テストで検証済み。ペイロードに `,` が
  含まれる場合や RSSI 無効時（`AT+RSSI=0`）の書式も含みます。
- ドライバとアプリはスタブヘッダでのコンパイルまで確認しています。
  **実機（ESP32-C6 + RYUW122）での動作確認は未実施です。**
- AT コマンドの書式は RYUW122 のデータシート（AT command set）に基づいています。
  既定値（ネットワーク ID、`AT+CAL` の既定値など）は個体・ファームウェア版により
  異なることがあるため、最初に `AT+VER?` と各 `?` 問い合わせで実機の値を確認する
  ことを推奨します。本ファームは起動時にそれらをログ出力します。

## ライセンス

MIT License
