#!/usr/bin/env bash
# 生成 .dtufw 固件包（App 镜像 + 包头 + SHA-256 + RSA-2048 签名）
#
# 用法：
#   ./pack-firmware.sh [选项]
#
# 选项：
#   --version X.Y.Z       包版本，默认取 User/ota_config.h 的 OTA_FW_VERSION
#   --key PEM             RSA-2048 私钥，默认 $FW_KEY 或 tools/keys/dev_private.pem
#   --pub DER             自检用公钥，默认由私钥路径推导（*_private.pem -> *_public.der）
#   --hw-id N             机型 ID，默认 1
#   --allow-downgrade     允许降级（设备默认拒绝版本号不增加的包）
#   -o, --output FILE     输出文件，默认 build/dtu-X.Y.Z.dtufw
#
# 环境变量：
#   APP_HEX                App 镜像 hex，默认 obj/dtu-ethernet-ble-demo.hex
#   FW_REQUIRE_KEY_MATCH   设为 1 时，私钥与内嵌公钥不一致即失败（CI 发布用）
#
# 打包前请先运行 ./build-app.sh（或 make），确保 hex 是最新构建。
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/tools/build-env.sh"

VERSION=""
KEY=${FW_KEY:-$ROOT_DIR/tools/keys/dev_private.pem}
PUB=""
HW_ID=1
ALLOW_DOWNGRADE=0
OUT=""

while [ $# -gt 0 ]; do
    case "$1" in
        --version)
            [ $# -ge 2 ] || die "$1 需要参数"
            VERSION=$2; shift 2 ;;
        --key)
            [ $# -ge 2 ] || die "$1 需要参数"
            KEY=$2; shift 2 ;;
        --pub)
            [ $# -ge 2 ] || die "$1 需要参数"
            PUB=$2; shift 2 ;;
        --hw-id)
            [ $# -ge 2 ] || die "$1 需要参数"
            HW_ID=$2; shift 2 ;;
        --allow-downgrade)
            ALLOW_DOWNGRADE=1; shift ;;
        -o|--output)
            [ $# -ge 2 ] || die "$1 需要参数"
            OUT=$2; shift 2 ;;
        -h|--help)
            usage_from_header "$0"; exit 0 ;;
        *)
            die "未知参数：$1（--help 查看用法）" ;;
    esac
done

if [ -z "$VERSION" ]; then
    VERSION=$(fw_version) || die "无法从 User/ota_config.h 读取 OTA_FW_VERSION，请用 --version 指定"
fi

printf '%s\n' "$VERSION" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' \
    || die "版本号格式应为 X.Y.Z（各段 0..255）：$VERSION"
[[ "$HW_ID" =~ ^(0[xX][0-9A-Fa-f]+|[0-9]+)$ ]] \
    || die "hw-id 必须是十进制或 0x 开头的十六进制：$HW_ID"

