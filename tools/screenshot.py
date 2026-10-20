#!/usr/bin/env python3
"""Tab5 の画面をシリアル経由で PNG として保存する。

ファームウェアに "screenshot" を送ると、M5GFX の createPng() で作った PNG が
"SCREENSHOT:BEGIN <バイト数>" と "SCREENSHOT:END" に挟まれて返ってくる。

使い方:
  python tools/screenshot.py /dev/cu.usbmodem101 docs/screenshot_main.png
  # 再起動して起動モード選択画面を撮る
  python tools/screenshot.py /dev/cu.usbmodem101 docs/screenshot_mode.png \
      --reboot --wait-for "シリアルで時刻設定" --delay 1.0

pyserial が必要（PlatformIO の Python 環境には同梱: ~/.platformio/penv/bin/python）。
"""
import argparse
import struct
import sys
import time
import zlib

import serial

BEGIN = b"SCREENSHOT:BEGIN "
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def open_port(port, reboot):
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    if reboot:
        # ESP32-P4 の USB-Serial-JTAG は DTR=0 / RTS の変化でチップをリセットする
        s.dtr = False
        s.rts = False
    s.open()
    return s


def reopen_after_reset(port, deadline):
    while time.time() < deadline:
        try:
            return open_port(port, reboot=True)
        except serial.SerialException:
            time.sleep(0.2)
    raise TimeoutError("ポートを開き直せませんでした")


def read_chunk(s, port, deadline):
    try:
        return s.read(4096), s
    except serial.SerialException:
        # リセットで USB が再接続された
        s.close()
        return b"", reopen_after_reset(port, deadline)


def verify_png(data):
    if not data.startswith(PNG_SIGNATURE):
        return False
    pos = len(PNG_SIGNATURE)
    while pos + 8 <= len(data):
        length, ctype = struct.unpack(">I4s", data[pos:pos + 8])
        end = pos + 8 + length + 4
        if end > len(data):
            return False
        body = data[pos + 4:pos + 8 + length]
        crc, = struct.unpack(">I", data[pos + 8 + length:end])
        if zlib.crc32(body) & 0xFFFFFFFF != crc:
            return False
        if ctype == b"IEND":
            return True
        pos = end
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("output")
    ap.add_argument("--reboot", action="store_true", help="ポートを開くときにデバイスを再起動する")
    ap.add_argument("--wait-for", help="このテキストがログに出るまで待ってから撮影する")
    ap.add_argument("--delay", type=float, default=0.0, help="撮影要求を送るまでの待ち時間（秒）")
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--verbose", action="store_true", help="デバイスのログを表示する")
    args = ap.parse_args()

    deadline = time.time() + args.timeout
    s = open_port(args.port, args.reboot)
    buf = b""

    if args.wait_for:
        marker = args.wait_for.encode()
        while marker not in buf:
            if time.time() > deadline:
                sys.exit(f"'{args.wait_for}' がログに出ませんでした")
            chunk, s = read_chunk(s, args.port, deadline)
            if args.verbose and chunk:
                sys.stderr.write(chunk.decode(errors="replace"))
            buf += chunk
    time.sleep(args.delay)
    s.reset_input_buffer()
    s.write(b"screenshot\n")
    buf = b""

    while True:
        if time.time() > deadline:
            sys.exit("スクリーンショットを受信できませんでした")
        chunk, s = read_chunk(s, args.port, deadline)
        buf += chunk
        start = buf.find(BEGIN)
        if start < 0:
            continue
        header_end = buf.find(b"\n", start)
        if header_end < 0:
            continue
        size = int(buf[start + len(BEGIN):header_end])
        data = buf[header_end + 1:]
        while len(data) < size:
            if time.time() > deadline:
                sys.exit("スクリーンショットの受信が途中で止まりました")
            chunk, s = read_chunk(s, args.port, deadline)
            data += chunk
        png = data[:size]
        break

    if not verify_png(png):
        sys.exit("受信した PNG が壊れています（ログが混入した可能性があります）。もう一度実行してください")
    with open(args.output, "wb") as f:
        f.write(png)
    print(f"{args.output} を保存しました（{len(png)} バイト）")


if __name__ == "__main__":
    main()
