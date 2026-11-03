# 编译、烧写与固件升级指南

本文覆盖：**环境准备 → 编译 App / Bootloader → 打固件包 → 烧写 → 串口升级 → 网络 OTA → 恢复**。
分区规划与协议设计细节见 [`how-to-upgrade-firmware.html`](how-to-upgrade-firmware.html)。

当前版本：Bootloader `0.1.0`、App `0.1.0`，分区布局 v0.4（480 KB Flash）。

---

## 0. 快速开始

```sh
# 1) 一键构建（脚本自动定位 MRS 工具链；App 需先在 MRS 里构建过一次，生成 obj/makefile）
./build-all.sh
#    产物：build/dtu-full.hex（Boot+App）、build/dtu-<ver>.dtufw（签名固件包）

#    也可分步执行：
#    ./build-bootloader.sh   # -> Bootloader/build/bootloader.hex
#    ./build-app.sh          # -> obj/dtu-ethernet-ble-demo.hex
#    ./merge-image.sh        # -> build/dtu-full.hex
#    ./pack-firmware.sh      # -> build/dtu-<OTA_FW_VERSION>.dtufw；--key 换签名私钥

# 2) 产线/首次烧录：用 MRS 的 Download 烧 build/dtu-full.hex，
#    或用 7.2.3 的 OpenOCD 命令行

# 3) 串口升级（先运行工具，再复位设备）
python3 tools/ota_uart.py /dev/tty.usbserial-XXXX build/dtu-0.1.0.dtufw
```

