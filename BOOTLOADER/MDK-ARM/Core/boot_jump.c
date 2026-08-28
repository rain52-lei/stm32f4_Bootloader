#include "boot_jump.h"
#include "stm32f4xx_hal.h"
#include "usart.h"
#include <string.h>

#define APP_BASE      0x08020000UL
#define APP_END       0x08100000UL
#define APP_SRAM_END  0x20020000UL

typedef void (*app_entry_t)(void);

static uint32_t app_msp;
static uint32_t app_reset;

int boot_app_is_valid(void)
{
	app_msp = *(volatile const uint32_t *)APP_BASE;
	app_reset = *(volatile const uint32_t *)(APP_BASE + 4U);
	
	if(app_msp < SRAM1_BASE || app_msp > APP_SRAM_END)
	{
		return 0;
	}
	
	if((app_reset & 1UL) == 0UL)
	{
		return 0;
	}
	
	if((app_reset & ~1UL) < APP_BASE || (app_reset & ~1UL) >= APP_END)
	{
		return 0;
	}
	return 1;
}

void boot_jump_to_app(void)
{
	if(!boot_app_is_valid())
	{
		return;
	}
	
	/*
	 * 让 App 看见与“真正复位后”一致的默认时钟环境：HSI 作为
	 * 系统时钟、PLL/HSE 关闭。否则 App 的 SystemClock_Config()
	 * 会在 PLL 已被 Bootloader 用作系统时钟时重配 PLL，并进入
	 * Error_Handler()。此函数内部需要 SysTick 计时，因此必须在
	 * 停止 SysTick 之前调用。
	 */
	HAL_RCC_DeInit();

	__disable_irq();
	
	SysTick->CTRL = 0U;
	SysTick->LOAD = 0U;
	SysTick->VAL  = 0U;
	
	for(uint32_t i = 0;i < 3U; i++)
	{
		NVIC->ICER[i] = 0xFFFFFFFFUL;
		NVIC->ICPR[i] = 0xFFFFFFFFUL; 		
	}
	
	SCB->VTOR = APP_BASE;
	__DSB();
	__ISB();
	
	__set_CONTROL(0U);
	__ISB();
	
	__set_MSP(app_msp);
	
	__enable_irq();
	
	((app_entry_t)app_reset)();
	
	while(1)
	{
	}
}

int boot_wait_update_command(void)
{
	static const uint8_t expected[] = "UPDATE";
	uint8_t byte;
	uint32_t matched = 0U;
	uint32_t start_tick = HAL_GetTick();

	/* 每次最多等 10 ms 收 1 个字节；总窗口为 2 秒。
	   这样串口打开时的杂字节或半条命令不会阻塞启动流程。 */
	while ((HAL_GetTick() - start_tick) < 2000U) {
		if (HAL_UART_Receive(&huart1, &byte, 1U, 10U) != HAL_OK) {
			continue;
		}

		if (byte == expected[matched]) {
			matched++;
			if (matched == (sizeof(expected) - 1U)) {
				return 1;
			}
		} else {
			/* 当前字节若正好是 U，可能是一条新命令的开始。 */
			matched = (byte == expected[0]) ? 1U : 0U;
		}
	}

	return 0;
}

