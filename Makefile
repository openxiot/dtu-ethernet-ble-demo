################################################################################
# App 便携构建（不依赖 MounRiver Studio 生成的 obj/）
#
# 编译器：MounRiver / xPack 的 riscv-none-embed- 前缀工具链（GCC 8.2，
#         需支持 -march=rv32imacxw 与 WCH-Interrupt-fast 中断属性）。
#         可用 CROSS 覆盖前缀，或把工具链 bin 目录加进 PATH。
#
# 用法：
#   make -j$(nproc)      # 一键：App + Bootloader + 两个烧写产物（macOS 用
#                        #   $(sysctl -n hw.ncpu)）
#   make app             # 只构建 App：build/app/dtu-ethernet-ble-demo.{elf,hex,bin}
#   make bootloader      # 只构建 Bootloader（等价 make -C Bootloader）
#   make image           # 只合并首次烧写镜像 build/dtu-full.hex
#   make fw              # 只打包升级固件 build/dtu-<ver>.dtufw
#   make size            # 只检查 App 的 148 KB 分区上限
#   make clean           # 清理 App 与 Bootloader 的构建产物
#
# 产物用途：
#   dtu-full.hex      Bootloader + App，首次 SWD/WCH-Link 烧写用；
#                     不含 Meta，首次上电后需再用 .dtufw 走一次串口 IAP。
#   dtu-<ver>.dtufw   512 B 包头 + 仅 App 镜像 + RSA 签名，串口 IAP / 网络 OTA 用。
#                     Bootloader 不参与 OTA（无备份区、不能自升级），所以只装 App。
#
# 变量：
#   FW_KEY=<PEM>           签名私钥。默认依次查找 prod_private.pem、
#                          tools/keys/prod_private.pem、tools/keys/dev_private.pem；
#                          都没有时只跳过 .dtufw，其余照常构建。
#   FW_VERSION=X.Y.Z       默认取 User/ota_config.h 的 OTA_FW_VERSION
#   FW_REQUIRE_KEY_MATCH=1 私钥与 User/ota_pubkey.h 不一致时报错（发布用）
#
# 输出目录与 obj/ 分离，不会覆盖 MRS 工程的构建产物。
################################################################################

CROSS    ?= riscv-none-embed-
CC       := $(CROSS)gcc
OBJCOPY  := $(CROSS)objcopy
SIZE     := $(CROSS)size

TARGET   := dtu-ethernet-ble-demo
BUILD    := build/app
OBJDIR   := $(BUILD)/obj
MAP      := $(BUILD)/$(TARGET).map
FLASH_MAX := 151552   # App 分区 0x03000..0x27FFF

APP_HEX  := $(BUILD)/$(TARGET).hex
BOOT_HEX := Bootloader/build/bootloader.hex
FULL_HEX := build/dtu-full.hex

# 打包私钥：优先发布私钥，其次开发私钥；都没有时跳过 .dtufw。
FW_KEY     ?= $(firstword $(wildcard prod_private.pem tools/keys/prod_private.pem tools/keys/dev_private.pem))
# 留空则由 pack-firmware.sh 读取 User/ota_config.h 的 OTA_FW_VERSION。
FW_VERSION ?=

ARCH     := -march=rv32imacxw -mabi=ilp32 -msmall-data-limit=8 -msave-restore
# 与 MRS 生成的 .compile_c_flags 保持一致
CFLAGS   := $(ARCH) -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char \
            -ffunction-sections -fdata-sections -fno-common -Wunused -Wuninitialized \
            -g -DDEBUG=1 -MMD -MP -std=gnu99 \
            -I Debug -I Core -I User -I Peripheral/inc -I Net/NetLib -I Net \
            -I mbedtls/include
ASFLAGS  := $(ARCH) -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char \
            -ffunction-sections -fdata-sections -fno-common -g \
            -x assembler-with-cpp -I Startup -I Net/NetLib
LDFLAGS  := $(ARCH) -T Ld/Link.ld -nostartfiles -Xlinker --gc-sections \
            -L Net/NetLib -Wl,-Map=$(MAP) --specs=nano.specs --specs=nosys.specs
LIBS     := -lwchnet -lm

# 源文件按 MRS subdir.mk 的包含顺序排列：链接顺序会影响 --gc-sections 后的
# 代码布局（jal/j 放松），保持与 MRS 构建一致的顺序便于比对产物大小。
C_SRCS_PRE  := $(sort $(wildcard mbedtls/library/*.c)) $(sort $(wildcard User/*.c))
C_SRCS_POST := $(sort $(wildcard Peripheral/src/*.c)) \
               Net/NetLib/eth_driver.c Debug/debug.c Core/core_riscv.c
S_SRCS      := Startup/startup_ch32v20x_D8W.S

S_OBJS   := $(S_SRCS:%.S=$(OBJDIR)/%.o)
OBJS     := $(C_SRCS_PRE:%.c=$(OBJDIR)/%.o) $(S_OBJS) \
            $(C_SRCS_POST:%.c=$(OBJDIR)/%.o)

.PHONY: all app bootloader image fw package size clean

all: image fw

bootloader:
	$(MAKE) -C Bootloader

app: $(APP_HEX) $(BUILD)/$(TARGET).bin size

# 首次烧写镜像：Bootloader（0x0）+ App（0x3000），不含 Meta。
image: app bootloader
	@APP_HEX=$(APP_HEX) BOOT_HEX=$(BOOT_HEX) ./merge-image.sh -o $(FULL_HEX)

# 升级固件：仅 App + 包头 + 签名，串口 IAP / 网络 OTA 共用。
package: fw
fw: app
	@key='$(FW_KEY)'; \
	if [ -z "$$key" ] || [ ! -f "$$key" ]; then \
	    echo "跳过 .dtufw：未找到签名私钥（FW_KEY='$$key'），用 make fw FW_KEY=<PEM> 指定"; \
	else \
	    APP_HEX=$(APP_HEX) FW_KEY="$$key" FW_REQUIRE_KEY_MATCH='$(FW_REQUIRE_KEY_MATCH)' \
	        ./pack-firmware.sh --version '$(FW_VERSION)' --key "$$key"; \
	fi

$(OBJDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS) Ld/Link.ld Makefile
	@mkdir -p $(@D)
	$(CC) $(OBJS) $(LDFLAGS) $(LIBS) -o $@

$(BUILD)/$(TARGET).hex: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O ihex $< $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

size: $(BUILD)/$(TARGET).elf
	@$(SIZE) $<
	@used=$$($(SIZE) $< | awk 'NR==2 {print $$1+$$2}'); \
	 echo "app flash: $$used / $(FLASH_MAX) bytes"; \
	 if [ "$$used" -gt "$(FLASH_MAX)" ]; then echo "ERROR: App 超过 148 KB 分区"; exit 1; fi

clean:
	rm -rf $(BUILD)
	$(MAKE) -C Bootloader clean

-include $(OBJS:.o=.d)
