#ifndef BOOT_METADATA_H
#define BOOT_METADATA_H

#include <stdint.h>
#include <stddef.h>     /* offsetof */

#define METADATA_0_SECTOR       3U                 //代表是扇区3
#define METADATA_0_BASE         0x0800C000UL
#define METADATA_0_END          0x08010000UL

#define METADATA_1_SECTOR       4U                //代表是扇区4
#define METADATA_1_BASE         0x08010000UL
#define METADATA_1_END          0x08020000UL

#define METADATA_MAGIC          0x4D455441UL   /* "META"，v1 沿用不变 */
#define METADATA_FORMAT_VERSION 3U

/* 槽状态：从擦除态出发逐位清 0（第 19 课位技巧）；白名单之外的值一律按最保守语义处理 */
#define SLOT_STATE_EMPTY     0xFFFFFFFFUL
#define SLOT_STATE_UPDATING  0x7FFFFFFFUL
#define SLOT_STATE_VALID     0x3FFFFFFFUL

#define SLOT_A  0U              /* 槽号 = 数组下标 */
#define SLOT_B  1U

/* 一次写定、终身不变的槽身份，CRC 覆盖 */
typedef struct {
    uint32_t size;
    uint32_t crc32;
    uint32_t version;
} slot_identity_t;

typedef struct {
    /* ---- CRC 覆盖区：连续前缀 36 字节，只在 begin_update 整擦重写时一次写入 ---- */
    uint32_t        magic;          /* 偏移 0  */
    uint32_t        format_version; /* 偏移 4  */
    uint32_t        generation;     /* 偏移 8  */
    slot_identity_t slot[2];        /* 偏移 12–35，[0]=A，[1]=B */
    /* ---- 原地编程区：逐位清 0，不进 CRC，靠合法值白名单自校验 ---- */
    uint32_t        slot_state[2];  /* 偏移 36/40，UPDATING→VALID 原地改 */
    uint32_t        active_slot;    /* 偏移 44，位梯子：0xFFFFFFFF=A，0x7FFFFFFF=B，0x3FFFFFFF=A… */
    uint32_t        header_crc32;   /* 偏移 48，覆盖前 36 字节 */
} app_metadata_t;                   /* 共 52 字节，全 uint32_t 无对齐空洞 */

/* active_slot 位梯子的最简编码：擦除态=A，切到 B 清最高位；再回 A 靠下次整擦重置 */
#define METADATA_ACTIVE_A    0xFFFFFFFFUL
#define METADATA_ACTIVE_B    0x7FFFFFFFUL

void boot_metadata_default(app_metadata_t *metadata);

/* 编译期守卫：将来手滑加字段忘了同步 CRC 范围，直接编译报错 */
typedef char metadata_size_check[(sizeof(app_metadata_t) == 52U) ? 1 : -1];

int boot_metadata_read(app_metadata_t *metadata);
int boot_metadata_is_valid(const app_metadata_t *metadata);
int boot_metadata_begin_update(const app_metadata_t *old, uint32_t slot,
                               uint32_t size, uint32_t crc32, uint32_t version);
int boot_metadata_mark_valid(uint32_t slot);
int boot_metadata_switch_active(void);
uint32_t boot_metadata_active_index(uint32_t active_slot_field);

#endif