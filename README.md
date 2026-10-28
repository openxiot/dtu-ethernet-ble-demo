# dtu-ethernet-ble-demo

基于 **CH32V208WBU6** 的 DTU 演示固件：设备上电后经**以太网**以 **WebSocket (wss)** 连接云平台接入服务（仿真 `yed-s710ym` 机型）。**本板无 485**，收到服务端任意请求帧即回一个**固定数据帧**（模拟从设备应答）。

- MCU：CH32V208WBU6（RISC-V，160K Flash / 32K SRAM，68 脚）
- 网络：板载 10/100M 以太网（以太网要求系统时钟 60/120MHz，本工程用 120MHz）
- 外设：**无 485**；BLE（CH32V208 自带）、UART1(PA9) 调试口
- 协议：wss 连接 `ws.dtu.ap.openxiot.cn/v1/yinerda/yed-s710ym/{IMEI}`

---

## 一、功能

| 项目 | 说明 |
|---|---|
| 自动上网 | 上电即试连；默认 DHCP 获取 IP，可用静态 IP（`dtu_config.h` 切换） |
| 域名解析 | 内置 DNS 解析 `ws.dtu.ap.openxiot.cn` |
| wss 连接 | WebSocket 客户端，服务端地址/端口/路径可配 |
| 心跳保活 | TCP 层 keep-alive + 应用层每 20s 心跳帧 `0a 0b 0c 0d 0e 0f` |
| 数据上报 | 周期上报信号强度 `csq=`、定位 `coordinates=经度,纬度` |
| 固定帧应答（无 485） | 收到服务端**任意**请求帧 → 回固定数据帧（`dtu_config.h` 的 `DTU_DEMO_REPLY_FRAME`） |
| 断线重连 | TCP/链接断开后指数退避自动重连 |
| 仿真 | 地理位置、信号强度、IMEI 先用随机数/模拟值，集中在配置头便于修改 |

---

## 二、目录结构

```
User/                    应用层（可改）
  main.c                 入口：时钟、TIM2/ETH、主循环
  ch32v20x_it.c          TIM2/ETH 中断（WCHNET 节拍 + 毫秒计数）
  dtu_config.h           ★ 所有可配参数（IMEI/服务端/上报/重连/TLS 开关）
  dtu_app.c/h            DTU 应用状态机 + 心跳/上报/应答
  transport_eth.c/h      以太网传输层（DHCP/DNS/TCP/重连，WCHNET 集成）
  wss_tls.c/h            TLS 适配层（wss 唯一需接入的外部组件）
  ws_client.c/h          WebSocket 客户端（握手/分帧/掩码/ping-pong/close）
  ws_security.c/h        SHA-1 + base64（握手 Sec-WebSocket-Accept 校验）
Net/                     以太网协议栈（已拷入 WCH 官方 NetLib）
  NetLib/                wchnet.h / libwchnet.a / eth_driver.c / eth_driver.h
  net_config.h           协议栈配置（TCP 数量、MSS、缓冲等）
Peripheral/ Startup/ Core/ Ld/   CH32V20x 标准库 & 启动/链接脚本
doc/说明.md              板卡说明、WCH-LINK 接线
```

---

## 三、主要配置（`User/dtu_config.h`）

按需修改，无需改其它代码：

```c
#define DTU_IMEI            "869999042529385"   // 设备 IMEI（URL 路径用）
#define DTU_MODEL           "yed-s710ym"        // 机型：yed-s710ym / yed-dg720ym
#define DTU_HOST            "ws.dtu.ap.openxiot.cn"
#define DTU_PORT            443                 // 443=wss, 80=ws
#define DTU_USE_DHCP        1                   // 1=DHCP, 0=静态 IP
#define DTU_TLS             1                   // 1=wss,   0=明文 ws 联调
// 定位（1e7 缩放，基准±抖动随机）
#define DTU_LON_BASE        1165583130L
#define DTU_LAT_BASE        397858900L
#define DTU_LOC_JITTER      500000L
// 信号强度随机区间
#define DTU_CSQ_MIN         19
#define DTU_CSQ_MAX         31
#define DTU_HEARTBEAT_INTERVAL_S 20
// demo：无 485，收到服务端请求回的固定数据帧
#define DTU_DEMO_REPLY_FRAME {0x01,0x03,0x02,0x00,0x64,0xB9,0xAF}
```

---

## 四、构建与烧录

