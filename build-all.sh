#!/usr/bin/env bash
# 一键构建：Bootloader + App -> 合并镜像 -> .dtufw 固件包
#
# 用法：
#   ./build-all.sh [--clean] [pack-firmware.sh 的选项]
#
# 示例：
#   ./build-all.sh
#   ./build-all.sh --clean
#   ./build-all.sh --version 0.2.0 --key prod_private.pem
#
# 除 --clean 外，其余参数原样传给 ./pack-firmware.sh（-h 查看其用法）。
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$ROOT/tools/build-env.sh"

case "${1:-}" in
    -h|--help) usage_from_header "$0"; exit 0 ;;
esac

if [ "${1:-}" = "--clean" ]; then
    "$ROOT/build-bootloader.sh" --clean
    "$ROOT/build-app.sh" --clean
    shift
else
    "$ROOT/build-bootloader.sh"
    "$ROOT/build-app.sh"
fi

"$ROOT/merge-image.sh"
"$ROOT/pack-firmware.sh" "$@"
