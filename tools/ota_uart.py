#!/usr/bin/env python3
"""
ota_uart.py — 通过 USART1 串口升级 .dtufw（设计文档 8.3）

帧格式：
    A5 5A | ver(1) | type(1) | flags/status(2 LE) | offset(4 LE) |
    len(2 LE) | data(len) | crc16(LE，覆盖 ver..data)

用法：
    # 先让设备进入 Bootloader（复位，或在 App 里请求串口升级），然后：
    python3 tools/ota_uart.py /dev/tty.usbserial-XXXX build/app.dtufw

    # 工具会持续发 HELLO，请在启动工具后复位设备，
    # 或在 500 ms 窗口前就开始运行。
"""
import argparse
import struct
import sys
import time

try:
    import serial
except ImportError:
    serial = None

SYNC0, SYNC1, VER = 0xA5, 0x5A, 0x01
MSG_HELLO, MSG_BEGIN, MSG_DATA, MSG_END = 0x01, 0x02, 0x03, 0x04
MSG_HELLO_ACK, MSG_BEGIN_ACK, MSG_DATA_ACK, MSG_END_ACK = 0x81, 0x82, 0x83, 0x84
MSG_ABORT, MSG_RESET = 0x05, 0x06

PAGE = 256
HDR = 10


def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def build(ftype, offset=0, data=b"", flags=0):
    body = struct.pack("<BBHIH", VER, ftype, flags, offset, len(data)) + data
    return struct.pack(">BB", SYNC0, SYNC1) + body + struct.pack("<H", crc16(body))


def read_frame(ser, timeout):
    """返回 (type, status, offset, data)，超时 None。"""
    deadline = time.monotonic() + timeout
    state = 0
    while time.monotonic() < deadline:
        b = ser.read(1)
        if not b:
            continue
        v = b[0]
        if state == 0:
            state = 1 if v == SYNC0 else 0
        elif state == 1:
            if v == SYNC1:
                break
            state = 1 if v == SYNC0 else 0
    else:
        return None

    hdr = ser.read(HDR)
    if len(hdr) != HDR:
        return None
    ver_, ftype, status, offset, length = struct.unpack("<BBHIH", hdr)
    if ver_ != VER or length > 256:
        return None
    data = ser.read(length)
    tail = ser.read(2)
    if len(data) != length or len(tail) != 2:
        return None
    crc = struct.unpack("<H", tail)[0]
    if crc16(hdr + data) != crc:
        return None
    return ftype, status, offset, data


