#include "boot_metadata.h"
#include "boot_update.h"      /* boot_crc32_buffer / APP_MAX_SIZE */
#include "stm32f4xx_hal.h"
#include <stddef.h>

/* ---------- 双副本调度：read() 裁决后生效，写点函数据此选目标 ---------- */

static uint32_t s_newest_copy;    /* 现在信哪份：mark_valid / switch_active 写它 */
static uint32_t s_stale_copy;     /* 下次动哪份：begin_update 擦它写它 */

/* 契约：任何写操作前必须先 read()——main 的启动流程天然满足（先读账本再决定动作）。
   两份都无效时 static 零初始化保证 newest=0 / stale=0：首刷写副本 0，行为确定。 */

static uint32_t metadata_copy_base(uint32_t copy)
{
    if (copy == 0U)
    {
        return METADATA_0_BASE;
    }
    return METADATA_1_BASE;
}

static uint32_t metadata_copy_sector(uint32_t copy)
{
    if (copy == 0U)
    {
        return METADATA_0_SECTOR;
    }
    return METADATA_1_SECTOR;
}

/* ---------- 位梯子：active_slot 编解码共用的底层 ---------- */

#define METADATA_ILLEGAL_STEP  0xFFFFFFFFUL

/* 返回梯级 = 最高位起连续 0 的个数（0..31）；值不在梯子上（含全 0）返回 ILLEGAL */
static uint32_t ladder_step(uint32_t field)
{
    uint32_t zeros;

    for (zeros = 0U; zeros < 32U; zeros++)
    {
        if ((field >> (31U - zeros)) & 1U)
        {
            break;
        }
    }
    /* 合法梯级要求其余位全 1（0xFFFFFFFF>>zeros 能还原出原值）。
       zeros==32 即 field==0，且此时不能再移位——uint32_t 移 32 位是未定义行为，必须先挡掉 */
    if (zeros >= 32U || field != (0xFFFFFFFFUL >> zeros))
    {
        return METADATA_ILLEGAL_STEP;
    }
    return zeros;
}

uint32_t boot_metadata_active_index(uint32_t active_slot_field)
{
    uint32_t step = ladder_step(active_slot_field);

    /* 非法值（撕裂编程的残留）保守按 A 处理，绝不因此作废整份记录 */
    if (step == METADATA_ILLEGAL_STEP)
    {
        return SLOT_A;
    }
    return (step % 2U == 1U) ? SLOT_B : SLOT_A;    /* 奇数级=B，偶数级=A */
}

/* ---------- 读与校验 ---------- */

void boot_metadata_default(app_metadata_t *metadata)
{
    uint32_t i;

    if (metadata == NULL)
    {
        return;
    }
    metadata->magic          = METADATA_MAGIC;
    metadata->format_version = METADATA_FORMAT_VERSION;
    metadata->generation     = 0U;    /* RAM 默认记录是第 0 代 */
    for (i = SLOT_A; i <= SLOT_B; i++)
    {
        metadata->slot[i].size    = 0xFFFFFFFFUL;   /* 保持擦除态 */
        metadata->slot[i].crc32   = 0xFFFFFFFFUL;
        metadata->slot[i].version = 0xFFFFFFFFUL;
        metadata->slot_state[i]   = SLOT_STATE_EMPTY;
    }
    metadata->active_slot  = METADATA_ACTIVE_A;
    metadata->header_crc32 = 0xFFFFFFFFUL;
}

int boot_metadata_read(app_metadata_t *metadata)
{
    app_metadata_t copies[2];    /* 两份各自读出，互不影响 */
    uint8_t       valid[2];      /* 各自的验货结果：1=完好 0=撕裂/无效 */
    uint32_t       i;

    if (metadata == NULL)
    {
        return 0;
    }

    for (i = 0U; i < 2U; i++)
    {
        copies[i] = *(const app_metadata_t *)metadata_copy_base(i);
        valid[i]  = (uint8_t)boot_metadata_is_valid(&copies[i]);
    }

    /* 两份都无效（全新板子 / 双双损坏）→ 没有档案，调用方构造默认记录 */
    if (!valid[0] && !valid[1])
    {
        return 0;
    }

    /* 裁决：先验货、后比号。generation 也在 CRC 覆盖区内——验不过的记录，
       它的流水号无从采信，所以 validity 永远在 generation 之前判。
       回绕：uint32 每次升级 +1，绕回需 42.9 亿次升级，普通 > 比较即可 */
    if (valid[0] != valid[1])
    {
        s_newest_copy = (valid[0] != 0U) ? 0U : 1U;   /* 恰一份完好：它是唯一真值 */
    }
    else if (copies[1].generation > copies[0].generation)
    {
        s_newest_copy = 1U;    /* 两份都完好：流水号大的是新一代 */
    }
    else
    {
        s_newest_copy = 0U;    /* 副本 0 更新；相等（防御，构造上不该发生）取 0 */
    }
    s_stale_copy = 1U - s_newest_copy;

    *metadata = copies[s_newest_copy];
    return 1;
}

static int slot_identity_sane(const slot_identity_t *id)
{
    return id->size != 0U &&
           id->size <= APP_MAX_SIZE &&
           id->version != 0U;
}

