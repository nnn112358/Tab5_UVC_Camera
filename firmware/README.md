# ビルド済みファームウェア

ESP-IDF v5.5 / M5Stack Tab5 (ESP32-P4 rev v1.0) 向けにビルドしたバイナリです。

| ファイル | 書き込みアドレス | 内容 |
|---|---|---|
| `bootloader.bin` | 0x2000 | ブートローダ |
| `partition-table.bin` | 0x10000 | パーティションテーブル |
| `Tab5_UVC_Camera.bin` | 0x20000 | アプリケーション |
| `Tab5_UVC_Camera_merged.bin` | 0x0 | 上記 3 つを結合した 1 ファイル版 |

フラッシュ設定: mode=dio, freq=80m, size=16MB

## 書き込み方法

個別ファイルで書き込む場合 (ESP-IDF 環境で `esptool.py` が使えること):

```sh
./flash.sh /dev/ttyACM0
```

結合イメージを 1 ファイルで書き込む場合:

```sh
esptool.py --chip esp32p4 -p /dev/ttyACM0 -b 460800 write_flash 0x0 Tab5_UVC_Camera_merged.bin
```
