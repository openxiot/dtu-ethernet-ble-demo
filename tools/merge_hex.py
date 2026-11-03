#!/usr/bin/env python3
"""
merge_hex.py — 合并 Bootloader 与 App 的 Intel HEX，供产线一次烧录。

布局：Boot 0x00000..0x02FFF，App 0x03000..0x27FFF。
两段之间/尾部空洞保持擦除态（0xFF），生成的 hex 不写空洞。

用法：
    python3 tools/merge_hex.py Bootloader/build/bootloader.hex \
        obj/dtu-ethernet-ble-demo.hex -o build/dtu-full.hex
"""
import argparse
import sys


def read_ihex(path):
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
            count, addr, rtype = raw[0], (raw[1] << 8) | raw[2], raw[3]
            data = raw[4:4 + count]
            if (sum(raw) & 0xFF) != 0:
                raise ValueError(f"{path}:{lineno}: 校验和错误")
            if rtype == 0x00:
                for i, b in enumerate(data):
                    a = upper + addr + i
                    if a in mem and mem[a] != b:
                        raise ValueError(f"{path}:{lineno}: 地址 0x{a:05X} 与已读数据冲突")
                    mem[a] = b
            elif rtype == 0x01:
                break
            elif rtype == 0x02:
                upper = int.from_bytes(data, "big") << 4
            elif rtype == 0x04:
                upper = int.from_bytes(data, "big") << 16
    return mem


def emit_ihex(mem, path, width=16):
    addrs = sorted(mem)
    lines = []
    upper = None
    i = 0
    while i < len(addrs):
        start = addrs[i]
        chunk = [mem[start]]
        j = i + 1
        while j < len(addrs) and addrs[j] == addrs[j - 1] + 1 and len(chunk) < width:
            chunk.append(mem[addrs[j]])
            j += 1

        hi = (start >> 16) & 0xFFFF
        if hi != upper:
            upper = hi
            rec = bytes([2, 0, 0, 4]) + hi.to_bytes(2, "big")
            lines.append(":" + rec.hex().upper() + _csum(rec))

        rec = bytes([len(chunk)]) + (start & 0xFFFF).to_bytes(2, "big") + b"\x00" + bytes(chunk)
        lines.append(":" + rec.hex().upper() + _csum(rec))
        i = j

    lines.append(":00000001FF")
    with open(path, "w", encoding="ascii") as f:
        f.write("\n".join(lines) + "\n")


def _csum(rec):
    return f"{(-sum(rec)) & 0xFF:02X}"


def main():
    ap = argparse.ArgumentParser(description="合并 Bootloader + App HEX")
    ap.add_argument("boot", help="bootloader.hex（0x0 起）")
    ap.add_argument("app", help="App hex（0x3000 起）")
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()

    boot = read_ihex(args.boot)
    app = read_ihex(args.app)

    if boot and min(boot) != 0:
        print(f"错误：{args.boot} 不从 0x0 开始", file=sys.stderr)
        return 1
    if boot and max(boot) >= 0x3000:
        print(f"错误：{args.boot} 越过 0x3000（Bootloader 超过 12 KB？）", file=sys.stderr)
        return 1
    if app and (min(app) < 0x3000 or max(app) >= 0x28000):
        print(f"错误：{args.app} 不在 App 分区 0x3000..0x27FFF", file=sys.stderr)
        return 1

    merged = dict(boot)
    for a, b in app.items():
        merged[a] = b

    emit_ihex(merged, args.output)
    print(f"合并完成: {len(merged)} 字节 -> {args.output}")
    print(f"  Boot: {len(boot)} B (0x0..0x{max(boot) if boot else 0:04X})")
    print(f"  App : {len(app)} B (0x{min(app) if app else 0:04X}..0x{max(app) if app else 0:04X})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