int boot_metadata_is_valid(const app_metadata_t *metadata)
{
    uint32_t i;

    if (metadata == NULL ||
        metadata->magic != METADATA_MAGIC ||
        metadata->format_version != METADATA_FORMAT_VERSION)
    {
        return 0;
    }

    /* CRC 只盖"一次写定"的前 36 字节：offsetof(app_metadata_t, slot_state) 恰好 = 36 */
    if (boot_crc32_buffer((const uint8_t *)metadata,
                          offsetof(app_metadata_t, slot_state))
        != metadata->header_crc32)
    {
        return 0;
    }

    /* 非 EMPTY 槽的身份必须健全。CRC 已保证读到的是写入值，这里防的是写端 bug */
    for (i = SLOT_A; i <= SLOT_B; i++)
    {
        if (metadata->slot_state[i] != SLOT_STATE_EMPTY &&
            !slot_identity_sane(&metadata->slot[i]))
        {
            return 0;
        }
    }
    return 1;
    /* 注意：这里故意不校验 state/active 的白名单——所有消费方只问
       "是不是 VALID"、active 走 active_index() 的保守兜底，
       非法值天然落不到 VALID 分支，整份记录不会因一个撕裂的字而作废 */
}

/* ---------- 写：整擦重写（唯一一处）与两次原地编程 ---------- */

int boot_metadata_begin_update(const app_metadata_t *old, uint32_t slot,
                               uint32_t size, uint32_t crc32, uint32_t version)
{
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t sector_error;
    app_metadata_t next;
    const uint32_t *words;
    uint32_t i;
    uint32_t word_count;
    uint32_t stale_base;

    if (old == NULL || slot > SLOT_B ||
        size == 0U || size > APP_MAX_SIZE || version == 0U)
    {
        return 0;
    }

    /* 以旧记录为底稿：另一槽的身份与状态、active 全部原样保留。
       active 不在这里重规整——决策权在 main，old->active_slot 由调用方给定 */
    next = *old;
    next.magic          = METADATA_MAGIC;
    next.format_version = METADATA_FORMAT_VERSION;
    next.slot[slot].size    = size;
    next.slot[slot].crc32   = crc32;
    next.slot[slot].version = version;
    next.slot_state[slot]   = SLOT_STATE_UPDATING;

    /* 流水号 +1：这份新记录是第 old->generation + 1 代。
       必须在算 CRC 之前——generation 在 CRC 覆盖区内，先加后算，账才对得上 */
    next.generation = old->generation + 1U;

    /* 必须在擦除前算好 CRC：擦完再算就来不及了（next 的原料抄自 Flash 里的 old），
       且 CRC 不含 state/active，之后两次原地编程都不影响它 */
    next.header_crc32 = boot_crc32_buffer((const uint8_t *)&next,
                                          offsetof(app_metadata_t, slot_state));

    /* 写旧不写新：整擦整写永远落在 stale 副本，newest 一个字节不动 */
    stale_base = metadata_copy_base(s_stale_copy);

    erase.TypeErase    = FLASH_TYPEERASE_SECTORS;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    erase.Sector       = metadata_copy_sector(s_stale_copy);
    erase.NbSectors    = 1U;

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        return 0;
    }
    if (HAL_FLASHEx_Erase(&erase, &sector_error) != HAL_OK)
    {
        HAL_FLASH_Lock();
        return 0;
    }

    /* 52 字节 = 13 个字，循环编程到 stale 副本。v1 逐字段写了 5 遍 HAL_FLASH_Program，
       字段翻倍后那种写法就不可维护了 */
    words      = (const uint32_t *)&next;
    word_count = (uint32_t)(sizeof(app_metadata_t) / sizeof(uint32_t));
    for (i = 0U; i < word_count; i++)
    {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                              stale_base + i * 4U, words[i]) != HAL_OK)
        {
            HAL_FLASH_Lock();
            return 0;
        }
    }

    HAL_FLASH_Lock();

    /* 新记录已完整落盘：即刻成为 newest——本会话内后续的 mark_valid /
       switch_active 都要写它；原来的 newest 降为 stale，等下一轮被重写。
       若不交换：mark_valid 会把 VALID 盖在旧记录上（旧的 dl 槽状态多半已是
       VALID，幂等返回"成功"），新记录永远停在 UPDATING——升级永远不生效 */
    s_newest_copy = s_stale_copy;
    s_stale_copy  = 1U - s_newest_copy;

    return 1;
}

int boot_metadata_mark_valid(uint32_t slot)
{
    volatile uint32_t *state_word;

    if (slot > SLOT_B)
    {
        return 0;
    }
    state_word = (volatile uint32_t *)(metadata_copy_base(s_newest_copy) +
                  offsetof(app_metadata_t, slot_state[slot]));

    /* Flash 编程只能把 1 清成 0，所以先看当前值再决定动不动手 */
    if (*state_word == SLOT_STATE_VALID)
    {
        return 1;                        /* 幂等：已是 VALID，重复调用无害 */
    }
    if (*state_word != SLOT_STATE_UPDATING)
    {
        return 0;                        /* 只允许 UPDATING→VALID 这一条路 */
    }

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        return 0;
    }
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                          (uint32_t)state_word,
                          SLOT_STATE_VALID) != HAL_OK)
    {
        HAL_FLASH_Lock();
        return 0;
    }
    HAL_FLASH_Lock();
    return 1;
}

int boot_metadata_switch_active(void)
{
    volatile uint32_t *field;
    uint32_t current;
    uint32_t step;
    uint32_t next;

    field = (volatile uint32_t *)(metadata_copy_base(s_newest_copy) +
              offsetof(app_metadata_t, active_slot));
    current = *field;
    step    = ladder_step(current);

    if (step == METADATA_ILLEGAL_STEP || step >= 31U)
    {
        return 0;                        /* 不在梯子上/梯子用尽：拒绝，走上层失败路径 */
    }

    /* 再清一位：0xFFFFFFFF→0x7FFFFFFF→0x3FFFFFFF，仍是"只能清位"的合法编程 */
    next = current & ~(0x80000000UL >> step);

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        return 0;
    }
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                          (uint32_t)field, next) != HAL_OK)
    {
        HAL_FLASH_Lock();
        return 0;
    }
    HAL_FLASH_Lock();
    return 1;
}