> **首次上电必读**：合并 hex 不含 OTA Meta 区。新板烧完重启时 Meta 为 `EMPTY`，
> Bootloader 会**无限等待串口升级**，不会自行进入 App。必须再用第 3 步刷一次固件包
> （升级过程会写入 Meta 与 App），设备才会正常运行。见 [7.2.6](#726-为什么初次烧完还要串口-iap)。

各脚本的参数（`--clean`、`--version`、`--key` 等）用 `./<脚本> --help` 查看；
手动命令的完整说明见第 3–6 节。

---

## 1. 环境准备

| 工具 | 用途 | 说明 |
|---|---|---|
| MounRiver Studio 3 | 打开/编译 App；下载（烧写） | 内置 WCH RISC-V 工具链与 WCH-Link 支持 |
| Python 3.8+ | `tools/*.py` 打包、合并、串口升级 | 仅标准库 + `pyserial` |
| pyserial | `tools/ota_uart.py` | `python3 -m pip install pyserial` |
| openssl | 生成/导出 RSA 密钥、验签自检 | macOS/Linux 自带；Windows 可用 Git Bash 自带版本 |

### 1.1 RISC-V 工具链

Bootloader 用 Makefile 构建，需要 `riscv-none-embed-gcc` 在 `PATH` 中。
MounRiver Studio 3 安装目录下：

```
macOS:   <MRS 3.app>/Contents/Resources/app/resources/darwin/components/WCH/Toolchain/RISC-V Embedded GCC/bin
Windows: <MRS 安装目录>\resources\app\resources\win32\components\WCH\Toolchain\RISC-V Embedded GCC\bin
Linux:   <MRS 安装目录>/resources/app/resources/linux/components/WCH/Toolchain/RISC-V Embedded GCC/bin
```

macOS 下可直接：

```sh
export MRS_BIN="/Applications/MounRiver Studio 3.app/Contents/Resources/app/resources/darwin/components/WCH/Toolchain/RISC-V Embedded GCC/bin"
export PATH="$MRS_BIN:$PATH"
riscv-none-embed-gcc --version     # 能打印版本即配置成功
```

（可把上面两行写进 `~/.zshrc`。）

### 1.2 Python 依赖

```sh
python3 -m pip install pyserial
python3 -c "import serial; print(serial.__version__)"
```

未安装 `pyserial` 时 `ota_uart.py` 仍可 `--help`，但实际升级会提示安装后退出。

---

## 2. 工程结构

| 路径 | 说明 |
|---|---|
| 仓库根目录 | **App 工程**（`.project` / `.cproject`），MRS 打开、构建的对象 |
| `obj/` | MRS 构建输出（`*.hex/.elf/.map` 与自动生成的 `makefile`），已忽略，不入库 |
| `Bootloader/` | **独立 Bootloader 工程**，用 `Bootloader/Makefile` 构建 |
| `Ld/Link.ld` | App 链接脚本，起始 `0x3000` |
| `Bootloader/Link_boot.ld` | Bootloader 链接脚本，起始 `0x0`，大小 12 KB |
| `User/ota_*.c/h` | OTA 公共代码：分区、Meta、包解析、CRC/SHA、串口协议、HTTPS |
| `tools/` | 打包、合并、串口升级主机端 |
| `build/` | 本地产物（`.dtufw`、合并 hex），已忽略 |
| `mbedtls/` | mbedTLS 2.28 源码（wss 与 OTA 验签用） |

> App 工程里已通过 `.project` 的 folder filter 和 `.cproject` 的 `excluding` 排除
> `Bootloader/`，避免 `boot_main.c` 的 `main()` 混进 App。不要删除这两处配置，
> 也不要把 `Bootloader/` 加进 App 的源文件列表。

---

## 3. 编译 App

App 是当前仓库根目录的 MRS 工程。

### 3.1 用 MounRiver Studio 3 打开并构建（推荐）

MRS 3 基于 VS Code 界面，不是 Eclipse 的 Import 流程：

1. 启动 MounRiver Studio 3。
2. 打开工程：**`File → Open Folder…`** 选择本仓库根目录；或按 `Cmd/Ctrl+Shift+P`
   打开命令面板，执行 **`Open MRS Project`**（欢迎页也有相同入口）。
3. 左侧 **Project Explorer** 中出现 `dtu-ethernet-ble-demo` 工程，展开可见 `User/`、`Net/`、`mbedtls/` 等。
4. 右键工程 → **`Build Project`**；改过配置或换版本时用 **`ReBuild Project`**。
5. 成功后在 `obj/` 得到：
   - `dtu-ethernet-ble-demo.hex`（烧写/打包用）
   - `dtu-ethernet-ble-demo.elf`（调试用）
   - `dtu-ethernet-ble-demo.map`（查看内存占用）

首次构建会生成 `obj/makefile`。该文件包含本机绝对路径、由 MRS 维护，**不要手工编辑，也不要提交**。

### 3.2 命令行构建（不依赖 MRS，推荐）

仓库根目录的 `Makefile` 由仓库维护，源文件与编译参数都来自仓库内容，**不需要** MRS 生成的 `obj/`：

```sh
make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
make size            # 只检查 text+data 是否超过 148 KB
make clean
```

一次 `make` 会构建 App 与 Bootloader，并生成两个烧写产物：

| 产物 | 内容 | 用途 |
|---|---|---|
| `build/dtu-full.hex` | Bootloader（`0x0`）+ App（`0x3000`），不含 Meta | **首次** SWD/WCH-Link 烧写 |
| `build/dtu-<版本>.dtufw` | 512 B 包头 + 仅 App + RSA 签名 | **后续升级**：串口 IAP / 网络 OTA |

`.dtufw` 只装 App：升级时由设备上已有的 Bootloader 接收并搬运，Bootloader 本身不参与 OTA。
签名私钥默认依次找 `prod_private.pem`、`tools/keys/prod_private.pem`、
`tools/keys/dev_private.pem`；都没有时 `make` 只跳过 `.dtufw`，其余照常构建。
也可只做其中一步：

```sh
make image                 # 只合并 build/dtu-full.hex
make fw FW_KEY=prod_private.pem          # 只打包签名固件
make fw FW_REQUIRE_KEY_MATCH=1           # 私钥与内嵌公钥不一致时直接失败
```

产物都在 `build/`，不会覆盖 `obj/` 里的 MRS 构建结果。

前提是 `riscv-none-embed-gcc` 在 `PATH` 中；也可显式指定前缀：

```sh
make CROSS="/Applications/MounRiver Studio 3.app/Contents/Resources/app/resources/darwin/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-"
```

GitHub Actions（见 6.1）用的就是这条路径。

### 3.3 用 MRS 生成的 `obj/` 构建

前提：MRS 已成功构建过至少一次（`obj/makefile` 已生成），且工具链在 `PATH` 中。

```sh
make -C obj all      # 增量构建，等价于 MRS 的 Build
make -C obj clean    # 清理 App 目标文件

./build-app.sh       # 等价封装：定位工具链、构建并校验 0x3000 起始/148 KB 上限
```

> `obj/makefile` 里的路径是绝对路径，仓库换目录后需在 MRS 里重建。
> 只想命令行构建、不想开 MRS 时，用 3.2 的 `make` 即可。

App 链接上限 148 KB（`0x3000–0x27FFF`），超过会链接失败。
当前实现约 137 KB Flash / 21 KB bss，余量约 11 KB。

---

## 4. 编译 Bootloader

```sh
make bootloader             # 等价于 make -C Bootloader
make -C Bootloader          # 构建 + 自动检查 12 KB 上限
make -C Bootloader size     # 只打印体积并检查上限
make -C Bootloader clean    # 清理

./build-bootloader.sh       # 等价封装：定位工具链，并校验 hex 位于 0x0–0x2FFF
```

产物：

| 文件 | 用途 |
|---|---|
| `Bootloader/build/bootloader.hex` | 合并/烧写 |
| `Bootloader/build/bootloader.bin` | 产测工具/其他烧写方式 |
| `Bootloader/build/bootloader.elf` | 调试 |
| `Bootloader/build/bootloader.map` | 体积分析 |

Makefile 内置 size gate：`text + data` 超过 12288 字节直接报错退出。
当前约 9060 字节。

Bootloader 与 App 共用 `User/ota_crc.c`、`ota_sha256.c`、`ota_meta.c`、`ota_pkg.c`，
改动这些文件后**两边都要重新编译**。Bootloader 不链接 WCHNET/mbedTLS，也不做 RSA 验签。

---

## 5. 版本号、hw_id 与密钥

### 5.1 固件版本

`User/ota_config.h`：

```c
#define OTA_HW_ID      0x00000001u
#define OTA_FW_VERSION OTA_MAKE_VERSION(0, 1, 0)
#define OTA_BL_VERSION OTA_MAKE_VERSION(0, 1, 0)
```

- 编码：`major << 16 | minor << 8 | patch`，文本显示为 `major.minor.patch`。
- 发布新版本：改 `OTA_FW_VERSION` → **重新编译 App** → 打包时 `--version` 用同一个值。
  走 GitHub Actions（6.1）时不用手改：工作流用 tag 版本覆盖 `OTA_FW_VERSION`。
- 版本不一致时 `mkfw.py` 会打印警告；升级后版本没变化的常见原因就是这里没改或没重编。
- 降级默认被拒绝（串口 BEGIN 与网络通知都会比较版本），确有需要时用
  `ota_uart.py --allow-downgrade` 或 `mkfw.py --allow-downgrade`。

### 5.2 hw_id

`OTA_HW_ID` 编译进 App 与 Bootloader，用于拒绝“别的机型”的包：

- 打包默认 `--hw-id 1`（与 `User/ota_config.h` 一致）；
- 串口 BEGIN 后 Bootloader 用 `ota_pkg_check()` 校验包头 hw_id；
- 网络 OTA 在 App 解析包时校验；
- 设备端与包不一致返回 `OTA_ST_HW_MISMATCH (10)`。

改机型/硬件版本时同时改 `User/ota_config.h` 与打包命令的 `--hw-id`。

### 5.3 密钥

仓库只带**开发用**公钥 `User/ota_pubkey.h` 和 `tools/keys/dev_public.der`；
私钥 `tools/keys/dev_private.pem` 已被 `.gitignore` 忽略，**禁止提交任何私钥**。

量产替换为生产密钥对：

```sh
openssl genrsa -out prod_private.pem 2048
openssl rsa -in prod_private.pem -pubout -outform DER -out prod_public.der
python3 tools/pubkey2h.py prod_public.der -o User/ota_pubkey.h
```

然后**重新编译 App**（Bootloader 不用改，它不验签）。私钥只在发布机/CI 密钥库保存，
发布时用 `mkfw.py --key prod_private.pem` 签名；可加 `--pub prod_public.der` 让工具打包后自检签名。

> 安全边界：**网络 OTA 在 App 侧验 RSA 签名**；串口 IAP 只校验 CRC32/SHA-256
> （Bootloader 不含 RSA），因此串口升级只保证完整性、不保证来源，依赖物理接触设备。
> Bootloader 的写保护（WRP）当前未启用，见设计文档。

---

## 6. 生成固件包 `.dtufw`

```sh
./pack-firmware.sh                 # 版本取 OTA_FW_VERSION，默认 dev 私钥
./pack-firmware.sh --version 0.2.0 --key prod_private.pem
```

脚本会检查签名私钥是否与 `User/ota_pubkey.h` 内嵌公钥一致（网络 OTA 必须一致），
并自动用同名 `*_public.der` 做打包后验签自检。等价的底层命令：

```sh
python3 tools/mkfw.py --hex obj/dtu-ethernet-ble-demo.hex \
    --version 0.2.0 \
    --hw-id 1 \
    --key tools/keys/dev_private.pem \
    --pub tools/keys/dev_public.der \
    -o build/dtu-0.2.0.dtufw
```

| 参数 | 说明 |
|---|---|
| `--hex` / `--bin` | App 镜像（HEX 必须链接在 `0x3000`；`--bin` 从 `0x3000` 起） |
| `--version` | 必须与编译进 App 的 `OTA_FW_VERSION` 一致 |
| `--hw-id` | 默认 `1`，需与 `OTA_HW_ID` 一致 |
| `--key` | RSA-2048 私钥（PEM），必填 |
| `--pub` | 可选，打包后用公钥验签自检 |
| `--allow-downgrade` | 包头 bit0 置位，允许设备接受不高于当前的版本 |
| `-o` | 输出文件 |

包结构（512 B 包头 + 256 B 对齐镜像）：

| 偏移 | 内容 |
|---|---|
| `0x000` | magic `44 54 55 46 57 01 00 00`（`DTUFW` + 版本） |
| `0x008` | header_size = 512 |
| `0x00A` | flags（bit0 = allow downgrade） |
| `0x00C` | file_size（包头 + 镜像，256 的倍数） |
| `0x010` | image_size（≤ 148 KB） |
| `0x014` | image CRC32 |
| `0x018` | version |
| `0x01C` | hw_id |
| `0x020` | image SHA-256 |
| `0x040` | RSA-2048 PKCS#1 v1.5 签名（256 B） |
| `0x140–0x1FF` | 保留 `0xFF` |

签名对象：`SHA-256(header[0x000..0x040) || image)`。
镜像超过 148 KB、或 hex 不在 `0x3000` 区段时工具直接失败。

### 6.1 GitHub Actions 自动发布（按 tag）

仓库带有 `.github/workflows/release.yml`。推送 **`vX.Y.Z`** 形式的 tag（如 `v0.0.1`）时自动：

1. 下载并缓存 MounRiver 官方 Linux 工具链（V1.92，无需本机 `obj/`）；
2. 把 tag 版本写入 `User/ota_config.h` 的 `OTA_FW_VERSION`，**固件内上报的版本就是 tag 版本**；
3. 写入 secret 中的签名私钥后执行 `make`：一次完成 App + Bootloader 编译、合并
   `build/dtu-full.hex`；
4. 同一次 `make` 用该私钥签名并自检，生成 `build/dtu-X.Y.Z.dtufw`；
5. 上传 Artifact 并创建同名 Release（附 `.dtufw` 与 `dtu-full.hex`）。

**一次性配置密钥**：GitHub 仓库 `Settings → Secrets and variables → Actions →
New repository secret`，名称 `FW_SIGNING_KEY`，值粘贴私钥 PEM 全文
（`-----BEGIN … PRIVATE KEY-----` 到 `-----END …-----`）。私钥必须与
`User/ota_pubkey.h` 内嵌公钥匹配，否则工作流会在密钥一致性检查处直接失败。

发布：

```sh
git tag -a v0.2.0 -m "fw 0.2.0"
git push origin v0.2.0
```

也可在 Actions 页面手动运行 `Release Firmware`（workflow_dispatch，填版本号 `X.Y.Z`）：
只构建并上传 Artifact，不创建 Release，用于上线前验证。

---

## 7. 烧写：初次 vs 后续

### 7.1 先搞清楚烧哪个文件

| 文件 | 内容 | 用途 |
|---|---|---|
| `build/dtu-full.hex` | Bootloader + App，**不含 Meta** | 初次烧写（SWD 一次写入） |
| `Bootloader/build/bootloader.hex` | 仅 Bootloader（`0x0–0x2FFF`） | 只安装/修复 Bootloader |
| `build/app/dtu-ethernet-ble-demo.hex` | 仅 App（`0x3000` 起） | 开发调试时 SWD 直接烧 App（MRS 构建则在 `obj/`） |
| `build/dtu-<ver>.dtufw` | 512 B 包头 + App 镜像 + 签名 | 串口 IAP / 网络 OTA 的唯一升级对象 |

记住两条原则：

- **初次烧写**：先用 SWD 写入 `dtu-full.hex`（或只写 `bootloader.hex`），再用 `.dtufw`
  走一次串口 IAP，写入 App 与 Meta。
- **后续烧写**：只用 `.dtufw`，优先网络 OTA，其次串口 IAP；不要再用 SWD 烧合并 hex，
  更不要整片擦除。

### 7.2 初次烧写（空片 / 新板 / 整片重装）

目标：`0x0` 有 Bootloader、`0x3000` 有 App、`0x28000` 有 Meta 记录。
有四种方式。**A / B / D 只写 Boot+App，Meta 仍为 EMPTY，必须再走一次串口 IAP**
（也就是方式 C 的第 4 步）；方式 C 本身就把 App 与 Meta 一起写入。

| 方式 | 烧写对象 | 工具 | 写 Meta | 适用场景 |
|---|---|---|---|---|
| A. MRS GUI | `build/dtu-full.hex` | MRS 3 + WCH-Link | 否 | 首次安装，推荐 |
| B. OpenOCD 命令行 | `build/dtu-full.hex` | MRS 自带 OpenOCD | 否 | 无 GUI、脚本化/批量 |
| C. 只烧 Boot + 串口 IAP | `bootloader.hex` → `.dtufw` | WCH-Link + USB-TTL | 是（IAP 写入） | 产线、手里没有 App hex |
| D. WCH-LinkUtility | `build/dtu-full.hex` | 需另装 WCH-LinkUtility | 否 | 已装该工具时可选 |

#### 7.2.1 WCH-Link 接线

| WCH-LINK | 板子 |
|---|---|
| 3V3 | VDD |
| GND | GND |
| SWDIO | PA13 |
| SWCLK | PA14 |
| RST | RST（建议接，下载器需要复位芯片） |

串口升级另需 USB-TTL：TX→PA10、RX→PA9、GND→GND（见 8.1）。

#### 7.2.2 方式 A：MRS GUI 烧合并 hex（推荐）

1. 生成合并 hex：

   ```sh
   python3 tools/merge_hex.py Bootloader/build/bootloader.hex \
       obj/dtu-ethernet-ble-demo.hex -o build/dtu-full.hex
   ```

   工具会校验 Boot < `0x3000`、App 位于 `0x3000–0x27FFF` 后合并。

2. 接好 WCH-Link，MRS 3 中右键工程 → **`Download Configuration`**：
   - `Target Path = build/dtu-full.hex`
   - `Erase All = true`（整片擦除，适合空片/重装）
   - `Program / Verify / Reset = true`

   这些设置保存在仓库根目录 `.template`（已被 git 跟踪）。

3. 点击 **`Download`** 烧写。

4. 复位后首次启动：Meta 仍是 `EMPTY`，Bootloader 无限等待串口升级。
   按方式 C 的第 4 步用 `ota_uart.py` 刷一次 `.dtufw`，写入 App 与 Meta。

> ⚠️ `Erase All` 是整片擦除。只烧 App hex 时若保持 `true`，会把 Bootloader 与 Meta
> 一起擦掉，设备无法启动。只有烧合并 hex / 只装 Bootloader 时才用 `true`。

#### 7.2.3 方式 B：OpenOCD 命令行烧合并 hex

MRS 3 安装目录自带 OpenOCD（WCH `wlinke` 适配器），不打开 IDE 也能烧。macOS 路径：

```sh
OC="/Applications/MounRiver Studio 3.app/Contents/Resources/app/resources/darwin/components/WCH/OpenOCD/OpenOCD/bin"
```

Windows/Linux 对应 `.../resources/app/resources/win32|linux/components/WCH/OpenOCD/OpenOCD/bin`。
先做只读连接检查：

```sh
"$OC/openocd" -f "$OC/wch-riscv.cfg" -c "init; halt; flash probe 0; exit"
```

正常应看到 `Target successfully examined.`、`device id = 0x...`、`flash size = ...kbytes`。
然后烧写合并 hex 并复位：

```sh
"$OC/openocd" -f "$OC/wch-riscv.cfg" \
    -c "init; halt; program build/dtu-full.hex verify reset; exit"
```

等价于 MRS 里 `Target Path=build/dtu-full.hex` + `Program/Verify/Reset`，但仍有 Meta=EMPTY
的问题，烧完同样要执行方式 C 的第 4 步。

限制与注意：

- 该 OpenOCD 的 flash bank 只有**前 160 KB**（`0x0–0x27FFF`）。合并 hex 写到约 `0x24xxx`，
  在范围内；Meta/Staging 在 `0x28000` 之后，**不能用 OpenOCD 写**，首次烧写也不需要。
- `program` 只擦除 hex 覆盖的扇区，不做 MRS 的 `Erase All`。
- OpenOCD 不设置芯片的 ROM/RAM 分配（128K+64K / 160K+32K 等）。新片或分配异常时，
  先在 MRS 里 `Download` 一次，之后再换命令行。
- 某些 WCH-Link 固件会打印 `The Chip type does not match`，甚至报告
  `ROM 128 kbytes RAM 64 kbytes`。若同时能 `Target successfully examined` 且
  flash 大小正确，可继续；否则先用 MRS/WCH-LinkUtility 确认芯片配置、更新 WCH-Link 固件。

#### 7.2.4 方式 C：只烧 Bootloader，再用串口 IAP 装 App（产线推荐）

这种方式 SWD 只写约 9 KB 的 Bootloader，App 与 Meta 都由串口 IAP 写入，适合产线批量。

1. 先构建 Bootloader（见第 4 节），得到 `Bootloader/build/bootloader.hex`。
2. MRS `Download Configuration`：
   - `Target Path = Bootloader/build/bootloader.hex`
   - 空片：`Erase All = true`；已有 App/Meta、只想修 Boot：`Erase All = false`
   - `Program / Verify / Reset = true`
3. `Download`。复位后 Meta 为 `EMPTY`，Bootloader 停在串口等待升级。
4. USB-TTL 接 PA9/PA10/GND，**先运行工具，再复位设备**：

   ```sh
   python3 tools/ota_uart.py /dev/tty.usbserial-XXXX build/dtu-0.1.0.dtufw
   ```

5. 工具握手后发送整包；Bootloader 校验 CRC32/SHA-256，写 Meta=`STAGED`，搬运到 App，
   再校验后跳转。至此 `0x0`、`0x3000`、`0x28000` 都有内容，设备正常运行。

#### 7.2.5 方式 D：WCH-LinkUtility（可选）

如果机器上装了 WCH 官方的 WCH-LinkUtility（仓库不提供，需自行安装）：

1. 连接 WCH-Link，选择芯片系列 `CH32V208`；
2. 打开 `build/dtu-full.hex`；
3. 勾选 `Erase All`（空片/重装）、`Program`、`Verify`、`Reset`；
4. 点击 `Program` 下载。

同样只写 Boot+App，烧完 Meta 仍为 EMPTY，需要按方式 C 第 4 步走一次串口 IAP。
不同版本工具界面可能略有差异，以实际界面为准。

#### 7.2.6 为什么初次烧完还要串口 IAP

合并 hex 不包含 Meta 区，也不包含 Staging/Backup。Bootloader 的判断完全依赖 Meta：

- Meta=`EMPTY` → **无限等待串口升级**，不会自行跳 App；
- 串口 IAP 完成后写 `STAGED`，搬运成功再写 `APP_VALID`，设备才会正常启动。

如果产测工具能按 `ota_meta_t` 格式直接写入一条 `APP_VALID` 记录，也可以代替串口 IAP；
否则串口 IAP 是最简单且能顺带验证升级链路的方式。

### 7.3 后续烧写（已有 Bootloader 的板子）

原则：**只升级 `.dtufw`**，按现场条件选择网络 OTA 或串口 IAP。SWD 只用于开发调试
或修复 Bootloader。

| 方式 | 烧写对象 | 校验 | 适用 |
|---|---|---|---|
| A. 网络 OTA | `.dtufw`（HTTPS 下载） | CRC32 + SHA-256 + RSA-2048 | 现网批量升级，推荐 |
| B. 串口 IAP | `.dtufw`（本地串口） | CRC32 + SHA-256 | 现场/产线/救砖，推荐 |
| C. SWD 只烧 App | `obj/dtu-ethernet-ble-demo.hex` | 无（绕过升级校验） | 开发调试 |
| D. SWD 更新 Bootloader | `Bootloader/build/bootloader.hex` | 无 | Boot 修复/升级，少见 |

#### 7.3.1 方式 A：网络 OTA（量产推荐）

- 烧哪个：服务器上托管的 `.dtufw`，设备通过 HTTPS 下载到 Staging，
  再由 Bootloader 搬运到 App。**不需要 SWD，也不需要人到现场。**
- 怎么触发：服务端在设备已连接的 WS 上发二进制 `DTUO` 通知（URL、大小、版本、CRC16），
  设备回 `ota=starting`，断开 WS 后下载、验签、写 Meta、复位、搬运。
- 结果：下次 WS 连接建立时设备上报 `fw=x.y.z;hw=XXXXXXXX;ota=done`，
  失败则 `ota=failed;code=N`（N 见状态码表）。
- 完整 TLV 格式、服务器要求见第 9 节。

#### 7.3.2 方式 B：串口 IAP（现场/产线推荐）

- 烧哪个：`build/dtu-<ver>.dtufw`（不是 hex）。
- 怎么烧：USB-TTL 接 PA9/PA10/GND，先运行工具、再复位设备：

  ```sh
  python3 tools/ota_uart.py /dev/tty.usbserial-XXXX build/dtu-0.2.0.dtufw
  ```

- 进入条件：Meta 为 `EMPTY`/`RECOVERY` 时无限等待；`BOOT_UART` 等 30 s；
  `APP_VALID` 每次启动只留 500 ms 窗口，所以顺序必须是“先开工具，再复位”。
- 默认拒绝降级；确需降级加 `--allow-downgrade`。
- 协议、状态码与故障处理见第 8 节。

#### 7.3.3 方式 C：SWD 只烧 App（仅开发调试）

1. MRS `Download Configuration`：
   - `Target Path = obj/dtu-ethernet-ble-demo.hex`
   - **`Erase All = false`**（否则 Bootloader 与 Meta 被擦除）
   - `Program / Verify / Reset = true`
2. `Download`，复位。

注意：这条路径绕过版本检查、包 CRC/SHA 与 RSA 验签，只适合开发调试。App 启动 30 s 后
会把 Meta 的版本更新为实际运行版本；若新 App 起不来，累计 5 次后进入 `RECOVERY`。

#### 7.3.4 方式 D：SWD 更新 Bootloader（少见）

- `Target Path = Bootloader/build/bootloader.hex`，`Erase All = false`，
  只擦写 `0x0–0x2FFF`，App 与 Meta 保持不变。
- Bootloader 与 App 共用 `User/ota_*.c` 的 Meta/包格式；升级 Boot 时保持两者兼容，
  改分区或 Meta 格式必须整片重装。
- 更新后复位，Bootloader 按现有 Meta 继续启动 App。

### 7.4 怎么选：一张表

| 场景 | 烧什么 | 方式 |
|---|---|---|
| 全新板/空片，首次安装 | `build/dtu-full.hex` + 一次 `.dtufw` | A（有 GUI）或 B（脚本），再串口 IAP |
| 产线批量首次安装 | `bootloader.hex` + `.dtufw` | C |
| 现网设备升级 | `.dtufw`（服务器） | A 网络 OTA |
| 现场无网络/救砖 | `.dtufw` | B 串口 IAP |
| 开发调试改 App | `obj/...hex` | C SWD 只烧 App（`Erase All=false`） |
| 修 Bootloader | `bootloader.hex` | D SWD 只烧 Boot（`Erase All=false`） |

---

## 8. 串口升级（USART1）

### 8.1 接线与参数

| USB-TTL | 板子 |
|---|---|
| RX | PA9（USART1_TX） |
| TX | PA10（USART1_RX） |
| GND | GND |
| 3V3 | VDD（可选，板子可独立供电时不要接） |

- 波特率 **115200**，8N1，无流控。
- 该串口同时是 App 的运行日志口；App 正常运行时不会响应 IAP 帧。

### 8.2 如何进入升级状态

| Meta 状态 | Bootloader 行为 |
|---|---|
| `EMPTY` / `RECOVERY` | **无限等待**串口升级 |
| `BOOT_UART` | 等 HELLO **30 s**，超时按 `APP_VALID` 处理 |
| `APP_VALID` | 每次启动留 **500 ms** 窗口；收到 HELLO 就升级，否则 attempts+1 后跳 App |
| `STAGED` / `COPYING` | 先校验并搬运，不等待串口 |

因此标准操作是：**先运行 `ota_uart.py`（它持续发 HELLO），再复位设备**。
500 ms 窗口较短，先启动工具、再按复位键即可稳定握手。

> App 中已有 `ota_app_request_serial()`（写 `BOOT_UART` 后复位），但当前没有 WS 命令
> 调用它。需要时可自行接一个文本命令（如收到 `ota-serial` 时调用），即可让设备
> 在远程指令下进入 30 s 的串口等待。

### 8.3 使用 `ota_uart.py`

```sh
python3 tools/ota_uart.py /dev/tty.usbserial-XXXX build/dtu-0.1.0.dtufw
```

macOS 串口通常是 `/dev/tty.usbserial-*` 或 `/dev/tty.usbmodem*`；Linux 为 `/dev/ttyUSB0`。

| 参数 | 默认 | 说明 |
|---|---|---|
| `--baud` | 115200 | 必须与固件一致 |
| `--hello-timeout` | 15 s | 等待 HELLO_ACK 的总时长（期间每 50 ms 发一次 HELLO） |
| `--ack-timeout` | 5 s | 单帧 ACK 超时 |
| `--allow-downgrade` | 关 | BEGIN 时置降级标志 |

典型输出：

```
固件 : build/dtu-0.2.0.dtufw
版本 : 0.2.0  hw_id=0x00000001  镜像=137676 B  CRC32=0x1A2B3C4D
HELLO: 等待 Bootloader（请现在复位设备）…
Boot : 0.1.0  hw_id=0x00000001  app=0.1.0 state=1 staged=0 chunk=256
BEGIN: 接受
DATA : 100%  138240 B，用时 12.3s (11.0 KB/s)
END  : 校验通过，Bootloader 正在搬运并重启…
```

失败时工具会主动发 `ABORT` 结束会话并返回非 0；若整包校验失败，Bootloader 回错误码并结束
本次会话（可重试）；若搬运或搬运后校验失败则进入 `RECOVERY`，修好固件后重跑即可
（`RECOVERY` 状态无限等待）。

### 8.4 帧格式与状态码

```
A5 5A | ver(1)=01 | type(1) | flags/status(2 LE) | offset(4 LE) |
len(2 LE) | data(len ≤ 256) | CRC16(2 LE，覆盖 ver..data)
```

- CRC16：CCITT-FALSE，初值 `0xFFFF`，多项式 `0x1021`。
- 数据按 **256 字节页**发送，`offset` 必须等于已接收长度，每页一个 `DATA_ACK`。

| type | 名称 | 方向 |
|---|---|---|
| `0x01` / `0x81` | HELLO / HELLO_ACK | 主机 / Bootloader |
| `0x02` / `0x82` | BEGIN / BEGIN_ACK | 主机 / Bootloader |
| `0x03` / `0x83` | DATA / DATA_ACK | 主机 / Bootloader |
| `0x04` / `0x84` | END / END_ACK | 主机 / Bootloader |
| `0x05` | ABORT | 主机 |
| `0x06` | RESET | 主机 |

- BEGIN 载荷 14 B：`file_size u32 | file_crc32 u32 | version u32 | flags u16`。
- END 后 Bootloader 校验整包 CRC32、镜像 CRC32、SHA-256 与包头：不符则回 `END_ACK`
  状态码并结束本次会话；通过则写 Meta `STAGED`，搬运到 App 并再校验一次，成功后跳转，
  搬运/再校验失败进入 `RECOVERY`。

| 状态码 | 名称 | 常见原因 |
|---|---|---|
| 0 | OK | |
| 1 | BUSY | 设备忙/正在处理 |
| 2 | BAD_FRAME | CRC16 错、帧长度错 |
| 3 | BAD_STATE | 未 BEGIN 就发 DATA/END |
| 4 | NO_SPACE | 包内 image_size 超过 App 分区上限（148 KB） |
| 5 | BAD_SIZE | 大小非 256 倍数、单页长度错、offset 跳变 |
| 6 | FLASH_ERR | Flash 擦写失败 |
| 7 | CRC_MISMATCH | 整包/镜像 CRC 或 SHA-256 不符 |
| 8 | BAD_SIGNATURE | 仅网络 OTA（App 侧 RSA 验签失败） |
| 9 | VERSION_REJECTED | 版本不高于当前，需 `--allow-downgrade` |
| 10 | HW_MISMATCH | 包 hw_id 与设备不符 |
| 11 | BAD_PACKAGE | 包头字段非法（magic/尺寸/保留位） |
| 12 | BAD_MAGIC | 包头 magic 不是 `DTUFW` |

---

## 9. 网络 OTA（HTTPS）

### 9.1 流程

1. App 保持 wss 连接；服务端下发**二进制通知**（`DTUO` TLV）。
2. App 校验通知，成功回文本 `ota=starting`，否则回 `ota=failed;code=N`。
3. 回调返回后 App 关闭 WS/TLS，复用**唯一的 TCP socket** 连接固件服务器。
4. HTTPS GET 下载整包到 Staging（`0x2A000`），要求 `Content-Length` 精确匹配。
5. 校验包 CRC32、SHA-256、版本/hw_id 与 RSA 签名，全部通过后写 Meta=`STAGED` 并复位。
6. Bootloader 校验 Staging → 搬运到 App → 再校验 → 跳转。
7. 下次 WS 连接建立时上报 `fw=x.y.z;hw=XXXXXXXX;ota=done`（失败则 `ota=failed;code=N`），
   随后清除待上报标志；同一次升级只报一次。

### 9.2 通知格式（WS binary，`DTUO` TLV）

| 偏移 | 长度 | 内容 |
|---|---|---|
| 0 | 4 | magic `"DTUO"`（`44 54 55 4F`） |
| 4 | 1 | 协议版本 = `1` |
| 5 | 1 | 类型 = `0x10`（OTA 通知） |
| 6 | 2 | flags（bit0 = allow downgrade），LE |
| 8 | 4 | file_size（`.dtufw` 总字节数），LE |
| 12 | 4 | version（`major<<16|minor<<8|patch`），LE |
| 16 | 2 | URL 长度 N（≤ 220），LE |
| 18 | N | 固件 URL，**必须 `https://`** |
| 18+N | 2 | CRC16/CCITT-FALSE，覆盖字节 `[4, 18+N)`（即 version..URL），LE |

总长度 `20+N`，限制在 `20..240` 字节。`file_size` 必须满足
`512 + 256 ≤ file_size ≤ 512 + 148 KB` 且 `(file_size - 512)` 是 256 的倍数。

生成通知的 Python 示例（服务端侧）：

```python
import struct

def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc

def ota_notify(url, file_size, version, flags=0):
    u = url.encode("ascii")
    assert u.startswith(b"https://") and len(u) <= 220
    body = b"DTUO" + bytes([1, 0x10]) + struct.pack("<HIIH", flags, file_size, version, len(u)) + u
    return body + struct.pack("<H", crc16(body[4:]))

# 版本 0.2.0 = 0x000200
frame = ota_notify("https://fw.example.com/dtu/dtu-0.2.0.dtufw", 152064, 0x000200)
# ws.send_binary(frame)
```

### 9.3 固件服务器要求

- **只支持 HTTPS**（URL 解析拒绝 `http://`）。TLS 1.2 客户端套件为
  `ECDHE-RSA-AES128-GCM-SHA256`，服务器证书需 RSA-2048；证书链记录不超过 4096 字节。
- 服务器**不校验证书**（`MBEDTLS_SSL_VERIFY_NONE`），完整性由包内 RSA 签名保证。
- 必须返回精确的 `Content-Length`；**不支持** chunked、Range、302/301 跳转。
- URL 最长 220 字符，可带 query；支持域名（走 DNS）或 IPv4 字面量（直连，跳过 DNS）。
- 下载期间设备只有一个 TCP socket，wss 会断开，升级完成后自动重连。
- 建议服务器提供 `HEAD` 或日志，便于确认设备确实完成了 GET。

### 9.4 安全约束（评审确认）

- 下载仅允许 `https://`；
- 固件 URL 不做域名白名单校验；
- TLS 不校验证书（`MBEDTLS_SSL_VERIFY_NONE`）；
- 默认拒绝降级，除非通知 flags bit0 与包 flags bit0 都允许；
- Bootloader 未启用 WRP 写保护；
- 私钥不入库，量产使用独立密钥对。

---

## 10. 启动决策与恢复

Bootloader 上电后读取 Meta，按状态决策：

| Meta 状态 | 行为 |
|---|---|
| `EMPTY` | 无限等待串口 IAP |
| `RECOVERY` | 无限等待串口 IAP |
| `STAGED` / `COPYING` | 校验 Staging（Meta VERIFIED + 包头 SHA/CRC/尺寸/版本一致）→ 搬运到 App → 校验 → `APP_VALID` 并跳转；Staging 无效则改回 `APP_VALID`，搬运失败进入 `RECOVERY` |
| `BOOT_UART` | 等 HELLO 30 s：收到则升级，超时转 `APP_VALID` |
| `APP_VALID` | attempts ≥ 5 或 App 向量表非法 → `RECOVERY`；否则留 500 ms HELLO 窗口，尝试次数 +1，跳 App |

App 启动 30 s 后调用 `confirm_running()`：写回 `APP_VALID`、attempts=0、版本以实际运行为准。
**累计 5 次上电都没撑过 30 s，下一次启动就进入 `RECOVERY`**（防“新固件起不来”死循环）。

恢复步骤：

1. USB-TTL 接 PA9/PA10 与 GND；
2. 运行 `python3 tools/ota_uart.py <port> <正常的 .dtufw>`；
3. 复位设备；工具握手后完成升级，Bootloader 搬运并跳转 App；
4. 若仍失败，检查包版本/hw_id/签名以及串口接线。

---

## 11. 故障排查

| 现象 | 原因 / 处理 |
|---|---|
| `riscv-none-embed-gcc: command not found` | 工具链未加入 `PATH`，见 1.1 |
| `make -C obj` 报 `No such file or directory` | 全新克隆没有 `obj/`，先在 MRS 中构建一次 |
| 链接报 `main` 重复定义 / 找不到 `boot_main.h` | `Bootloader/` 被加入 App 构建；恢复 `.project` filter 与 `.cproject` excluding |
| 链接报 `undefined reference to WCHNET_*` / `libwchnet.a` | `Net/NetLib` 路径被改；用 MRS 重新构建 |
| 烧录后完全无输出、串口无反应 | 只烧了 App hex 且 `Erase All=true`，Bootloader 被擦；烧 `build/dtu-full.hex` |
| 烧完合并 hex 后设备不运行、串口等升级 | 正常现象：Meta=`EMPTY`，执行一次串口 IAP |
| 串口工具报“等待 HELLO_ACK 超时” | 未在窗口内复位；或 TX/RX 接反；或设备处于 `APP_VALID` 且错过 500 ms 窗口。先运行工具再复位；必要时让设备进 `RECOVERY` |
| 串口 BEGIN 返回 9 | 包版本不高于当前版本；换更高版本或 `--allow-downgrade` |
| 串口返回 10 | 包 hw_id 与设备不符；检查 `--hw-id` 与 `OTA_HW_ID` |
| 串口返回 5/7 | 包损坏或大小不对；重新打包，避免用文本模式传输 |
| 网络 OTA 回复 `ota=failed;code=9` | 版本未提升；改 `OTA_FW_VERSION` 重编重打包 |
| 网络 OTA 卡住/超时 | 服务器返回 chunked/302、`Content-Length` 不符，或 TLS 套件/证书不匹配（见 9.3） |
| 升级后版本号没变 | `mkfw.py --version` 与 `OTA_FW_VERSION` 不一致；改版本、重编 App、重打包 |
| App 运行 30 s 内复位多次后进 RECOVERY | 正常保护；串口重新刷入稳定固件 |
| MRS 编译 Flash 超限 | App 上限 148 KB；用 `obj/*.map` 查体积，关闭不用的功能 |

---

## 12. 命令速查

```sh
# 工具链
export MRS_BIN="/Applications/MounRiver Studio 3.app/Contents/Resources/app/resources/darwin/components/WCH/Toolchain/RISC-V Embedded GCC/bin"
export PATH="$MRS_BIN:$PATH"

# 脚本（自动定位工具链；MRS_BIN/PATH 可省略）
./build-all.sh                                   # Boot + App + 合并 + 打包
./build-bootloader.sh                            # 仅 Bootloader
./build-app.sh                                   # 仅 App
./merge-image.sh                                 # build/dtu-full.hex
./pack-firmware.sh --version 0.2.0 --key tools/keys/dev_private.pem

# 编译（推荐，不依赖 obj/）
make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)"   # App + Boot + 两个烧写产物
make image                         # 只合并 build/dtu-full.hex
make fw FW_KEY=prod_private.pem    # 只打包 build/dtu-<ver>.dtufw

# 编译（MRS 的 obj/）
make -C Bootloader                 # Bootloader + 12 KB 检查
make -C obj all                    # App（需 MRS 先构建过）

# 打包与合并
python3 tools/mkfw.py --hex obj/dtu-ethernet-ble-demo.hex --version 0.2.0 \
    --key tools/keys/dev_private.pem --pub tools/keys/dev_public.der \
    -o build/dtu-0.2.0.dtufw
python3 tools/merge_hex.py Bootloader/build/bootloader.hex \
    obj/dtu-ethernet-ble-demo.hex -o build/dtu-full.hex

# 密钥
openssl genrsa -out prod_private.pem 2048
openssl rsa -in prod_private.pem -pubout -outform DER -out prod_public.der
python3 tools/pubkey2h.py prod_public.der -o User/ota_pubkey.h

# 串口升级（先运行工具，再复位设备）
python3 tools/ota_uart.py /dev/tty.usbserial-XXXX build/dtu-0.2.0.dtufw
python3 tools/ota_uart.py /dev/tty.usbserial-XXXX build/dtu-0.1.0.dtufw --allow-downgrade
```

---

## 附录：Flash 分区（已冻结）

| 分区 | 地址 | 大小 | 用途 |
|---|---|---|---|
| Bootloader | `0x00000–0x02FFF` | 12 KB | 启动决策、串口 IAP、Staging→App 搬运 |
| App | `0x03000–0x27FFF` | 148 KB | 应用镜像（`Ld/Link.ld`） |
| Meta | `0x28000–0x29FFF` | 8 KB | OTA 状态记录，两个 4 KB 页轮换 |
| Staging | `0x2A000–0x51FFF` | 160 KB | 接收的固件包（含 512 B 包头） |
| Backup | `0x52000–0x77FFF` | 152 KB | 预留，本期未使用 |

分区常量在 `User/ota_layout.h`。修改布局必须同步 `Ld/Link.ld`、`Bootloader/Link_boot.ld`
与本文档，并重新评估打包/升级工具的上限。
