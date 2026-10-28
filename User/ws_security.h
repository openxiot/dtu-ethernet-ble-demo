/*********************************************************************
 * File Name          : ws_security.h
 *
 * Description        : WebSocket 握手安全原语：SHA-1 与 Base64。
 *                      用于计算/校验 Sec-WebSocket-Accept。
 *                      自包含、无外部依赖，适合裸机。
 *******************************************************************************/
#ifndef __WS_SECURITY_H__
#define __WS_SECURITY_H__

#include <stdint.h>
#include <stddef.h>

/* SHA-1，输出 20 字节摘要写入 out[20]。 */
void ws_sha1(const uint8_t *data, size_t len, uint8_t out[20]);

/* Base64 编码（标准 RFC4648 字母表），写入 out，返回输出长度(不含\0)。
 * out 需 >= (len+2)/3*4；函数不会写终止符。 */
size_t ws_base64_encode(const uint8_t *data, size_t len, char *out);

/* RFC6455：Sec-WebSocket-Accept = base64( SHA1( client_key + magic ) )
 * magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
 * client_key 是握手时客户端随机生成的 Sec-WebSocket-Key。
 * 返回值写入 accept(>=29 字节)，以 \0 结尾，正常返回指向 accept 指针。 */
char *ws_compute_accept(const char *client_key, char *accept);

#endif /* __WS_SECURITY_H__ */