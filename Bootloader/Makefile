################################################################################
# Bootloader 独立构建
#
# 与 App 共用 User/ota_*.c 与 Ld/Link_boot.ld；不链接 WCHNET / mbedTLS。
# 用法：
#   make            # 构建 build/bootloader.{elf,hex,bin}
#   make size       # 只检查 12 KB 上限
#   make clean
################################################################################

CROSS    ?= riscv-none-embed-
CC       := $(CROSS)gcc
OBJCOPY  := $(CROSS)objcopy
SIZE     := $(CROSS)size

TARGET   := bootloader
BUILD    := build
LDSCRIPT := Link_boot.ld
MAP      := $(BUILD)/$(TARGET).map

ARCH     := -march=rv32imacxw -mabi=ilp32 -msmall-data-limit=8 -msave-restore
CFLAGS   := $(ARCH) -Os -fsigned-char -ffunction-sections -fdata-sections \
            -fno-common -g -std=gnu99 -Wall \
            -I. -I../User -I../Core -I../Peripheral/inc -I../Debug
ASFLAGS  := $(ARCH) -x assembler-with-cpp -DBOOTLOADER
LDFLAGS  := $(ARCH) -T $(LDSCRIPT) -nostartfiles -Xlinker --gc-sections \
            -Wl,-Map=$(MAP) --specs=nano.specs --specs=nosys.specs

C_SRCS   := boot_main.c boot_iap.c boot_uart.c boot_flash.c \
            ../User/ota_crc.c ../User/ota_sha256.c \
            ../User/ota_meta.c ../User/ota_pkg.c \
            ../User/system_ch32v20x.c ../Core/core_riscv.c \
            ../Peripheral/src/ch32v20x_flash.c \
            ../Peripheral/src/ch32v20x_gpio.c \
            ../Peripheral/src/ch32v20x_rcc.c \
            ../Peripheral/src/ch32v20x_usart.c
S_SRCS   := ../Startup/startup_ch32v20x_D8W.S

C_OBJS   := $(addprefix $(BUILD)/,$(notdir $(C_SRCS:.c=.o)))
S_OBJS   := $(addprefix $(BUILD)/,$(notdir $(S_SRCS:.S=.o)))
OBJS     := $(C_OBJS) $(S_OBJS)

vpath %.c . .. ../User ../Core ../Peripheral/src
vpath %.S .. ../Startup

.PHONY: all clean size

all: $(BUILD)/$(TARGET).hex $(BUILD)/$(TARGET).bin size

$(BUILD):
	@mkdir -p $@

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS) $(LDSCRIPT) | $(BUILD)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

$(BUILD)/$(TARGET).hex: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O ihex $< $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

size: $(BUILD)/$(TARGET).elf
	@$(SIZE) $<
	@used=$$($(SIZE) $< | awk 'NR==2 {print $$1+$$2}'); \
	 echo "bootloader flash: $$used / 12288 bytes"; \
	 if [ "$$used" -gt 12288 ]; then echo "ERROR: Bootloader 超过 12 KB"; exit 1; fi

clean:
	rm -rf $(BUILD)
