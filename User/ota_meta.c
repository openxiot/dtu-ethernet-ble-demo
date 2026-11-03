/*********************************************************************
 * File Name          : ota_meta.c
 *
 * Description        : OTA 元数据双页存储实现。
 *******************************************************************************/
#include "ota_meta.h"
#include "ota_crc.h"
#include "ch32v20x_flash.h"
#include <string.h>

static uint32_t g_seq;          /* 当前最大有效 seq */
static int      g_page = -1;    /* 当前有效记录所在页；-1 表示都无效 */
static ota_meta_t g_cached;     /* 最近一次 load 的结果 */

static void meta_defaults(ota_meta_t *m)
{
    memset(m, 0, sizeof(*m));
    m->magic = OTA_META_MAGIC;
    m->state = OTA_META_EMPTY;
    m->record_crc32 = ota_crc32(m, 0x40);
}

static int meta_record_valid(const ota_meta_t *m)
{
    if (m->magic != OTA_META_MAGIC) return 0;
    if (m->record_crc32 != ota_crc32(m, 0x40)) return 0;
    if (m->state > OTA_META_RECOVERY) return 0;
    return 1;
}

void ota_meta_load(ota_meta_t *out)
{
    uint32_t best_seq = 0;
    int best_page = -1;
    uint32_t i;

    for (i = 0; i < OTA_META_PAGES; i++) {
        const ota_meta_t *m =
            (const ota_meta_t *)OTA_FLASH_PTR(OTA_META_BASE + i * OTA_META_PAGE_SIZE);

        if (meta_record_valid(m)) {
            if (best_page < 0 || m->seq > best_seq) {
                best_seq = m->seq;
                best_page = (int)i;
            }
        }
    }

    if (best_page < 0) {
        meta_defaults(&g_cached);
        g_seq = 0;
        g_page = -1;
    } else {
        memcpy(&g_cached,
               (const void *)OTA_FLASH_PTR(OTA_META_BASE + (uint32_t)best_page * OTA_META_PAGE_SIZE),
               sizeof(g_cached));
        g_seq = best_seq;
        g_page = best_page;
    }

    if (out) *out = g_cached;
}

int ota_meta_save(ota_meta_t *m)
{
    static uint32_t rec[OTA_META_RECORD_SIZE / 4] __attribute__((aligned(4)));
    uint32_t next_page;
    uint32_t addr;
    FLASH_Status st;

    if (!m) return -1;

    m->magic = OTA_META_MAGIC;
    m->seq = g_seq + 1;
    m->record_crc32 = ota_crc32(m, 0x40);

    next_page = (g_page < 0) ? 0u : (uint32_t)((g_page + 1) % (int)OTA_META_PAGES);
    addr = OTA_FLASH_ADDR(OTA_META_BASE + next_page * OTA_META_PAGE_SIZE);

    memcpy(rec, m, OTA_META_RECORD_SIZE);

    st = FLASH_ROM_ERASE(addr, OTA_META_PAGE_SIZE);
    if (st != FLASH_COMPLETE) return -1;
    st = FLASH_ROM_WRITE(addr, rec, OTA_META_RECORD_SIZE);
    if (st != FLASH_COMPLETE) return -1;

    if (memcmp((const void *)OTA_FLASH_PTR(OTA_META_BASE + next_page * OTA_META_PAGE_SIZE),
               rec, OTA_META_RECORD_SIZE) != 0)
        return -1;

    g_seq = m->seq;
    g_page = (int)next_page;
    g_cached = *m;
    return 0;
}

uint8_t ota_meta_state(void)
{
    return g_cached.state;
}

uint32_t ota_meta_version(void)
{
    return g_cached.image_version;
}
