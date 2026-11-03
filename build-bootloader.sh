#!/usr/bin/env bash
# 构建 Bootloader（0x0000..0x2FFF，12 KB）
#
# 用法：
#   ./build-bootloader.sh            # 增量构建
#   ./build-bootloader.sh --clean    # 先清理再构建
#
# 输出：
#   Bootloader/build/bootloader.elf / .hex / .bin
#
# 工具链默认使用 MounRiver Studio 自带的 riscv-none-embed-gcc；
# 可用 RISCV_TOOLCHAIN_BIN 或 MRS_HOME 指定安装位置。
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/tools/build-env.sh"

CLEAN=0
while [ $# -gt 0 ]; do
    case "$1" in
        --clean) CLEAN=1 ;;
        -h|--help) usage_from_header "$0"; exit 0 ;;
        *) die "未知参数：$1（--help 查看用法）" ;;
    esac
    shift
done

find_toolchain

BOOT_DIR="$ROOT_DIR/Bootloader"
BOOT_HEX="$BOOT_DIR/build/bootloader.hex"
BOOT_ELF="$BOOT_DIR/build/bootloader.elf"

if [ "$CLEAN" -eq 1 ]; then
    info "==> 清理 Bootloader/build"
    make -C "$BOOT_DIR" clean
fi

info "==> 构建 Bootloader（工具链：${TOOLCHAIN_BIN}）"
make -C "$BOOT_DIR" -j"$(cpu_count)"

[ -f "$BOOT_HEX" ] || die "构建结束但未找到 $BOOT_HEX"
[ -f "$BOOT_ELF" ] || die "构建结束但未找到 $BOOT_ELF"

read -r HEX_MIN HEX_MAX HEX_BYTES <<< "$(hex_range "$BOOT_HEX")"
[ "$HEX_MIN" = "0x00000" ] || die "Bootloader hex 起始地址是 ${HEX_MIN}，应为 0x00000"
[ $((HEX_MAX)) -lt $((0x3000)) ] \
    || die "Bootloader hex 结束地址 $HEX_MAX 越过 0x2FFF（超过 12 KB）"

info "Bootloader: ${HEX_MIN}..${HEX_MAX}，${HEX_BYTES} B / 12288 B"
info "输出: ${BOOT_HEX#"$ROOT_DIR"/} (.elf/.hex/.bin)"
