# ryuw122

6.5GHz/8GHz帯UWB無線モジュール RYUW122 ファームウェア（ESP32-C6 / PlatformIO + ESP-IDF）

REYAX RYUW122（秋月電子 通販コード 132078）を ESP32-C6 から UART の AT コマンドで
制御し、ANCHOR - TAG 間の測距（Two Way Ranging）と最大 12 バイトのデータ交換を行う
ファームウェアです。

- `platformio.ini` … PlatformIO 設定（`anchor` / `tag` の 2 環境）
- `components/ryuw122` … RYUW122 ドライバ（ESP-IDF コンポーネント）
- `main/app_main.c` … ANCHOR / TAG のアプリケーション
- `configs/*.defaults` … 各環境の Kconfig 初期値
- `docs/AT_COMMANDS.md` … 使用している AT コマンドの一覧
- `test/host` … PC 上で動かせるパーサの単体テストとコンパイルチェック

## 必要なもの

- ESP32-C6 ボード（ESP32-C6-DevKitC-1 などを想定）× 2
- RYUW122 モジュール × 2（ANCHOR 用・TAG 用）
- PlatformIO Core 6.1.16 以降（VS Code 拡張でも可）

platform には ESP32-C6 + ESP-IDF 5.x に対応した
[pioarduino 版 platform-espressif32](https://github.com/pioarduino/platform-espressif32)
を使用しています（PlatformIO 公式の `espressif32` は C6 + ESP-IDF に未対応）。
ツールチェーンと ESP-IDF は初回ビルド時に自動で取得されるので、ESP-IDF を別途
インストールする必要はありません。

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
別のピンを使う場合は `pio run -e anchor -t menuconfig` → *RYUW122 UWB firmware*
→ *Wiring* で変更できます。

NRST を接続しておくと、書き込み直後などにモジュールの状態が不定でも起動時に
ハードウェアリセットして復帰できます（`ryuw122_hw_reset()`）。

## ビルドと書き込み

役割ごとに PlatformIO の環境を分けてあります。`anchor` と `tag` をそれぞれ別の
ボードに書き込んでください（測距には 2 台必要です）。

```bash
# 1 台目: ANCHOR
pio run -e anchor -t upload

# 2 台目: TAG
pio run -e tag -t upload

# ログを見る
pio device monitor -b 115200
```

設定（ピン配置、アドレス、チャネル、送信出力など）は menuconfig で変更できます。

```bash
pio run -e anchor -t menuconfig    # → "RYUW122 UWB firmware" メニュー
```

各環境の Kconfig 初期値は `configs/anchor.defaults` / `configs/tag.defaults` で、
そこから `sdkconfig.anchor` / `sdkconfig.tag` が生成されます（生成物は git 管理外）。
既定値を変えたいときは `configs/*.defaults` を編集してから
`rm sdkconfig.anchor && pio run -e anchor` としてください。

| 項目 | ANCHOR 既定値 | TAG 既定値 |
| --- | --- | --- |
| 役割 | ANCHOR | TAG |
| 自局アドレス | `ANCHOR01` | `TAGT0001` |
| 測距相手 | `TAGT0001` | － |
| ネットワーク ID | `REYAX123` | `REYAX123` |

**ネットワーク ID、チャネル、帯域、暗号鍵は 2 台で必ず一致させてください。**
一致しないと測距要求に応答せず、ANCHOR 側はタイムアウトし続けます。

### ESP-IDF 単体でビルドする場合

`main/` を標準のまま使っているので、PlatformIO を使わずに `idf.py`（v5.1 以降）で
ビルドすることもできます。この場合の Kconfig 初期値は `sdkconfig.defaults` です。

```bash
idf.py set-target esp32c6
idf.py menuconfig     # 役割を ANCHOR / TAG で切り替える
idf.py build flash monitor
```

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

ツールチェーンを取得しなくても、プロトコル解析部分の単体テストとコンパイル
チェックだけを PC 上で実行できます。

```bash
./test/host/run_tests.sh          # パーサの単体テスト（ASan/UBSan 付き）+ Kconfig 検証
./test/host/run_compile_check.sh  # スタブヘッダでドライバ・アプリをコンパイル
```

`run_tests.sh` は `kconfiglib`（`pip install kconfiglib`）があれば
`main/Kconfig.projbuild` の構文と `configs/*.defaults` の内容（役割が 1 つだけ
選ばれているか、存在しない CONFIG キーを書いていないか）も検証します。

## 検証状況

- パーサ（`ryuw122_parse.c`）は単体テストで検証済み。ペイロードに `,` が
  含まれる場合や RSSI 無効時（`AT+RSSI=0`）の書式も含みます。
- `main/Kconfig.projbuild` と `configs/*.defaults` は `test/host/check_kconfig.py`
  で検証済み（役割の排他選択、`app_main.c` が参照する CONFIG キーの実在確認）。
- ドライバとアプリはスタブヘッダでのコンパイルまで確認しています。
- `platformio.ini` は `pio project config` で `anchor` / `tag` 両 env の解決を確認
  済みですが、**`pio run` による実ビルドと実機動作の確認は未実施です**
  （作成環境から Espressif のツールチェーン配布サイトへ到達できなかったため）。
- AT コマンドの書式は RYUW122 のデータシート（AT command set）に基づいています。
  既定値（ネットワーク ID、`AT+CAL` の既定値など）は個体・ファームウェア版により
  異なることがあるため、最初に `AT+VER?` と各 `?` 問い合わせで実機の値を確認する
  ことを推奨します。本ファームは起動時にそれらをログ出力します。

## ライセンス

MIT License
