# dtu-ethernet-ble-demo

基于 **CH32V208WBU6** 的 DTU 演示固件：设备上电后经**以太网**以 **WebSocket (wss)** 连接云平台接入服务（仿真 `yed-s710ym` 机型）。**本板无 485**，收到服务端任意请求帧即回一个**固定数据帧**（模拟从设备应答）。

- MCU：CH32V208WBU6（RISC-V，480K CodeFlash〔前 160K 零等待〕/ 32K SRAM，68 脚）
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
| 固件升级 | Bootloader + 分区布局；USART1 串口 IAP；网络 OTA（WS 通知触发、HTTPS 下载、CRC/SHA-256/RSA-2048 校验） |
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
  ota_*.c/h              OTA：分区/元数据/固件包/串口/HTTPS/应用状态机
Bootloader/              独立 Bootloader 工程（Makefile 构建）
  boot_main.c            启动决策与 App 跳转
  boot_iap.c             串口 IAP 协议
  boot_flash.c/.h        Flash 擦写与 Staging→App 搬运
  boot_uart.c/.h         USART1 逐字节收发（PA9/PA10）
  Link_boot.ld           0x0 起 12 KB
tools/                   打包与升级工具（Python 3）
  mkfw.py                生成 .dtufw 固件包
  merge_hex.py           合并 Boot + App，供产线烧录
  ota_uart.py            串口 IAP 主机端
  keys/                  开发用密钥（私钥被 .gitignore 忽略）
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

完整步骤（环境变量、命令行构建、合并烧录、首次上电注意事项）见
[`how-to-build-flash.md`](how-to-build-flash.md)；网页版见
[`how-to-build-flash.html`](how-to-build-flash.html)。

1. **工具**：MounRiver Studio 3（WCH 官方 IDE，内置 RISC-V 工具链）。
2. **打开工程**：`File → Open Folder…` 选本仓库根目录（或命令面板执行 `Open MRS Project`）。
   `.cproject`/`.project` 已配好源文件、头文件路径与 `libwchnet.a` 链接。
3. **构建**：右键工程 → `Build Project`（或 `ReBuild Project`）。`obj/` 不入库，由 MRS 自动生成，
   产物 `obj/dtu-ethernet-ble-demo.hex`。`Bootloader/` 已在工程中排除，不会混入 App 构建。
4. **烧录接线（WCH-Link）**：

   | WCH-LINK | 板子 |
   |---|---|
   | 3V3 | VDD |
   | GND | GND |
   | SWDIO | PA13 |
   | SWCLK | PA14 |
   | RST  | RST（建议）|

5. **串口**：UART1(PA9) 115200 查看日志（连接/上报/心跳/查询应答）。

> App 链接在 `0x3000`，**不能直接烧到 0x0**。量产请先合并 Bootloader（见 5.4），
> 或先烧 Bootloader 再用串口 IAP 写入 App 包。

---

## 五、固件升级（OTA）

完整方案（分区规划、启动决策、串口协议、网络 OTA 时序）见
[`how-to-upgrade-firmware.html`](how-to-upgrade-firmware.html)；
动手操作（编译、烧写、打包、串口/网络升级、故障排查）见
[`how-to-build-flash.md`](how-to-build-flash.md)
（网页版 [`how-to-build-flash.html`](how-to-build-flash.html)）。

### 5.1 Flash 分区（已冻结）

| 分区 | 地址 | 大小 | 用途 |
|---|---|---|---|
| Bootloader | `0x00000–0x02FFF` | 12 KB | 启动决策、串口 IAP、Staging→App 搬运 |
| App | `0x03000–0x27FFF` | 148 KB | 应用镜像（`Ld/Link.ld`） |
| Meta | `0x28000–0x29FFF` | 8 KB | OTA 状态记录，两页轮换 |
| Staging | `0x2A000–0x51FFF` | 160 KB | 接收的固件包（含 512 B 包头） |
| Backup | `0x52000–0x77FFF` | 152 KB | 预留，本期未使用 |

分区常量在 `User/ota_layout.h`；改布局必须同时改 `Ld/Link.ld`、`Bootloader/Link_boot.ld`。

### 5.2 构建 Bootloader

将 MounRiver 工具链加入 `PATH`（安装目录下
`.../WCH/Toolchain/RISC-V Embedded GCC/bin`），然后：

```sh
make -C Bootloader
```

产物 `Bootloader/build/bootloader.{hex,bin,elf}`；Makefile 带 size gate，超过 12 KB 直接失败。
App 仍用 MounRiver IDE 构建（新增的 `User/ota_*.c` 位于工程源目录中，自动参与编译）。

