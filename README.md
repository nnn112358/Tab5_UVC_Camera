# Tab5_UVC_Camera

M5Stack Tab5 (ESP32-P4) の USB-A ポートに接続した UVC カメラの映像を画面に表示する ESP-IDF プロジェクトです。
MJPEG をハードウェア JPEG デコーダで展開し、PPA で回転・拡大して 1280x720 に表示します (640x480 で約 30 fps)。

- 動作確認: M5Stack Tab5 (ESP32-P4 rev v1.0) + ESP-IDF v5.5 + USB カメラ (MJPEG 対応推奨)
- 依存コンポーネント (ビルド時に自動取得): `m5stack/m5unified`, `espressif/usb_host_uvc`

## ビルドと書き込み

```sh
. ~/esp/esp-idf/export.sh
idf.py set-target esp32p4
idf.py -p /dev/ttyACM0 build flash monitor
```

ビルド環境が無い場合は `firmware/` のビルド済みバイナリを書き込めます ([firmware/README.md](firmware/README.md))。

```sh
cd firmware && ./flash.sh /dev/ttyACM0
```

## 使い方

起動すると「Connect a USB camera」と表示されます。USB-A にカメラを接続すると映像が出て、左上に解像度と fps が表示されます。抜き差しには自動で追従します。

## ドキュメント

- [詳細 (構成、設定値、処理の流れ、設計メモ、sdkconfig の注意点、参考リンク)](docs/details.md)
