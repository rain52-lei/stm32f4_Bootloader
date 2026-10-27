#include "boot_jump.h"
#include "stm32f4xx_hal.h"
#include "usart.h"

#define APP_SRAM_END  0x20020000UL

/* No C code may use the old stack after changing MSP. r0/r1 are the AAPCS
   arguments; branch directly instead of making another C function call. */
__attribute__((naked, noreturn)) static void boot_start_app(uint32_t msp,
                                                          uint32_t reset)
{
    __asm volatile (
        "movs r2, #0\n"
        "msr control, r2\n"
        "isb\n"
        "msr msp, r0\n"
        "dsb\n"
        "isb\n"
        "cpsie i\n"
        "bx r1\n"
    );
}

int boot_app_is_valid(uint32_t app_base, uint32_t app_end)
{
    uint32_t app_msp;
    uint32_t app_reset;

    if (app_base == 0U || app_base >= app_end || (app_end - app_base) < 8U)
    {
        return 0;
    }

    app_msp = *(volatile const uint32_t *)app_base;
    app_reset = *(volatile const uint32_t *)(app_base + 4U);

    if (app_msp <= SRAM1_BASE || app_msp > APP_SRAM_END || (app_msp & 0x7U) != 0U)
    {
        return 0;
    }
    if ((app_reset & 1UL) == 0UL)
    {
        return 0;
    }
    if ((app_reset & ~1UL) < app_base || (app_reset & ~1UL) >= app_end)
    {
        return 0;
    }
    return 1;
}

void boot_jump_to_app(uint32_t app_base, uint32_t app_end)
{
    uint32_t app_msp;
    uint32_t app_reset;

    if (!boot_app_is_valid(app_base, app_end))
    {
        return;
    }

    app_msp = *(volatile const uint32_t *)app_base;
    app_reset = *(volatile const uint32_t *)(app_base + 4U);

    HAL_UART_DeInit(&huart1);
    HAL_RCC_DeInit();
    HAL_DeInit();

    __disable_irq();

    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL = 0U;
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;

    for (uint32_t i = 0U; i < 8U; i++)
    {
        NVIC->ICER[i] = 0xFFFFFFFFUL;
        NVIC->ICPR[i] = 0xFFFFFFFFUL;
    }

    SCB->VTOR = app_base;
    __DSB();
    __ISB();

    /* __disable_irq 置位的 PRIMASK 必须在这里清掉：App 的 startup 不会重开
       全局中断，少这行 App 里第一个 HAL_Delay 就等不到 SysTick、永久卡死。
       此处 NVIC 使能与挂起均已清空，重开不会冒出意外中断 */
    __set_BASEPRI(0U);
    __set_FAULTMASK(0U);
    boot_start_app(app_msp, app_reset);

    while (1)
    {
    }
}

/* App writes a one-shot request to BKP0R before software reset. Backup data
   can survive VDD removal when VBAT remains powered. Consume the request
   immediately; do not assume that every power cycle clears the backup domain.
   读到标志立即清掉，
   写备份域要先解 DBP 写保护（PWR 时钟 SystemClock_Config 已开）。 */
#define GOTOBOOT_MAGIC  0x4F544144UL    /* ASCII "OTAD"，与 App 侧约定一致 */

int boot_goto_boot_requested(void)
{
    if (RTC->BKP0R != GOTOBOOT_MAGIC)
    {
        return 0;
    }

    PWR->CR |= PWR_CR_DBP;
    RTC->BKP0R = 0U;
    return 1;
}

/* 在 timeout_ms 内逐字节匹配 "UPDATE"，命中返回 1，超时返回 0 */
int boot_wait_update_command(uint32_t timeout_ms)
{
    static const uint8_t expected[] = "UPDATE";
    uint8_t byte;
    uint32_t matched = 0U;
    uint32_t start_tick = HAL_GetTick();

    while ((HAL_GetTick() - start_tick) < timeout_ms)
    {
        if (HAL_UART_Receive(&huart1, &byte, 1U, 10U) != HAL_OK)
        {
            continue;
        }

        if (byte == expected[matched])
        {
            matched++;
            if (matched == (sizeof(expected) - 1U))
            {
                return 1;
            }
        }
        else
        {
            matched = (byte == expected[0]) ? 1U : 0U;
        }
    }

    return 0;
}
