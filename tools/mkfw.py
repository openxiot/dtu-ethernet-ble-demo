#!/usr/bin/env python3
"""
mkfw.py — 生成 .dtufw 固件包（设计文档 5.2 / 10.x）

包结构：
    0x000  512 B 包头
    0x200  镜像，256 B 对齐，尾部补 0xFF

签名对象：SHA-256(header[0x000..0x040) || image)，RSA-2048 PKCS#1 v1.5。

用法：
    python3 tools/mkfw.py --hex obj/dtu-ethernet-ble-demo.hex \
        --version 0.2.0 --key tools/keys/dev_private.pem \
        -o build/dtu-0.2.0.dtufw

    # 或从 bin 打包
    python3 tools/mkfw.py --bin app.bin --version 0.2.0 \
        --key tools/keys/dev_private.pem -o app.dtufw
"""
import argparse
import hashlib
import os
import re
import struct
import subprocess
import sys
import tempfile

APP_BASE = 0x00003000
APP_SIZE = 148 * 1024
HEADER_SIZE = 512
DELTA = 256
HW_ID = 1

MAGIC = b"DTUFW" + bytes([1, 0, 0])


def parse_version(s: str) -> int:
    parts = s.split(".")
    if len(parts) != 3:
        raise argparse.ArgumentTypeError("版本必须是 major.minor.patch")
    v = [int(p) for p in parts]
    if any(x < 0 or x > 255 for x in v):
        raise argparse.ArgumentTypeError("版本各段必须在 0..255")
    return (v[0] << 16) | (v[1] << 8) | v[2]


def read_ihex(path: str) -> bytes:
    """读取 Intel HEX，返回从 App 基址开始的镜像（长度按最高地址裁剪）。"""
    mem = {}
    upper = 0
    with open(path, "r", encoding="ascii") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            if not line.startswith(":"):
                raise ValueError(f"{path}:{lineno}: 不是 Intel HEX 行")
            raw = bytes.fromhex(line[1:])
            if len(raw) < 5:
                raise ValueError(f"{path}:{lineno}: 记录太短")
            count, addr, rtype = raw[0], (raw[1] << 8) | raw[2], raw[3]
            data = raw[4:4 + count]
            if (sum(raw) & 0xFF) != 0:
                raise ValueError(f"{path}:{lineno}: 校验和错误")
            if rtype == 0x00:
                base = upper + addr
                for i, b in enumerate(data):
                    mem[base + i] = b
            elif rtype == 0x01:
                break
            elif rtype == 0x02:
                upper = int.from_bytes(data, "big") << 4
            elif rtype == 0x04:
                upper = int.from_bytes(data, "big") << 16
            else:
                continue

    if not mem:
        raise ValueError(f"{path}: 没有数据记录")

    lo, hi = min(mem), max(mem)
    if lo < APP_BASE:
        raise ValueError(f"{path}: 数据从 0x{lo:05X} 开始，低于 App 基址 0x{APP_BASE:05X}"
                         "（是否误用了合并 hex？）")
    img = bytearray(hi - APP_BASE + 1)
    for a, b in mem.items():
        img[a - APP_BASE] = b
    return bytes(img)


def read_bin(path: str) -> bytes:
    with open(path, "rb") as f:
        return f.read()


def sign(key: str, blob: bytes) -> bytes:
    with tempfile.NamedTemporaryFile(delete=False) as tf:
        tf.write(blob)
        tmp = tf.name
    try:
        out = subprocess.run(
            ["openssl", "dgst", "-sha256", "-sign", key, tmp],
            check=True, capture_output=True)
        return out.stdout
    finally:
        os.unlink(tmp)


def verify(pub: str, blob: bytes, sig: bytes) -> bool:
    with tempfile.NamedTemporaryFile(delete=False) as tf:
        tf.write(blob)
        tmp = tf.name
    with tempfile.NamedTemporaryFile(delete=False) as sf:
        sf.write(sig)
        sigf = sf.name
    try:
        out = subprocess.run(
            ["openssl", "dgst", "-sha256", "-verify", pub,
             "-signature", sigf, tmp],
            capture_output=True)
        return out.returncode == 0
    finally:
        os.unlink(tmp)
        os.unlink(sigf)


