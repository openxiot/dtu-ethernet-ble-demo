#!/usr/bin/env python3
"""
pubkey2h.py — 把 RSA 公钥 DER 转成 User/ota_pubkey.h。

用法：
    python3 tools/pubkey2h.py tools/keys/prod_public.der -o User/ota_pubkey.h

私钥（*.pem）不随仓库提交；本工具只处理公钥。
"""
import argparse
import sys

TEMPLATE = """\
/*********************************************************************
 * File Name          : ota_pubkey.h
 *
 * Description        : .dtufw 验签用的 RSA-2048 公钥（DER，SubjectPublicKeyInfo）。
 *
 * 由 tools/pubkey2h.py 根据 {source} 生成，请勿手工修改。
 * 对应私钥不得提交仓库；量产前必须替换为产品线密钥对：
 *   1) openssl genrsa -out prod_private.pem 2048
 *   2) openssl rsa -in prod_private.pem -pubout -outform DER -out prod_public.der
 *   3) python3 tools/pubkey2h.py prod_public.der -o User/ota_pubkey.h
 *******************************************************************************/
#ifndef __OTA_PUBKEY_H__
#define __OTA_PUBKEY_H__

#include <stdint.h>

static const uint8_t ota_pubkey_der[] = {{
{body}
}};

#define OTA_PUBKEY_DER_LEN      ((uint32_t)sizeof(ota_pubkey_der))

#endif /* __OTA_PUBKEY_H__ */
"""


def main() -> int:
    ap = argparse.ArgumentParser(description="DER 公钥 -> ota_pubkey.h")
    ap.add_argument("der", help="SubjectPublicKeyInfo DER 文件")
    ap.add_argument("-o", "--output", default="User/ota_pubkey.h")
    ap.add_argument("-s", "--source", help="写进注释的来源名（默认取文件名）")
    args = ap.parse_args()

    try:
        with open(args.der, "rb") as f:
            data = f.read()
    except OSError as e:
        print(f"错误：无法读取 {args.der}: {e}", file=sys.stderr)
        return 1

    if len(data) < 64 or data[0] != 0x30:
        print("错误：不是 DER 编码的 SubjectPublicKeyInfo", file=sys.stderr)
        return 1

    rows = []
    for i in range(0, len(data), 12):
        rows.append("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 12])
                    + ("," if i + 12 < len(data) else ""))
    body = "\n".join(rows)

    text = TEMPLATE.format(source=args.source or args.der, body=body)
    with open(args.output, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"{len(data)} B -> {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
