#!/bin/sh
# Tab5_UVC_Camera ビルド済みファームウェアの書き込み
#   使い方: ./flash.sh [シリアルポート]   (既定: /dev/ttyACM0)
PORT="${1:-/dev/ttyACM0}"
cd "$(dirname "$0")"
esptool.py --chip esp32p4 -p "$PORT" -b 460800 --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x2000  bootloader.bin \
  0x10000 partition-table.bin \
  0x20000 Tab5_UVC_Camera.bin