1. **工具**：MounRiverStudio（WCH 官方 IDE，内置 RISC-V 工具链）。
2. **打开工程**：打开本工程目录（`.cproject`/`.project` 已配置好源文件、头文件路径与 `libwchnet.a` 链接，无需手动加）。
3. **构建**：`Project → Clean` 后 `Build`。产物 `obj/dtu-ethernet-ble.hex`。
4. **烧录接线（WCH-Link）**：

   | WCH-LINK | 板子 |
   |---|---|
   | 3V3 | VDD |
   | GND | GND |
   | SWDIO | PA13 |
   | SWCLK | PA14 |
   | RST  | RST（建议）|

5. **串口**：UART1(PA9) 115200 查看日志（连接/上报/心跳/查询应答）。

---

## 五、wss 的 TLS（mbedTLS）

WCHNET 仅提供 TCP、**不含 TLS**。`wss`(443) 需要 TLS。

- **mbedTLS 2.28.x LTS 已内置到工程 `mbedtls/`**（`include/` + `library/`，Apache-2.0，见 `mbedtls/LICENSE`）。`.cproject` 已配好：`mbedtls/include` 加入头文件路径，`mbedtls/library/*.c` 自动进编译。
- 编译用**针对本服务端定制的完整精简配置**：`mbedtls/include/mbedtls/config.h` 已换成薄封装，真正配置在 `User/mbedtls_config.h`。只保留 TLS1.2 客户端 + `ECDHE-RSA` + `AES-128-GCM-SHA256` + RSA/ECP(secp256r1) + X509 解析 + 熵/DRBG，其余全关（依据实测：服务端为 RSA-2048 证书，TLS1.2 仅接受 ECDHE-RSA AES-GCM）。
  - 记录缓冲：`IN_CONTENT_LEN=4096`（容纳服务器证书链 ~3.7KB）、`OUT_CONTENT_LEN=1024`。
  - 设备不校验服务端证书（`VERIFY_NONE`），但仍解析证书取公钥验握手签名。
- `DTU_TLS=1` 走 wss；`User/wss_tls.c` 已实现：
  - 自定义 BIO：`net_send/net_recv` 桥接 WCHNET 的 `transport_tcp_read/write`，暂不可用/断开时返回 `MBEDTLS_ERR_SSL_WANT_READ/WRITE` 或 `CONN_EOF`；
  - 合作式握手：`wss_tls_handshake()` 返回 `0=完成 / 1=进行中 / <0=失败`，主循环逐轮轮询（配 `dtu_app` 的 `ST_TLS→ST_WS→ST_OPEN`）；
  - `MBEDTLS_SSL_VERIFY_NONE`（不校验证书链，省 RAM），裸机弱熵 `weak_entropy`（正式产品请接真随机源）。

**⚠️ 内存/Flash 警告（务必读）：**
`mbedtls/` 是完整库源码、默认配置体积较大；CH32V208 仅 **32K SRAM / 160K Flash**，默认配置很可能 **Flash 吃紧、且 16K 记录的 TLS 缓冲运行时放不下**。
- 本版先把「库进工程、能编译」跑通；
- 若 Flash/RAM 报超限，需把 `User/mbedtls_config.h` 升级成一份**真正精简的完整配置**（`MBEDTLS_CONFIG_FILE`，含：调小 `MBEDTLS_SSL_IN/OUT_CONTENT_LEN`、只留 ECDHE+AES-GCM-SHA256、去掉大量证书/套件），这要在真实工具链上编译联调才稳妥；
- 终极选项：换更大 SRAM/Flash 型号（如 CH32V307/303，64K）。

> `DTU_TLS=0` 为不依赖 mbedTLS 的明文联调分支，可先跑通 WebSocket 协议层整条链路（服务端需能接入 ws 明文口）。便携协议层（SHA-1/base64、WebSocket 握手/分帧/Ping-Pong、查询应答）已在本机对本地 WebSocket 服务端联测通过。

---

## 六、协议摘要（对齐服务端 DeviceYedS710YmProcessor）

- 地址：`wss(s)://host/v1/{org}/{model}/{IMEI}`，`org=yinerda`、`model=yed-s710ym`
- 心跳（二进制，每 20s）：`0a 0b 0c 0d 0e 0f`
- 信号强度（文本帧）：`csq=31`
- 定位（文本帧）：`coordinates=经度,纬度`（第一个值是**经度**）
- 服务端请求（二进制，任意内容）→ 回固定数据帧 `DTU_DEMO_REPLY_FRAME`（默认 `01 03 02 00 64 b9 af`）
- IMEI 已在 URL 路径中，协议内无需单独上报

---

## 七、许可证

见根目录 `LICENSE`（本项目基于 WCH CH32V20x 官方库扩展）。