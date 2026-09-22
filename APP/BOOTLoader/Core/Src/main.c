/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* 与 bootloader 约定的"进升级模式"留言值（ASCII "OTAD"），写进 RTC->BKP0R */
#define GOTOBOOT_MAGIC    0x4F544144UL
/* 槽 B 的链接基址，bootloader 跳转前会把 VTOR 设成它 */
#define SLOT_B_CODE_BASE  0x080A0000UL
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static const uint8_t cmd_goto_boot[] = "GOTOBOOT";
static uint32_t s_match_pos = 0U;     /* GOTOBOOT 匹配进度 */
static uint32_t s_last_toggle = 0U;   /* LED 上次翻转的时刻 */

/* 裸寄存器 USART1（115200 8N1，与 bootloader 同配置）：App 的 HAL 配置里
   没开 UART 模块，直接配寄存器反而最省 */
static void app_usart1_init(void)
{
    GPIO_InitTypeDef gpio;

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();

    gpio.Pin       = GPIO_PIN_9 | GPIO_PIN_10;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* APB2 = 84MHz：84M / (16 * 45.5625) = 115200，BRR = 45<<4 | 9 */
    USART1->BRR = 0x2D9U;
    USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

static void app_uart_write(const uint8_t *data, uint32_t length)
{
    uint32_t i;

    for (i = 0U; i < length; i++)
    {
        while ((USART1->SR & USART_SR_TXE) == 0U)
        {
        }
        USART1->DR = data[i];
    }
    while ((USART1->SR & USART_SR_TC) == 0U)
    {
    }
}

/* 上电只打一次，上位机拿它当"App 已在运行"的同步点；
   槽号读自 bootloader 跳转前设置的 VTOR */
static void app_print_ready(void)
{
    static const uint8_t ready_a[] = "APP READY (SLOT A)\r\n";
    static const uint8_t ready_b[] = "APP READY (SLOT B)\r\n";

    if (SCB->VTOR == SLOT_B_CODE_BASE)
    {
        app_uart_write(ready_b, sizeof(ready_b) - 1U);
    }
    else
    {
        app_uart_write(ready_a, sizeof(ready_a) - 1U);
    }
}

/* 主循环高频轮询：RXNE 置位到读 DR 只隔几微秒，8 字节连发不会丢。
   以后要加别的串口命令，在这个状态机里扩展 */
static int app_poll_goto_boot(void)
{
    uint8_t byte;

    if ((USART1->SR & USART_SR_RXNE) == 0U)
    {
        return 0;
    }

    byte = (uint8_t)USART1->DR;
    if (byte == cmd_goto_boot[s_match_pos])
    {
        s_match_pos++;
        if (s_match_pos == (sizeof(cmd_goto_boot) - 1U))
        {
            s_match_pos = 0U;
            return 1;
        }
    }
    else
    {
        s_match_pos = (byte == cmd_goto_boot[0]) ? 1U : 0U;
    }
    return 0;
}

/* 收到完整 GOTOBOOT：写留言 → 通知上位机 → 复位进 bootloader */
static void app_reboot_into_boot(void)
{
    static const uint8_t msg[] = "GOTOBOOT: REBOOT INTO BOOTLOADER\r\n";
    volatile uint32_t drain;

    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();      /* 备份域默认写保护，先解锁 */
    RTC->BKP0R = GOTOBOOT_MAGIC;

    app_uart_write(msg, sizeof(msg) - 1U);
    for (drain = 0U; drain < 100000U; drain++)
    {
    }
    NVIC_SystemReset();
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  /* USER CODE BEGIN 2 */
  app_usart1_init();
  app_print_ready();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    uint32_t now = HAL_GetTick();

    if (app_poll_goto_boot())
    {
      app_reboot_into_boot();
    }

    /* LED 翻转走 tick 比较，主循环不再有 HAL_Delay 死等：
       串口轮询间隙只有几微秒，命令不会丢字节 */
    if ((now - s_last_toggle) >= 500U)
    {
      s_last_toggle = now;
      HAL_GPIO_TogglePin(GPIOF, GPIO_PIN_9 | GPIO_PIN_10);
    }
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /*
   * 临时诊断：若 App 的时钟配置失败，也要能从 LED 看出来。
   * PF9/PF10 是探索者板 LED0/LED1，低电平点亮。
   */
  __disable_irq();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  GPIOF->MODER &= ~((3UL << (9U * 2U)) | (3UL << (10U * 2U)));
  GPIOF->MODER |=  ((1UL << (9U * 2U)) | (1UL << (10U * 2U)));
  while (1)
  {
		GPIOF->ODR |= GPIO_PIN_10;
    GPIOF->ODR ^= GPIO_PIN_9;
    for (volatile uint32_t delay = 0U; delay < 800000U; delay++)
    {
    }
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
