/*********************************************************************
 * File Name          : boot_main.c
 *
 * Description        : Bootloader 入口与启动决策（设计文档 6.2）。
 *
 * 流程：
 *   STAGED/COPYING → 校验 Staging → 搬运到 App → 校验 → APP_VALID → 跳转
 *   BOOT_UART      → 等 HELLO 30 s，超时按 APP_VALID 处理
 *   APP_VALID      → attempts 达到上限则 RECOVERY；否则 500 ms 窗口，
 *                    收到 HELLO 进入串口升级，否则 attempts++ 后跳 App
 *   EMPTY/RECOVERY → 无限等待 HELLO
 *******************************************************************************/
#include "boot_main.h"
#include "boot_iap.h"
#include "boot_uart.h"
#include "boot_flash.h"
#include "ota_meta.h"
#include "ota_pkg.h"
#include "ota_config.h"
#include "ota_layout.h"
#include "ch32v20x.h"

/* 判断 App 区是否像是有效镜像：只看首字，不能当作函数指针。 */
static int app_looks_valid(void)
{
    uint32_t w = *(volatile const uint32_t *)OTA_FLASH_PTR(OTA_APP_BASE);
    return (w != 0xFFFFFFFFu) && (w != 0u);
}

static void jump_to_app(void)
{
    uint32_t i;

    __disable_irq();
    SysTick->CTLR = 0;
    SysTick->SR = 0;
    for (i = 0; i < 8; i++) {
        NVIC->IRER[i] = 0xFFFFFFFFu;    /* 关闭全部中断 */
        NVIC->IPRR[i] = 0xFFFFFFFFu;    /* 清 pending */
    }
    ((void (*)(void))OTA_APP_BASE)();   /* App 的 handle_reset 会重设 sp/gp/bss */
    for (;;)
        ;
}

void boot_jump_to_app(void)
{
    if (!app_looks_valid()) {
        ota_meta_t m;
        ota_meta_load(&m);
        m.state = OTA_META_RECOVERY;
        ota_meta_save(&m);
        NVIC_SystemReset();
    }
    jump_to_app();
}

/* 校验 Staging 并把它应用到 App。
 * 成功跳转不返回；Staging 无效时改为 APP_VALID 并返回 0；
 * 搬运/校验失败写入 RECOVERY 并返回 -1。 */
static int apply_staged(ota_meta_t *m)
{
    ota_pkg_header_t ph;
    uint32_t crc_out = 0;

    if (!(m->flags & OTA_META_FLAG_VERIFIED) ||
        ota_pkg_verify_staging(&ph) != OTA_ST_OK ||
        ph.image_size != m->image_size ||
        ph.image_crc32 != m->image_crc32 ||
        ph.image_version != m->image_version) {
        /* Staging 无效：清除 STAGED，按 APP_VALID 处理（文档 6.2） */
        m->state = OTA_META_APP_VALID;
        m->attempts = 0;
        ota_meta_save(m);
        return 0;
    }

    /* 先标记 COPYING；写失败也继续搬运，掉电后 STAGED 仍可重试 */
    m->state = OTA_META_COPYING;
    ota_meta_save(m);

    if (boot_copy_staging_to_app(m->image_size, &crc_out) != 0 ||
        crc_out != m->image_crc32 ||
        boot_flash_verify(OTA_APP_BASE, m->image_size, m->image_crc32) != 0) {
        m->state = OTA_META_RECOVERY;
        ota_meta_save(m);
        return -1;
    }

    m->state = OTA_META_APP_VALID;
    m->attempts = 0;
    m->flags |= OTA_META_FLAG_REPORT_PENDING;
    ota_meta_save(m);
    jump_to_app();
    return 0;
}

static void enter_recovery_wait(void)
{
    /* RECOVERY：无限等待串口升级；会话结束或失败都复位重来。 */
    for (;;) {
        int r = boot_iap_run(OTA_IAP_WAIT_FOREVER);
        if (r == 1 || r == -2)
            NVIC_SystemReset();
        /* ABORT：主机放弃，继续等下一次 HELLO */
    }
}

int main(void)
{
    ota_meta_t m;

    ota_meta_load(&m);
    boot_uart_init(OTA_UART_BAUD);

    for (;;) {
        switch (m.state) {
        case OTA_META_STAGED:
        case OTA_META_COPYING:
            if (apply_staged(&m) != 0)
                enter_recovery_wait();
            ota_meta_load(&m);
            break;

        case OTA_META_BOOT_UART: {
            int r = boot_iap_run(OTA_BOOT_UART_WAIT_MS);
            if (r == -1) {
                /* 30 s 未收到 HELLO：按 APP_VALID 处理 */
                m.state = OTA_META_APP_VALID;
                ota_meta_save(&m);
            } else {
                NVIC_SystemReset();
            }
            break;
        }

        case OTA_META_APP_VALID: {
            int r;

            if (m.attempts >= OTA_MAX_ATTEMPTS || !app_looks_valid()) {
                m.state = OTA_META_RECOVERY;
                ota_meta_save(&m);
                break;
            }

            r = boot_iap_run(OTA_BOOT_WINDOW_MS);
            if (r == -1) {
                /* 无人升级：attempts++ 后正常启动 */
                m.attempts++;
                ota_meta_save(&m);
                if (app_looks_valid())
                    jump_to_app();
            }
            NVIC_SystemReset();
            break;
        }

        case OTA_META_EMPTY:
        case OTA_META_RECOVERY:
        default:
            /* 无有效记录 / App 不可用：无限等待串口升级 */
            enter_recovery_wait();
            break;
        }
    }
}