def wait_for(ser, types, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        f = read_frame(ser, min(0.25, max(0.01, deadline - time.monotonic())))
        if f and f[0] in types:
            return f
    return None


def version_str(v):
    return f"{(v >> 16) & 0xFF}.{(v >> 8) & 0xFF}.{v & 0xFF}"


def main():
    ap = argparse.ArgumentParser(description="串口 IAP 升级工具")
    ap.add_argument("port")
    ap.add_argument("file", help=".dtufw 固件包")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--hello-timeout", type=float, default=15.0)
    ap.add_argument("--ack-timeout", type=float, default=5.0)
    ap.add_argument("--allow-downgrade", action="store_true")
    args = ap.parse_args()

    if serial is None:
        print("需要 pyserial: pip3 install pyserial", file=sys.stderr)
        return 2

    pkg = open(args.file, "rb").read()
    if len(pkg) < 512 or pkg[0:5] != b"DTUFW":
        print("错误：不是 .dtufw 文件", file=sys.stderr)
        return 1
    file_size, image_size, image_crc, version, hw_id = struct.unpack_from(
        "<IIIII", pkg, 0x00C)
    if file_size != len(pkg):
        print(f"错误：包头 file_size={file_size} 与实际 {len(pkg)} 不符", file=sys.stderr)
        return 1
    if file_size % PAGE:
        print("错误：文件未按 256 B 对齐", file=sys.stderr)
        return 1

    print(f"固件 : {args.file}")
    print(f"版本 : {version_str(version)}  hw_id=0x{hw_id:08X}  "
          f"镜像={image_size} B  CRC32=0x{image_crc:08X}")

    ser = serial.Serial(args.port, args.baud, timeout=0.02)
    ok = False
    try:
        # 1) HELLO：持续发送直至 Bootloader 应答
        print("HELLO: 等待 Bootloader（请现在复位设备）…")
        deadline = time.monotonic() + args.hello_timeout
        ack = None
        while time.monotonic() < deadline:
            ser.write(build(MSG_HELLO))
            ser.flush()
            f = wait_for(ser, (MSG_HELLO_ACK,), 0.05)
            if f:
                ack = f
                break
        if not ack:
            print("错误：等待 HELLO_ACK 超时", file=sys.stderr)
            return 1

        bl_ver, app_ver, app_state, staged, chunk, dev_hw = struct.unpack(
            "<IIBBHI", ack[3][:16])
        print(f"Boot : {version_str(bl_ver)}  hw_id=0x{dev_hw:08X}  "
              f"app={version_str(app_ver)} state={app_state} staged={staged} "
              f"chunk={chunk}")
        if dev_hw != hw_id:
            print(f"错误：机型不匹配（设备 0x{dev_hw:08X} ≠ 包 0x{hw_id:08X}）",
                  file=sys.stderr)
            return 1

        # 2) BEGIN
        flags = 0x0001 if args.allow_downgrade else 0x0000
        ser.write(build(MSG_BEGIN, data=struct.pack(
            "<II IH", file_size, crc32(pkg), version, flags)))
        ser.flush()
        f = wait_for(ser, (MSG_BEGIN_ACK,), args.ack_timeout)
        if not f:
            print("错误：等待 BEGIN_ACK 超时", file=sys.stderr)
            return 1
        if f[1] != 0:
            print(f"错误：BEGIN 被拒绝，status={f[1]}", file=sys.stderr)
            return 1
        print("BEGIN: 接受")

        # 3) DATA
        total = len(pkg) // PAGE
        t0 = time.monotonic()
        for i in range(total):
            off = i * PAGE
            ser.write(build(MSG_DATA, offset=off, data=pkg[off:off + PAGE]))
            ser.flush()
            f = wait_for(ser, (MSG_DATA_ACK,), args.ack_timeout)
            if not f:
                print(f"\n错误：offset {off} 等待 DATA_ACK 超时", file=sys.stderr)
                return 1
            if f[1] != 0:
                print(f"\n错误：offset {off} 写入失败 status={f[1]}", file=sys.stderr)
                return 1
            if f[2] != off + PAGE:
                print(f"\n警告：ACK offset={f[2]}，期望 {off + PAGE}", file=sys.stderr)
            if (i + 1) % 64 == 0 or i + 1 == total:
                pct = (i + 1) * 100 // total
                print(f"\rDATA : {pct:3d}%  {off + PAGE}/{len(pkg)} B", end="")
        dt = time.monotonic() - t0
        print(f"\rDATA : 100%  {len(pkg)} B，用时 {dt:.1f}s "
              f"({len(pkg) / max(dt, 0.001) / 1024:.1f} KB/s)")

        # 4) END
        ser.write(build(MSG_END))
        ser.flush()
        f = wait_for(ser, (MSG_END_ACK,), args.ack_timeout)
        if not f:
            print("错误：等待 END_ACK 超时", file=sys.stderr)
            return 1
        if f[1] != 0:
            print(f"错误：整包校验失败 status={f[1]}", file=sys.stderr)
            return 1

        ok = True
        print("END  : 校验通过，Bootloader 正在搬运并重启…")
        return 0
    finally:
        if not ok:
            # 主动结束会话，避免 Bootloader 停在等待帧的状态
            try:
                ser.write(build(MSG_ABORT))
                ser.flush()
                time.sleep(0.05)
            except Exception:
                pass
        ser.close()


def crc32(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xEDB88320 if crc & 1 else crc >> 1
    return crc ^ 0xFFFFFFFF


if __name__ == "__main__":
    sys.exit(main())
