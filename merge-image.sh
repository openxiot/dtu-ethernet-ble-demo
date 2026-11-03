#!/usr/bin/env bash
# 合并 Bootloader + App，生成首次烧写用的完整镜像
#
# 用法：
#   ./merge-image.sh [-o 输出文件]
#
# 默认输出：
#   build/dtu-full.hex
#
# 布局：Boot 0x00000..0x02FFF，App 0x03000..0x27FFF。
# 该镜像用于 WCH-Link/OpenOCD 首次烧写；后续升级用 pack-firmware.sh 生成的 .dtufw。
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/tools/build-env.sh"

OUT="$ROOT_DIR/build/dtu-full.hex"
while [ $# -gt 0 ]; do
    case "$1" in
        -o|--output)
            [ $# -ge 2 ] || die "$1 需要参数"
            OUT=$2; shift 2 ;;
        -h|--help) usage_from_header "$0"; exit 0 ;;
        *) die "未知参数：$1（--help 查看用法）" ;;
    esac
done

BOOT_HEX="$ROOT_DIR/Bootloader/build/bootloader.hex"
APP_HEX="$ROOT_DIR/obj/dtu-ethernet-ble-demo.hex"

[ -f "$BOOT_HEX" ] || die "缺少 ${BOOT_HEX}（先运行 ./build-bootloader.sh）"
[ -f "$APP_HEX" ]  || die "缺少 ${APP_HEX}（先运行 ./build-app.sh）"

case "$OUT" in
    /*) ;;
    *) OUT="$ROOT_DIR/$OUT" ;;
esac
mkdir -p "$(dirname "$OUT")"

info "==> 合并 Bootloader + App"
python3 "$ROOT_DIR/tools/merge_hex.py" "$BOOT_HEX" "$APP_HEX" -o "$OUT"

info "输出: ${OUT#"$ROOT_DIR"/}"
info "烧写: openocd ... -c \"program ${OUT#"$ROOT_DIR"/} verify reset exit\""
