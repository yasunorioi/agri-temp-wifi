# agri-temp-wifi

**DS18B20 × N**（1-Wire マルチドロップ）の多点温度ノード。
`agri-*` ファミリーの **WiFi 機**。
**MQTT トピックに既定値は無い**（v0.3.3〜）。設置時に必ず命名する — 理由は下記「初回セットアップ」。

**対応ボードは2種類**。差分は `src/board.h` と `platformio.ini` の `-D` フラグだけに
閉じてあり、`sensors` / `mqtt_pub` / `ccm_pub` / `webui` / `self_update` はボード非依存。

| env | ボード | MCU | Flash | 1-Wire 既定 | 状態 |
|---|---|---|---|---|---|
| **`m5atoms3-wifi`**（既定） | [AtomS3 Lite](https://docs.m5stack.com/en/core/AtomS3%20Lite) | ESP32-S3 | 8MB | **G1**（Grove 白） | 実機稼働 |
| `m5atomu-wifi` | [ATOM U](https://docs.m5stack.com/en/core/ATOM%20U) | ESP32-PICO-D4 | 4MB | G25（手配線） | 棚上げ（配線待ち） |

| | ATOM U | AtomS3 Lite |
|---|---|---|
| ステータス LED | SK6812 on G27（`M5.dis`） | WS2812C on **G35**（FastLED 直叩き） |
| ボタン | G39（`M5.Btn`） | **G41**（active-low を直読み） |
| USB | USB-A 直挿し（書込は外付け FTDI） | **USB-C ネイティブ CDC**（変換チップ無し） |
| ボードライブラリ | `m5stack/M5Atom` | 無し（S3 非対応なので意図的に外してある） |

前身: `Documents/Arduino/M5Atom-ds18b20_influxdb/M5Atom-ds18b20_influxdb.ino`
（OneWire を手書きデコード＋WiFi ハードコード＋InfluxDB UDP 直投げ）。
`MCP3004.ino → agri-amp-wifi` と同じ「旧スケッチを agri 流儀に書き換える」系譜。

---

## なぜ PoE ではないのか

ATOM U は底面拡張が無く PoE ベースを履けない。よって **`agri-node-poe-core` は使えない**
（あれは W5500/ETH 前提で、`ETH.localIP()` や `Network*` クラスに依存している）。
`agri-amp-wifi` と同じく MQTT / UECS-CCM / WebUI を `src/` 内に自前実装している。
見た目・`/api/status` スキーマ・HTTP OTA はファミリーに揃えてあるので、
運用上は他ノードと同じように扱える。

| | PoE 機 (env/drain/canopy…) | この機 |
|---|---|---|
| 基盤 | `agri-node-poe-core` | 自前 (`src/*.h`) |
| 回線 | W5500 (ETH) | WiFi (WiFiManager) |
| 設定 | core の `/config` | `webui.h` の `/config` |
| OTA | core `/api/ota` (raw body) | **multipart** `/api/ota` |
| セルフ更新 | core `AgriOTA.h` | `self_update.h`（同API面で自前移植） |

> **OTA の形式が違う点に注意**: PoE 機は raw body、この機は `curl -F firmware=@...`。
> `agri-display-atom` と同じ形式。

---

## 配線

### AtomS3 Lite（Grove ユニット・推奨）

Grove DS18B20 ユニット（スイッチサイエンス 10979 / 防水プローブ 2m）は
**プルアップ内蔵**なので、Grove ポートに挿すだけで終わり。

```
AtomS3 Lite  Grove (HY2.0-4P)
  G1  (白) ──── DQ      ← DATA はこちら
  G2  (黄) ──── 未使用
  5V  (赤) ──── VCC
  GND (黒) ──── GND
```

- **DATA = G1**。シルクの `SIG`/黄 = G2 に見えるが **G2 では何も読めない**。
  これは推測ではなく `agri-temp-poe`（AtomS3 Lite + PoE ベース、現場3台）で
  実機確認済みの事実。
- 多点化は同じバスに DQ / VCC / GND を並列（マルチドロップ）。
- 裸の DS18B20 を直結するなら下の ATOM U と同じく 4.7k プルアップが要る。
  その場合は **3.3V 給電＋3.3V プルアップ**に揃えること（理由は下記）。

### ATOM U（裸プローブを手配線）

```
ATOM U            DS18B20 (パラサイト給電は非推奨・3線で使う)
  G25  ──┬────────  DQ   (黄)
         │
        4.7k
         │
  3.3V ──┴────────  VDD  (赤)
  GND  ──────────── GND  (黒)
```

- **DATA = G25**（旧スケッチと同じピン。`/config` で変更可、変更時は自動再起動）
- **4.7k プルアップは必須**。無いとバスが浮いて `probes=0` になる。
- **プルアップと VDD は同じレールに繋ぐこと**。DS18B20 を 5V で駆動して
  プルアップも 5V にすると **G25 に 5V が乗って ESP32 の絶対最大定格を超える**。
  さらに 5V 駆動時の DS18B20 の V<sub>IH</sub> は 0.7×VDD = 3.5V なので、
  「5V 給電＋3.3V プルアップ」も規格外（動くこともあるが不安定）。
  **3.3V 給電＋3.3V プルアップに揃えるのが正解。**
- 複数個は DQ / VDD / GND をそのまま並列（マルチドロップ）。長尺・多点なら
  プルアップを 2.2k 程度まで下げ、分岐はスター配線を避ける。

---

## スロットモデル（ここが肝）

1-Wire は列挙順＝ROM アドレス順で、**配線した順ではない**。
添字で対応付けると、センサーを1本交換しただけで全系列の意味が入れ替わる。

そこで **ROM アドレス → スロット**を config で束縛し、
**スロット**が MQTT トピック / UECS 型 / 校正オフセットを持つ。

- ROM 空 = そのスロットは無効
- Topic 空 = MQTT に出さない
- CCM識別子 空 = そのスロットは CCM を出さない（env v0.10.0 の流儀）

初回起動時、どのスロットにも ROM が入っていなければ **バス順に自動割当**して NVS に保存する。
順序は任意なので、**1本を手で握って Dashboard のどれが上がるかで実体を確認**してから
ラベル／トピックを付けること。

Config ページの ROM 欄はバス上で見つかった ROM の `<select>`（現在温度付き）なので、
16桁の hex を手打ちする必要はない。**バス上にあるがどのスロットにも属さない ROM は
Dashboard に「Unassigned probes」として出る** ＝ センサーを足したらすぐ気づける。

---

## MQTT

スロットごとに **1物理量1トピック**、`retain`。`agri-env-poe` と同じ正準形:

```
agriha/farm/sensor/WaterTempTank     {"value":24.12,"unit":"C","ts":1789881535}
agriha/farm/sys/temp_tank_01/online  1 / 0  (LWT, retain)
```

現用ノード（No.2/No.3 共用の給水タンク水温計）の実際の出力。
トピックの決め方は正準仕様書
[`Arsprout-RESTAPI/mqtt-topics.md`](https://github.com/yasunorioi/Arsprout-RESTAPI/blob/main/mqtt-topics.md)
§0.3.1（限定子付き型名）と §0.6（ライフサイクル）に従う。

2本目以降の `/N` サフィックスは、このブローカーで同一型が複数ある時に既に使われている
慣行（`agriha/2/actuator/Relay/2`、`agriha/1/actuator/VenSdWinopr/2`）に合わせたもの。

`ts` は SNTP 同期後の実 epoch、未同期なら `0`。

> **blob を出さないのは意図的**。yasu-hp の logger は JSON blob を
> `<topic>#field` という series に割って保存するので、`agriha/1/sensor/Drain#drainage_ml`
> のような扱いにくい系列が増える。値1個のトピックなら `series.key` がそのまま意味になる。

## UECS-CCM（任意・既定 OFF）

スロットごとに **1パケット1 `<DATA>`**、ブロードキャスト + マルチキャスト両送出。
（ArSprout は複数 DATA の最後しか取らず、かつ 255.255.255.255 でしか受けない。
`agri-flow` / `agri-amp` が数週間ハマった罠。詳細は `agri-node-poe-core/docs/ccm_pub.md`）

> **⚠️ CCM を有効にしても、それだけでは agriha には出ない。**
> yasu-hp の `~/ccm-mqtt-bridge/config.site.yaml` は **送信元 IP ごとの
> `sender_override`** でハウスを決めており、**未登録の IP は自動 drop** される。
> このノードの CCM を agriha に乗せたいなら bridge に1行足すこと。
> 逆に言えば、**MQTT ネイティブ publish だけで完結するので CCM は基本 OFF のままでよい**
> （ArSprout 側に温度を渡したい場合だけ ON）。
> 既定 region=13 は暫定値。`.165` の region 取り違えで別ハウスの series を誤生成した
> 事故があるので、ON にする前に必ず突き合わせる。

---

## ビルド / 書き込み

`pio` を PATH に通せば **Windows / Linux 共通**（`platformio.ini` は OS 非依存・
`upload_port` 未指定＝自動検出。ATOM U は FTDI なので Win=`COMx` / Linux=`/dev/ttyUSB*`、
AtomS3 Lite はネイティブ USB CDC なので Win=`COMx` / Linux=`/dev/ttyACM*`）。
`default_envs = m5atoms3-wifi` なので、env を省くと AtomS3 が対象になる:

```bash
pio run                          # = -e m5atoms3-wifi
pio run -t upload

pio run -e m5atomu-wifi -t upload
```

> 🛠 **ビルド環境（Windows / Linux 共用）・Linux 初回セットアップ（udev 等）** →
> [agri-node-poe-core/docs/cross-platform-build.md](https://github.com/yasunorioi/agri-node-poe-core/blob/main/docs/cross-platform-build.md)

- 実測 — AtomS3: RAM 18.2% / **Flash 44.4%**（3.34MB スロット）／
  ATOM U: RAM 16.8% / Flash 60.3%（1.87MB スロット、`min_spiffs.csv`）。

### AtomS3 Lite 固有

- **パーティション指定は無し**（ボード既定の `default_8MB.csv`）。8MB あるので
  OTA スロットが 2面 × 3.3MB 取れ、ATOM U の約3倍の余裕がある。
  `min_spiffs` を被せる必要は無い。
- `-DARDUINO_USB_CDC_ON_BOOT=1`。USB-UART 変換チップが無く、`Serial` は
  ESP32-S3 のネイティブ USB CDC そのもの。`upload_speed` はボード既定の 460800
  （CDC なのでボーレートは実質飾り。ATOM U の 115200 縛りとは無関係）。
- **シリアルモニタの罠**: USB-Serial-JTAG では **RTS が EN、DTR が GPIO0** に
  繋がっている。pyserial や一部のターミナルは open 時に両方を assert するので、
  **チップがリセット状態で保持されて一切出力が出ない**。生の pyserial で覗くなら
  open 直後に `setRTS(False); setDTR(False)` すること。さらに **RTS でリセットを
  打つと S3 自身の USB が落ちて既存ハンドルが死ぬ**（例外も出ず 0 バイトになる）。
  そもそも状態は WebUI（`/api/status`）で全部見えるので、普段シリアルは要らない。

### ATOM U 固有

- **`upload_speed = 115200`**。この FTDI FT232R は 921600 でも 460800 でも
  ボーレート切替直後に `Unable to verify flash chip connection (No serial data received)`
  で落ちる。115200 なら確実（1MB で約 90 秒）。
- **パーティションは `min_spiffs.csv`**。セルフ更新が mbedTLS を抱き込むので、
  既定のパーティションだと Flash 90.4% まで埋まって余地が無くなる。
  min_spiffs は OTA スロット2面（セルフ更新に必須）を保ったまま各面を 1.9MB に広げる。
  この機は SPIFFS を使わないので損はない。
  > **パーティションテーブルはアプリイメージの外にあるので OTA では変えられない。**
  > 既に旧レイアウトで動いているノードを移すときは USB で1回焼く必要がある。以後は OTA でよい。
- USB 書き込みは初回だけ。以後は HTTP OTA（Win は `curl.exe`）:
  ```bash
  curl -F firmware=@.pio/build/m5atoms3-wifi/firmware.bin http://agri-temp-01.local/api/ota
  ```
  > **env を間違えないこと。** ESP32-S3 と ESP32-PICO-D4 はアーキテクチャが違うので、
  > 相手違いの bin を送ってもイメージヘッダ検証で弾かれる（＝無駄足になる）。

## 初回セットアップ

1. 焼く → 起動すると WiFi 未設定なので AP **`agri-temp-setup`** が立つ（192.168.4.1）
   - 起動時にボタンを押しっぱなしにすると、設定済みでも強制的にポータルを開ける
     （AtomS3 Lite は G41 = 天面の丸ボタン、ATOM U は G39）
   - ポータルは 180 秒でタイムアウトして再起動する。取り逃すと AP が一瞬消えるが、
     すぐ立ち上がり直すので繋ぎ直せばよい
   - **NVS に前の用途の WiFi 資格情報が残っていると、ポータルを無視して勝手に
     そちらへ繋がる**ことがある（AtomS3 実機で実際に起きた）
2. スマホ/PC で繋いで現地 WiFi を設定
3. `http://agri-temp-01.local/` を開く
4. `/config` で **Node ID** / **Hostname** / MQTT Host（`yasu-hp.local`、既定で入っている）と
   スロットを設定
   - **Node ID はノードごとに必ず変える**（既定は `temp_node_01`）。これは MQTT の client id
     そのものなので、2台が同じ id で繋ぐとブローカが先の接続を蹴り、両方が接続と切断を
     延々と繰り返す。LWT トピック `<prefix>/sys/<Node ID>/online` も決める。15 文字まで。
     **再起動不要**（保存時に MQTT を張り直す）
   - **Hostname は再起動するまで mDNS / ArduinoOTA に反映されない**。`/config` が自動再起動
     するのは DATA pin を変えたときだけなので、変えたら Config ページの **Reboot** ボタン
     （または `curl -X POST http://<host>/api/reboot`）を押す
   - **スロットの topic を入れるまで MQTT には何も出ない**（v0.3.3〜、既定は空）。
     Dashboard に温度は出るが「not set / nothing is being published」と警告が出る。
     これは故障ではなく、名前を決めるまで publish しない設計
   - 名前は**型名の末尾に短い物理的 descriptor**を付けて区別する。
     `WaterTempTank` のように。既存: `WaterTempTap` / `WaterTempNear` /
     `WaterTempPump` / `WaterTempFar`。**素の `WaterTemp` と `/2` のような番号は使わない**
     （v0.3.2 以前は素の `WaterTemp` が既定だったため、全ノードが同じ名前に着地して
     互いの履歴系列に書き込む事故が起きた。2026-09-20 に 566 サンプルを手で削除している）
   - **topic を変えたら旧 topic の retain を消す**: `mosquitto_pub -t <旧topic> -r -n`。
     消さないと固まった値が永久に残り、消費側からは生きているように見える。
     **`sys_prefix` を変えても slot topic は追従しない**（別フィールド）ので両方直すこと
5. Dashboard に温度が出る → broker で設定したトピックを確認

## API

| | |
|---|---|
| `GET /api/status` | `fw_name`/`fw_version`/`ip`/`link`/`mqtt_connected`/`ccm_enabled`/`ow_pin`/`probe_count`/`probes[]`/`slots[]` |
| `GET /api/config` | 設定 JSON |
| `POST /api/scan` | 1-Wire バス再列挙 |
| `POST /api/reboot` | 再起動（Hostname 変更を mDNS に反映させるときに使う。GET は不可） |
| `POST /api/check` | GitHub Release を今すぐ確認 |
| `POST /api/update` | セルフ更新を予約（次の `poll()` で焼く。更新が無ければ 409） |
| `POST /api/ota` | multipart ファーム更新（手動） |

`/config` への POST は**部分 POST が安全**（無いキーは現状維持）。
チェックボックスは `ccmform` というマーカー隠しフィールドとセットで判定するので、
`curl -d "mqhost=..."` を投げても **CCM が勝手に off に落ちない**。
これは core WebUI の既知の罠（`.27` と `.165` で2回踏んだ）を、この機では塞いだもの。

## トラブルシュート

**`probes=0`** — 設定ピンで0本のとき、起動時に安全な候補 GPIO
（26/32/25/33/21/22/19/23。PICO-D4 の内蔵フラッシュ 6-11・16/17 と LED の 27 は除外）を
自動スキャンしてシリアルに出す:

```
[1W] pin=G25 res=12 bit  probes=0
[1W] no device on the configured pin — scanning candidates
[1W]   G26 : 0 device(s)
[1W]   G25 : 2 device(s)   <-- set DATA pin to this in /config
```

**全ピンが 0** なら、ピンの問題ではなく**電気的にバスが死んでいる**:
給電（VDD/GND）が来ていない / 4.7k プルアップが無い / センサーが繋がっていない。

---

## セルフ更新（GitHub Release）

`src/self_update.h` は core の `AgriOTA.h` の移植。**名前空間も関数名も同じ**
（`agri::OTA::begin` / `checkLatest` / `schedule` / `poll`）なので、
この機を将来 3.x に上げたらこのファイルを消して `#include <AgriOTA.h>` に差し替えるだけで済む。
core が使えないのは、あれが 3.x の `NetworkClientSecure` と ETH 前提の core に依存しているため
（`agri-display-atom` も同じ理由で同じ移植を持っている）。

**半自動＝勝手には焼かない**。起動時に1回＋24時間ごとに Release を見に行き、
新しければ Dashboard に黄色いバナーと「Update now」ボタンを出す。押すと `POST /api/update` で
予約され、次の `poll()` で焼いて再起動する。

リリース手順（**タグと asset 名が規約どおりでないと、タグは見つかるのに download で 404 する**）:

```bash
pio run -e m5atoms3-wifi
pio run -e m5atomu-wifi

# asset 名 = ファイル名。いったん規約名にコピーしてから上げる
cp .pio/build/m5atoms3-wifi/firmware.bin agri-temp-wifi-atoms3.bin
cp .pio/build/m5atomu-wifi/firmware.bin  agri-temp-wifi.bin
gh release create v0.3.0 agri-temp-wifi-atoms3.bin agri-temp-wifi.bin \
  --target main --title v0.3.0 --notes "..."
```

> ⚠️ **`gh release create "path#name"` では asset 名は変わらない。** `#` の後ろは
> *label*（表示名）にしかならず、実体は `name=firmware.bin` のまま上がる。
> 2 ボード分を同時に上げるとその `firmware.bin` が衝突して
> `HTTP 404` でアップロードごと失敗する（v0.3.0 で実際に踏んだ）。
> **必ず規約名に `cp` してから渡すこと。**
> なお `--target` はブランチ名（`main`）を渡す。短縮 SHA は
> `Release.target_commitish is invalid` で弾かれる。

- タグ = `v` + `FW_VERSION`（`main.cpp`）
- asset 名 = `FW_BIN_NAME` と完全一致。**v0.3.0 からボードごとに別名**で、
  `platformio.ini` の `-DFW_BIN_NAME_STR` が決める:

  | env | asset 名 |
  |---|---|
  | `m5atoms3-wifi` | `agri-temp-wifi-atoms3.bin` |
  | `m5atomu-wifi`  | `agri-temp-wifi.bin` |

  同じ release に両方の bin を上げておけば、各ノードは自分の分だけを拾う。
  名前を共用すると S3 機が PICO 用イメージを掴んで更新に失敗するので、分けてある。

## 残作業

- **AtomS3 機の複数プローブ検証**（現在1本のみ）。
- ATOM U 機は**バスに応答なし＝配線確認待ち**のまま棚上げ。
- ROM ↔ 実体の対応付け（1本ずつ握って Dashboard で確認）。
- CCM を使うなら yasu-hp の bridge に `sender_override` を1行追加。
