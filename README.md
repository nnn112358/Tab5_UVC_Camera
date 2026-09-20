# Tab5_UVC_Camera

M5Stack Tab5 (ESP32-P4) の USB-A ポートに接続した UVC カメラの映像を、M5Unified で画面に表示する ESP-IDF プロジェクトです。

- 著者: **nnn112358**
- 動作確認: M5Stack Tab5 (ESP32-P4 rev v1.0) + ESP-IDF v5.5 + USB HS カメラ (MJPEG)
- 実測: 640x480 MJPEG を 30 fps で表示 (デコード 4 ms + 回転/拡大 19 ms / フレーム)

## 特徴

| 処理 | 実装 |
|---|---|
| USB ホスト / UVC | `espressif/usb_host_uvc` v2 |
| MJPEG デコード | ESP32-P4 ハードウェア JPEG デコーダ (`esp_driver_jpeg`) → RGB565 |
| 回転・拡大 | ESP32-P4 PPA (`esp_driver_ppa`) で時計回り 90 度 + 画面フィット |
| 表示 | M5GFX (Panel_DSI) のフレームバッファへ PPA が直接書き込み。文字は `M5.Display` で上書き |
| USB-A 5V 給電 | `M5.Power.setExtOutput(true, m5::ext_USB)` |
| 再接続 | カメラの抜き差しを検出して自動で再オープン |
| フォールバック | MJPEG が無いカメラは YUY2 をソフトウェア変換 |

## 必要なもの

- ESP-IDF v5.5 以降 (`get_idf` で有効化)
- M5Stack Tab5
- UVC 対応 USB カメラ (MJPEG 対応推奨)

依存コンポーネント (`main/idf_component.yml`) はビルド時に自動取得されます。

- `m5stack/m5unified`
- `espressif/usb_host_uvc`

## ビルドと書き込み

```sh
get_idf
cd Tab5_UVC_Camera
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

起動すると「Connect a USB camera」と表示され、カメラを接続すると映像が出ます。左上に解像度と fps を表示します。

### ビルド済みファームウェアを使う

`firmware/` にビルド済みバイナリを格納しています。ビルド環境が無くても `esptool.py` だけで書き込めます。

```sh
cd firmware
./flash.sh /dev/ttyACM0
# または結合イメージを 1 ファイルで
esptool.py --chip esp32p4 -p /dev/ttyACM0 -b 460800 write_flash 0x0 Tab5_UVC_Camera_merged.bin
```

詳細は `firmware/README.md` を参照してください。

## 設定

`main/main.cpp` 冒頭の定数で変更できます。

| 定数 | 既定 | 内容 |
|---|---|---|
| `PREF_FRAME_W` / `PREF_FRAME_H` | 640 / 480 | カメラに要求する解像度。無ければ画面に収まる最大 MJPEG、次に YUY2 を選択 |
| `FIT_TO_SCREEN` | 1 | 1: アスペクト比を保って画面に拡大、0: 等倍で中央表示 |
| `SHOW_FPS` | 1 | 左上に解像度と fps を表示 |
| `LANDSCAPE_ROTATION` | `PPA_SRM_ROTATION_ANGLE_270` | 表示の向き。`_90` にすると上下逆 |

## 処理の流れ

1. `M5.begin()` 後に USB-A の 5V を ON
2. `usb_host_uvc` の接続イベントで対応フォーマット一覧を取得し、解像度を選択
3. 受信フレームをキュー経由で処理タスク (Core 1) へ渡す
4. ハードウェア JPEG デコーダで RGB565 に展開
5. PPA で回転 + 拡大し、DSI フレームバッファへ直接書き込み
6. 切断イベントでストリームを閉じ、再接続を待つ

## 設計メモ

- **M5GFX の回転描画は使わない**: `setRotation(1)` で `pushImage` すると PSRAM 上の縦向きフレームバッファへ 1 ピクセルずつ書き込むため、1280x720 で約 1 fps しか出ません。PPA で回転してフレームバッファに直接書くことで 30 fps になります。
- **色順**: ハードウェア JPEG デコーダの `JPEG_DEC_RGB_ELEMENT_ORDER_BGR` はリトルエンディアン RGB565 (uint16 = RRRRRGGGGGGBBBBB) を出力します。`M5.Display.setSwapBytes(true)` を設定しているので、`pushImage(uint16_t*)` にこの形式をそのまま渡せます。
- **フレームバッファ**: `Panel_DSI::config_detail().buffer` で取得できます (`buffer_length` は 0 のままなのでパネル寸法から計算)。
- **JPEG デコーダの入力**: DMA がフラッシュ (rodata) を読めないため、入力データは RAM に置く必要があります。

## sdkconfig の注意点

`sdkconfig.defaults` に設定済みです。

- Tab5 の ESP32-P4 はリビジョン v1.0 のため `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` と `CONFIG_ESP32P4_REV_MIN_100=y` が必要です。既定 (v3.x 向け) のままだと書き込み時に拒否されます。
- ブートローダが 0x8000 に収まらないため、パーティションテーブルを 0x10000 に配置しています (`partitions.csv`)。
- M5GFX の Tab5 サポートは PSRAM 200MHz が必須です。

## 参考

- [Hiroki-Kawakami/Tab5-UVC-Display](https://github.com/Hiroki-Kawakami/Tab5-UVC-Display)
- [espressif/usb_host_uvc](https://components.espressif.com/components/espressif/usb_host_uvc)
- [M5Unified](https://github.com/m5stack/M5Unified) / [M5GFX](https://github.com/m5stack/M5GFX)
