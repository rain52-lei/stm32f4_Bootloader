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
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "boot_jump.h"
#include "boot_update.h"
#include "boot_flash.h"
#include "boot_metadata.h"
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define BOOT_LED_SELF_TEST  0U
#define BOOT_EARLY_LED_TEST 0U
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

#if BOOT_EARLY_LED_TEST
  /*
   * 最早期硬件自检：此处尚未配置 HSE、PLL、串口和 App 跳转。
   * 只用复位后的内部 HSI 时钟直接点亮 PF9/PF10。
   */
  RCC->AHB1ENR |= RCC_AHB1ENR_GPIOFEN;
  GPIOF->MODER &= ~((3UL << (9U * 2U)) | (3UL << (10U * 2U)));
  GPIOF->MODER |=  ((1UL << (9U * 2U)) | (1UL << (10U * 2U)));
  while (1)
  {
    GPIOF->ODR ^= GPIO_PIN_9 | GPIO_PIN_10;
    for (volatile uint32_t delay = 0U; delay < 500000U; delay++)
    {
    }
  }
#endif

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
#if BOOT_LED_SELF_TEST
  /*
   * 纯 Bootloader 自检：不初始化串口、不接收命令、也不跳 App。
   * 若 USB 串口线接着时这里仍能闪灯，说明供电、复位、时钟和
   * Bootloader 本身正常，后续再逐项恢复串口与跳转。
   */
  while (1)
  {
    HAL_GPIO_TogglePin(GPIOF, GPIO_PIN_9 | GPIO_PIN_10);
    HAL_Delay(300U);
  }
#endif
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */
  static const uint8_t wait[] = "WAIT UPDATE (2s)\r\n";
  static const uint8_t jump[] = "TIMEOUT: JUMPING TO APP\r\n";
  static const uint8_t invalid[] = "APP INVALID: STAY IN BOOTLOADER\r\n";
	static const uint8_t header_bad[] = "HEADER BAD\r\n";
	static const uint8_t header_timeout[] = "HEADER TIMEOUT\r\n";
	static const uint8_t erase_ok[] = "ERASE OK\r\n";
	static const uint8_t erase_fail[] = "ERASE FAIL\r\n";
	static const uint8_t write_done[] = "WRITE DONE: REBOOTING\r\n";
	static const uint8_t write_fail[] = "WRITE FAIL\r\n";
	static const uint8_t crc_fail[] = "CRC FAIL\r\n";
	static const uint8_t metadata_fail[] = "METADATA FAIL\r\n";
	static const uint8_t version_reject[] = "VERSION FAIL\r\n";
  
	const uint8_t *status;
  uint16_t status_length;
	app_metadata_t metadata;

  HAL_UART_Transmit(&huart1, wait, sizeof(wait) - 1U, HAL_MAX_DELAY);

if (boot_wait_update_command())
{
    firmware_header_t header;

    if (!boot_receive_header(&header))
    {
        status = header_timeout;
        status_length = sizeof(header_timeout) - 1U;
    }
    else if (!boot_header_is_valid(&header))
    {
        status = header_bad;
        status_length = sizeof(header_bad) - 1U;
    }
    else if (!boot_metadata_read(&metadata))
    {
        status = metadata_fail;
        status_length = sizeof(metadata_fail) - 1U;
    }
    else if (boot_metadata_is_valid(&metadata) &&
             header.version < metadata.app_version)
    {
        status = version_reject;
        status_length = sizeof(version_reject) - 1U;
    }
    else if (!boot_metadata_begin_update(header.size,
                                         header.crc32,
                                         header.version))
    {
        status = metadata_fail;
        status_length = sizeof(metadata_fail) - 1U;
    }
    else if (!boot_flash_erase_app(header.size))
    {
        status = erase_fail;
        status_length = sizeof(erase_fail) - 1U;
    }
    else
    {
        HAL_UART_Transmit(&huart1, erase_ok,
                          sizeof(erase_ok) - 1U,
                          HAL_MAX_DELAY);

        if (!boot_receive_and_write(header.size))
        {
            status = write_fail;
            status_length = sizeof(write_fail) - 1U;
        }
        else
        {
            uint32_t flash_crc =
                boot_crc32_flash(APP_BASE, header.size);

            if (flash_crc != header.crc32)
            {
                status = crc_fail;
                status_length = sizeof(crc_fail) - 1U;
            }
            else if (!boot_metadata_mark_valid())
            {
                status = metadata_fail;
                status_length = sizeof(metadata_fail) - 1U;
            }
            else
            {
                HAL_UART_Transmit(&huart1, write_done,
                                  sizeof(write_done) - 1U,
                                  HAL_MAX_DELAY);
                HAL_Delay(500U);
                NVIC_SystemReset();
            }
        }
    }
}
else
{
    if (boot_metadata_read(&metadata) &&
        boot_metadata_is_valid(&metadata) &&
        boot_app_is_valid())
    {
        uint32_t flash_crc =
            boot_crc32_flash(APP_BASE, metadata.app_size);

        if (flash_crc == metadata.app_crc32)
        {
            HAL_UART_Transmit(&huart1, jump,
                              sizeof(jump) - 1U,
                              HAL_MAX_DELAY);
            HAL_Delay(20U);
            boot_jump_to_app();
        }
    }

    status = invalid;
    status_length = sizeof(invalid) - 1U;
}
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* UPDATE 模式或 App 无效时，持续给出可观察的状态。 */
    HAL_GPIO_TogglePin(GPIOF, GPIO_PIN_9 | GPIO_PIN_10);
    HAL_UART_Transmit(&huart1, status, status_length, HAL_MAX_DELAY);
    HAL_Delay(1000U);
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
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
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