def main() -> int:
    ap = argparse.ArgumentParser(description="生成 .dtufw 固件包")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--hex", help="App 的 Intel HEX（链接在 0x3000）")
    src.add_argument("--bin", help="App 的原始二进制（从 0x3000 起）")
    ap.add_argument("--version", required=True, type=parse_version)
    ap.add_argument("--hw-id", type=lambda s: int(s, 0), default=HW_ID)
    ap.add_argument("--key", required=True, help="RSA 私钥（PEM），不提交仓库")
    ap.add_argument("--pub", help="用于打包后自检的公钥 DER/PEM")
    ap.add_argument("--allow-downgrade", action="store_true")
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()

    cfg = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "..", "User", "ota_config.h")
    fw_ver = config_version(cfg)
    if fw_ver is not None and fw_ver != args.version:
        print(f"警告：包头版本 {version_str(args.version)} 与 "
              f"User/ota_config.h 的 OTA_FW_VERSION "
              f"{version_str(fw_ver)} 不一致（编译进固件的版本才是升级比较依据）",
              file=sys.stderr)

    image = read_ihex(args.hex) if args.hex else read_bin(args.bin)
    if len(image) == 0:
        print("错误：镜像为空", file=sys.stderr)
        return 1
    raw_len = len(image)
    if raw_len > APP_SIZE:
        print(f"错误：镜像 {raw_len} B 超过 App 分区 {APP_SIZE} B", file=sys.stderr)
        return 1

    if raw_len % DELTA:
        image += b"\xff" * (DELTA - raw_len % DELTA)

    file_size = HEADER_SIZE + len(image)
    image_crc = 0xFFFFFFFF
    for b in image:
        image_crc = (image_crc >> 8) ^ CRC_TABLE[(image_crc ^ b) & 0xFF]
    image_crc ^= 0xFFFFFFFF
    image_sha = hashlib.sha256(image).digest()

    flags = 0x0001 if args.allow_downgrade else 0x0000
    hdr = bytearray(b"\xff" * HEADER_SIZE)
    hdr[0:8] = MAGIC
    struct.pack_into("<H", hdr, 0x008, HEADER_SIZE)
    struct.pack_into("<H", hdr, 0x00A, flags)
    struct.pack_into("<I", hdr, 0x00C, file_size)
    struct.pack_into("<I", hdr, 0x010, len(image))
    struct.pack_into("<I", hdr, 0x014, image_crc)
    struct.pack_into("<I", hdr, 0x018, args.version)
    struct.pack_into("<I", hdr, 0x01C, args.hw_id)
    hdr[0x020:0x040] = image_sha
    hdr[0x040:0x140] = b"\x00" * 256     # 签名占位

    blob = bytes(hdr[0:0x40]) + image
    sig = sign(args.key, blob)
    if len(sig) != 256:
        print(f"错误：签名长度 {len(sig)}，需要 RSA-2048（256 B）", file=sys.stderr)
        return 1
    hdr[0x040:0x140] = sig

    pkg = bytes(hdr) + image
    with open(args.output, "wb") as f:
        f.write(pkg)

    print(f"镜像      : {raw_len} B（补 0xFF 后 {len(image)} B）")
    print(f"文件      : {len(pkg)} B -> {args.output}")
    print(f"版本      : {version_str(args.version)}  hw_id=0x{args.hw_id:08X}")
    print(f"镜像 CRC32: 0x{image_crc:08X}")
    print(f"镜像 SHA  : {image_sha.hex()}")
    print(f"签名      : {sig[:8].hex()}…（256 B）")

    if args.pub:
        if verify(args.pub, blob, sig):
            print("自检      : 签名验证通过")
        else:
            print("自检      : 签名验证失败！", file=sys.stderr)
            return 1
    return 0


def config_version(path: str) -> int:
    """从 ota_config.h 读取 OTA_FW_VERSION，读不到返回 None。"""
    try:
        with open(path, "r", encoding="utf-8") as f:
            txt = f.read()
    except OSError:
        return None
    m = re.search(r"#define\s+OTA_FW_VERSION\s+OTA_MAKE_VERSION\(\s*(\d+)\s*,\s*"
                  r"(\d+)\s*,\s*(\d+)\s*\)", txt)
    if not m:
        return None
    a, b, c = (int(x) for x in m.groups())
    return (a << 16) | (b << 8) | c


def version_str(v: int) -> str:
    return f"{(v >> 16) & 0xFF}.{(v >> 8) & 0xFF}.{v & 0xFF}"


def _crc_table():
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0xEDB88320 if (c & 1) else c >> 1
        table.append(c)
    return table


CRC_TABLE = _crc_table()

if __name__ == "__main__":
    sys.exit(main())