### 5.3 生成固件包

先用 `User/ota_config.h` 的 `OTA_FW_VERSION` 定义新版本并重新编译，再打包：

```sh
python3 tools/mkfw.py --hex obj/dtu-ethernet-ble-demo.hex \
    --version 0.2.0 --key tools/keys/dev_private.pem \
    --pub tools/keys/dev_public.der -o build/dtu-0.2.0.dtufw
```

包结构：512 B 包头（`DTUFW` 魔数、文件/镜像大小、CRC32、版本、hw_id、SHA-256、RSA-2048 签名）
+ 256 B 对齐镜像。签名对象为 `SHA-256(header[0x000..0x040) || image)`。
`--version` 必须与编译进固件的 `OTA_FW_VERSION` 一致（不一致时工具告警）；
镜像超过 148 KB 直接失败。

> 仓库只带开发公钥（`User/ota_pubkey.h`）。私钥 `tools/keys/*.pem` 已被 `.gitignore`
> 忽略；量产前必须替换为生产密钥对，并另行保管私钥。

### 5.4 产线合并烧录

```sh
python3 tools/merge_hex.py Bootloader/build/bootloader.hex \
    obj/dtu-ethernet-ble-demo.hex -o build/dtu-full.hex
```

工具校验 Boot < `0x3000`、App 在 `0x3000..0x27FFF` 后合并，用 WCH-Link 一次烧录。

> **首次上电**：合并 hex 不含 Meta 区，首次启动 Meta 为 EMPTY，Bootloader 会**无限等待
> 串口升级**，不会自行进入 App。产线烧录后需用 `ota_uart.py` 走一次串口升级
> （升级流程会写入 Meta），或由产测工具写入 APP_VALID 记录。

### 5.5 串口升级（USART1，115200）

```sh
python3 tools/ota_uart.py /dev/tty.usbserial-XXXX build/dtu-0.2.0.dtufw
```

工具持续发送 HELLO，请在启动工具后复位设备（App 侧置 BOOT_UART 后复位亦可）。
帧格式 `A5 5A` + CRC16/CCITT-FALSE，256 B 分页逐页 ACK；END 后 Bootloader 校验整包
CRC32/SHA-256、搬运、校验 App、跳转（Bootloader 不验 RSA，串口升级只保证完整性）。
RECOVERY/EMPTY 状态下无限等待，随时可重试。

### 5.6 网络 OTA

前提：设备只有 **一个 TCP socket**。收到 WS 二进制通知（`DTUO` TLV：文件大小、版本、
HTTPS URL、CRC16）后，App 断开 WS/TLS，复用 socket 连接固件服务器，HTTPS GET 下载到
Staging，校验 CRC32/SHA-256 与 RSA 签名，写 Meta=STAGED 后复位；Bootloader 搬运并
启动新固件。下次 WS 连接建立时上报 `ota=done` 或 `ota=failed;code=N`。

评审确认的约束：

- 下载只允许 `https://`（URL 解析拒绝明文 http）；
- 固件 URL 不做域名白名单校验；
- TLS 不校验服务器证书（`MBEDTLS_SSL_VERIFY_NONE`），固件完整性由包签名保证；
- 默认拒绝降级，通知/包带 allow-downgrade 位时除外；
- `Content-Length` 必须等于通知中的文件大小，不支持 chunked/Range/302。

### 5.7 升级状态与恢复

- 新固件启动后 30 s 内未确认，尝试计数 +1；累计 5 次未确认后，下次启动进入 RECOVERY，
  停在串口等待升级。
- 搬运过程中掉电不影响 Staging：Meta 停在 COPYING，下次启动重新搬运并校验。
- RECOVERY/EMPTY：无限等待串口 IAP。

---

## 六、wss 的 TLS（mbedTLS）

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

## 七、协议摘要（对齐服务端 DeviceYedS710YmProcessor）

- 地址：`wss(s)://host/v1/{org}/{model}/{IMEI}`，`org=yinerda`、`model=yed-s710ym`
- 心跳（二进制，每 20s）：`0a 0b 0c 0d 0e 0f`
- 信号强度（文本帧）：`csq=31`
- 定位（文本帧）：`coordinates=经度,纬度`（第一个值是**经度**）
- 服务端请求（二进制，任意内容）→ 回固定数据帧 `DTU_DEMO_REPLY_FRAME`（默认 `01 03 02 00 64 b9 af`）
- IMEI 已在 URL 路径中，协议内无需单独上报

---

## 八、许可证

见根目录 `LICENSE`（本项目基于 WCH CH32V20x 官方库扩展）。