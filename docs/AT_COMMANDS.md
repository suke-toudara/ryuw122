# RYUW122 AT コマンド メモ

本ファームが使用する REYAX RYUW122 の AT コマンドをまとめたものです。
コマンドは末尾に `\r\n` を付けて送信します（UART 既定値 115200 bps / 8-N-1）。

応答は次の 3 種類です。

| 応答 | 意味 |
| --- | --- |
| `+OK` | 設定コマンド成功 |
| `+<名前>=<値>` | 問い合わせコマンド (`?`) の応答 |
| `+ERR=<n>` | エラー |

`+ERR=<n>` の内容:

| n | 意味 |
| --- | --- |
| 1 | `<CR><LF>` が無い |
| 2 | コマンドが `AT` で始まっていない |
| 3 | パラメータ異常 |
| 4 | コマンド実行失敗 |
| 5 | 未知のコマンド |

## 設定コマンド

| コマンド | 説明 | ドライバ API |
| --- | --- | --- |
| `AT` | 疎通確認（`+OK`） | `ryuw122_test()` |
| `AT+MODE=<0/1/2>` | 0=TAG, 1=ANCHOR, 2=SLEEP | `ryuw122_set_mode()` |
| `AT+NETWORKID=<8文字>` | ネットワークグループ。通信する両者で一致させる | `ryuw122_set_network_id()` |
| `AT+ADDRESS=<8文字>` | 自局アドレス（ASCII 8 バイト） | `ryuw122_set_address()` |
| `AT+UID?` | モジュール固有 ID | `ryuw122_get_uid()` |
| `AT+CPIN=<16進32文字>` | AES128 暗号鍵。両者で一致させる | `ryuw122_set_password()` |
| `AT+CHANNEL=<5/9>` | 5 = 6489.6 MHz, 9 = 7987.2 MHz | `ryuw122_set_channel()` |
| `AT+BANDWIDTH=<0/1>` | 0 = 850 kbps, 1 = 6.8 Mbps | `ryuw122_set_bandwidth()` |
| `AT+CRFOP=<0..5>` | 送信出力 0=-65 dBm, 1=-50, 2=-45, 3=-40, 4=-35, 5=-32 dBm | `ryuw122_set_rf_power()` |
| `AT+RSSI=<0/1>` | 受信通知に RSSI を付加するか | `ryuw122_set_rssi_report()` |
| `AT+TAGD=<on>,<off>` | TAG の RF デューティ（各 10〜28000 ms） | `ryuw122_set_tag_duty_cycle()` |
| `AT+CAL=<値>` | 距離補正値 | `ryuw122_set_calibration()` |
| `AT+IPR=<9600/57600/115200>` | UART ボーレート変更 | `ryuw122_set_baud_rate()` |
| `AT+VER?` | ファームウェアバージョン | `ryuw122_get_version()` |
| `AT+RESET` | ソフトリセット（`+RESET`） | `ryuw122_soft_reset()` |
| `AT+FACTORY` | 工場出荷設定に戻す（`+FACTORY`） | `ryuw122_factory_reset()` |

各設定コマンドは末尾に `?` を付けると現在値を読み出せます（例 `AT+MODE?` → `+MODE=1`）。

## 測距・データ伝送

### ANCHOR 側

```
AT+ANCHOR_SEND=<TAGアドレス>,<ペイロード長>,<ペイロード>
```

`+OK` の後、TAG が応答すると次の通知が上がります。

```
+ANCHOR_RCV=<TAGアドレス>,<ペイロード長>,<TAGのデータ>,<距離[cm]>,<RSSI>
```

`<RSSI>` は `AT+RSSI=1` のときのみ付きます。TAG が圏外・電源断・ネットワーク ID
不一致の場合は通知が来ません（ドライバは `ESP_ERR_TIMEOUT` を返します）。

### TAG 側

```
AT+TAG_SEND=<ペイロード長>,<ペイロード>
```

次に ANCHOR から要求が来たときに返すデータを 1 回分だけ登録します。
ANCHOR から要求を受けると TAG 側には次の通知が上がります。

```
+TAG_RCV=<ペイロード長>,<ANCHORのデータ>,<RSSI>
```

応答データは 1 回の交信で消費されるため、`+TAG_RCV=` を受けるたびに
`AT+TAG_SEND` を送り直す必要があります（本ファームの `tag_loop()` がこれを行います）。

### 制限

- ペイロードは最大 12 バイト。
- AT コマンドはテキスト行なので、制御文字（`\r` `\n` を含む）は送れません。
  ドライバは印字可能 ASCII 以外を検出すると `ESP_ERR_INVALID_ARG` を返すため、
  バイナリを送る場合は 16 進などにエンコードしてください。
- `,` はペイロード長が先に送られるため通過しますが、区切り文字と紛らわしいので
  避けた方が無難です（受信側パーサは長さ優先で復元します）。
- アドレス / ネットワーク ID はどちらも ASCII 8 文字ちょうどです。