case "$KEY" in /*) ;; *) KEY="$ROOT_DIR/$KEY" ;; esac
[ -f "$KEY" ] || die "找不到签名私钥 ${KEY}。请用 --key 指定，或设置 FW_KEY；/tools/keys/*.pem 不入库。"

# 可用环境变量 APP_HEX 指向其他构建产物（如 make 生成的 build/app/*.hex）。
APP_HEX=${APP_HEX:-$ROOT_DIR/obj/dtu-ethernet-ble-demo.hex}
case "$APP_HEX" in /*) ;; *) APP_HEX="$ROOT_DIR/$APP_HEX" ;; esac
[ -f "$APP_HEX" ] || die "缺少 ${APP_HEX}（先运行 ./build-app.sh 或 make）"

if [ -z "$OUT" ]; then
    OUT="$ROOT_DIR/build/dtu-$VERSION.dtufw"
else
    case "$OUT" in /*) ;; *) OUT="$ROOT_DIR/$OUT" ;; esac
fi
mkdir -p "$(dirname "$OUT")"

# 由私钥路径推导自检公钥；找不到就跳过自检。
if [ -z "$PUB" ]; then
    case "$KEY" in
        *_private.pem) PUB="${KEY%_private.pem}_public.der" ;;
        *.pem)         PUB="${KEY%.pem}.der" ;;
    esac
    [ -f "$PUB" ] || PUB=""
else
    case "$PUB" in /*) ;; *) PUB="$ROOT_DIR/$PUB" ;; esac
    [ -f "$PUB" ] || die "找不到公钥 $PUB"
fi

# 源码比 hex 新时提醒（mtime 可能受 git 检出影响，仅提示）。
NEWER=$(find "$ROOT_DIR/User" "$ROOT_DIR/Core" "$ROOT_DIR/Peripheral/src" \
            "$ROOT_DIR/Net" "$ROOT_DIR/Startup" "$ROOT_DIR/mbedtls" \
            -type f \( -name '*.c' -o -name '*.h' -o -name '*.S' \) \
            -newer "$APP_HEX" -print -quit 2>/dev/null || true)
if [ -n "$NEWER" ]; then
    warn "有源文件比 App hex 新（如 ${NEWER#"$ROOT_DIR"/}），建议先运行 ./build-app.sh"
fi

# 网络 OTA 用 App 内嵌的公钥验签；私钥不匹配时打包仍可用于串口 IAP。
# 设置 FW_REQUIRE_KEY_MATCH=1 时，无法确认一致即失败（CI 发布用）。
check_embedded_key() {
    local hdr="$ROOT_DIR/User/ota_pubkey.h" key_sha emb_sha
    local strict=${FW_REQUIRE_KEY_MATCH:-0}
    [ -f "$hdr" ] || { warn "缺少 ${hdr}，跳过密钥一致性检查"; return 0; }

    key_sha=$(openssl pkey -in "$KEY" -pubout -outform DER 2>/dev/null \
              | openssl dgst -sha256 2>/dev/null | awk '{print $NF}') || true
    if [ -z "$key_sha" ]; then
        if [ "$strict" = 1 ]; then
            die "无法从 $KEY 导出公钥，无法确认与 User/ota_pubkey.h 一致（FW_REQUIRE_KEY_MATCH=1）"
        fi
        warn "无法从 $KEY 导出公钥，跳过与 User/ota_pubkey.h 的一致性检查"
        return 0
    fi

    emb_sha=$(python3 - "$hdr" <<'PY'
import hashlib, re, sys
txt = open(sys.argv[1], encoding="utf-8").read()
m = re.search(r"ota_pubkey_der\s*\[\s*\]\s*=\s*\{(.*?)\}", txt, re.S)
if not m:
    sys.exit(0)
data = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))
print(hashlib.sha256(data).hexdigest())
PY
)

    if [ -z "$emb_sha" ]; then
        warn "无法解析 User/ota_pubkey.h，跳过密钥一致性检查"
    elif [ "$key_sha" != "$emb_sha" ]; then
        if [ "$strict" = 1 ]; then
            die "签名私钥与 App 内嵌公钥（User/ota_pubkey.h）不一致，网络 OTA 会验签失败。请改用匹配的私钥，或用 tools/pubkey2h.py 重新生成 User/ota_pubkey.h 后重新编译 App。"
        fi
        warn "签名私钥与 App 内嵌公钥（User/ota_pubkey.h）不一致："
        warn "  - 串口 IAP 不验签，这个包仍可用于串口升级；"
        warn "  - 网络 OTA 会验签失败。请改用匹配的私钥（--key <PEM>），或用 tools/pubkey2h.py 重新生成 User/ota_pubkey.h 后重新编译 App。"
    else
        info "密钥检查: 签名私钥与 User/ota_pubkey.h 一致"
    fi
}

check_embedded_key

info "==> 打包 .dtufw"
MKFW_ARGS=(--hex "$APP_HEX" --version "$VERSION" --hw-id "$HW_ID" --key "$KEY" -o "$OUT")
if [ -n "$PUB" ]; then
    MKFW_ARGS+=(--pub "$PUB")
fi
if [ "$ALLOW_DOWNGRADE" -eq 1 ]; then
    MKFW_ARGS+=(--allow-downgrade)
fi
python3 "$ROOT_DIR/tools/mkfw.py" "${MKFW_ARGS[@]}"

[ -f "$OUT" ] || die "打包结束但未找到 $OUT"
info "输出: ${OUT#"$ROOT_DIR"/}（$(wc -c < "$OUT" | tr -d ' ') B）"
info "串口 IAP: python3 tools/ota_uart.py /dev/tty.usbserial-XXXX ${OUT#"$ROOT_DIR"/}"
