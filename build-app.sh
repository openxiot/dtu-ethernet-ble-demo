#!/usr/bin/env bash
# 构建 App（0x3000..0x27FFF，148 KB）
#
# 用法：
#   ./build-app.sh            # 增量构建
#   ./build-app.sh --clean    # 先清理再构建
#
# 输出：
#   obj/dtu-ethernet-ble-demo.elf / .hex / .lst / .map
#
# 说明：
#   App 使用 MounRiver Studio 生成的 obj/ 构建文件（makefile、subdir.mk），
#   这些文件与绝对路径相关且不入库。新克隆仓库或移动目录后，
#   请先在 MRS 里对工程执行一次 Build，生成本机可用的 obj/。
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

OBJ_DIR="$ROOT_DIR/obj"
APP_ELF="$OBJ_DIR/dtu-ethernet-ble-demo.elf"
APP_HEX="$OBJ_DIR/dtu-ethernet-ble-demo.hex"
MK="$OBJ_DIR/makefile"

[ -f "$MK" ] || die "找不到 obj/makefile。该文件由 MounRiver Studio 生成且不入库：请在 MRS 中打开工程执行一次 Build（Clean 后 Build 亦可），再运行本脚本。"

# 工程移动/改名后，makefile 里残留的绝对路径会指向旧位置，构建会悄悄用错源码。
if ! grep -qF "$ROOT_DIR/" "$MK"; then
    die "obj/makefile 中的路径与当前仓库（${ROOT_DIR}）不一致。请在 MRS 中 Clean + Build 重新生成后重试。"
fi

if [ "$CLEAN" -eq 1 ]; then
    info "==> 清理 App 目标文件"
    make -C "$OBJ_DIR" clean
fi

info "==> 构建 App（工具链：${TOOLCHAIN_BIN}）"
# obj/makefile 的第一条规则是 FORCE（flags 文件生成），默认目标不是 all，
# 必须显式指定 all，否则 make 什么都不做。
make -C "$OBJ_DIR" all -j"$(cpu_count)"

[ -f "$APP_HEX" ] || die "构建结束但未找到 $APP_HEX"
[ -f "$APP_ELF" ] || die "构建结束但未找到 $APP_ELF"

read -r HEX_MIN HEX_MAX HEX_BYTES <<< "$(hex_range "$APP_HEX")"
[ "$HEX_MIN" = "0x03000" ] || die "App hex 起始地址是 ${HEX_MIN}，应为 0x03000（Ld/Link.ld 的 ORIGIN）"
[ $((HEX_MAX)) -lt $((0x28000)) ] \
    || die "App hex 结束地址 $HEX_MAX 越过 0x27FFF，超出 148 KB 分区"

SIZE_LINE=$("$TOOLCHAIN_BIN/riscv-none-embed-size" "$APP_ELF" | awk 'NR==2 {print $1, $2, $3}')
read -r TEXT DATA BSS <<< "$SIZE_LINE"
FLASH=$((TEXT + DATA))

info "App: flash ${FLASH} B / 151552 B（text ${TEXT} + data ${DATA}），bss ${BSS} B"
info "App: hex ${HEX_MIN}..${HEX_MAX}，${HEX_BYTES} B"
info "输出: ${APP_HEX#"$ROOT_DIR"/}"
