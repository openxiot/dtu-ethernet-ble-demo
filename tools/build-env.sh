#!/usr/bin/env bash
# 公共构建环境（供仓库根目录的构建/打包脚本 source，不单独执行）
#
#   ROOT_DIR        仓库根目录
#   find_toolchain  定位 MounRiver Studio 的 riscv-none-embed 工具链并加入 PATH
#   fw_version      读取 User/ota_config.h 中的 OTA_FW_VERSION
#   hex_range       读取 Intel HEX 的地址范围与字节数
#
# 工具链查找顺序：
#   1. PATH 中已有的 riscv-none-embed-gcc
#   2. $RISCV_TOOLCHAIN_BIN
#   3. $MRS_HOME 下的常见安装位置
#   4. /Applications/MounRiver Studio*.app（macOS 默认安装）
#
# App 与 Bootloader 都使用 GCC 8.2 的 riscv-none-embed- 前缀；
# GCC12/GCC15 自带工具链的前缀是 riscv-wch-elf- / riscv32-wch-elf-，不适用。

ROOT_DIR=${ROOT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)}

info() { printf '%s\n' "$*"; }
warn() { printf '警告：%s\n' "$*" >&2; }
die()  { printf '错误：%s\n' "$*" >&2; exit 1; }

# 打印脚本开头的注释块（跳过 shebang），用于 --help。
usage_from_header() {
    awk 'NR==1 {next} /^#/ {sub(/^# ?/, ""); print; next} {exit}' "$1"
}

cpu_count() {
    getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1
}

find_toolchain() {
    local c
    local -a candidates=()

    if command -v riscv-none-embed-gcc >/dev/null 2>&1; then
        TOOLCHAIN_BIN=$(cd "$(dirname "$(command -v riscv-none-embed-gcc)")" && pwd)
        return 0
    fi

    if [ -n "${RISCV_TOOLCHAIN_BIN:-}" ]; then
        candidates+=("$RISCV_TOOLCHAIN_BIN")
    fi
    if [ -n "${MRS_HOME:-}" ]; then
        candidates+=(
            "$MRS_HOME/Contents/Resources/app/resources/darwin/components/WCH/Toolchain/RISC-V Embedded GCC/bin"
            "$MRS_HOME/Resources/app/resources/darwin/components/WCH/Toolchain/RISC-V Embedded GCC/bin"
            "$MRS_HOME/resources/app/resources/darwin/components/WCH/Toolchain/RISC-V Embedded GCC/bin"
            "$MRS_HOME/components/WCH/Toolchain/RISC-V Embedded GCC/bin"
        )
    fi
    for c in /Applications/MounRiver*.app; do
        if [ -d "$c" ]; then
            candidates+=("$c/Contents/Resources/app/resources/darwin/components/WCH/Toolchain/RISC-V Embedded GCC/bin")
        fi
    done

    # 空数组在 bash 3.2 + set -u 下展开 "${arr[@]}" 会报错，先判断元素个数。
    if [ ${#candidates[@]} -gt 0 ]; then
        for c in "${candidates[@]}"; do
            if [ -x "$c/riscv-none-embed-gcc" ]; then
                TOOLCHAIN_BIN=$c
                export PATH="$TOOLCHAIN_BIN:$PATH"
                return 0
            fi
        done
    fi

    die "未找到 riscv-none-embed-gcc。请安装 MounRiver Studio（macOS 默认安装在 /Applications），或设置 RISCV_TOOLCHAIN_BIN 指向包含 riscv-none-embed-gcc 的 bin 目录。"
}

# 输出 x.y.z；读不到返回非 0。
fw_version() {
    local line v
    line=$(grep -E '^[[:space:]]*#define[[:space:]]+OTA_FW_VERSION' \
               "$ROOT_DIR/User/ota_config.h" 2>/dev/null | head -1) || return 1
    [ -n "$line" ] || return 1
    v=$(printf '%s\n' "$line" | sed -E \
        's/.*OTA_MAKE_VERSION[[:space:]]*\([[:space:]]*([0-9]+)[[:space:]]*,[[:space:]]*([0-9]+)[[:space:]]*,[[:space:]]*([0-9]+)[[:space:]]*\).*/\1.\2.\3/')
    case "$v" in
        "$line"|"") return 1 ;;
        *[!0-9.]*) return 1 ;;
    esac
    printf '%s\n' "$v"
}

# 输出 "0xMIN 0xMAX BYTES"（MIN/MAX 为实际有数据的地址）。
hex_range() {
    python3 - "$1" <<'PY'
import sys

mem = set()
upper = 0
with open(sys.argv[1], "r", encoding="ascii") as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        if not line.startswith(":"):
            sys.exit(f"{sys.argv[1]}: 不是 Intel HEX 行")
        raw = bytes.fromhex(line[1:])
        n, addr, rtype = raw[0], (raw[1] << 8) | raw[2], raw[3]
        data = raw[4:4 + n]
        if rtype == 0x00:
            base = upper + addr
            mem.update(range(base, base + n))
        elif rtype == 0x01:
            break
        elif rtype == 0x02:
            upper = int.from_bytes(data, "big") << 4
        elif rtype == 0x04:
            upper = int.from_bytes(data, "big") << 16
if not mem:
    sys.exit(f"{sys.argv[1]}: 没有数据记录")
print(f"0x{min(mem):05X} 0x{max(mem):05X} {len(mem)}")
PY
}
