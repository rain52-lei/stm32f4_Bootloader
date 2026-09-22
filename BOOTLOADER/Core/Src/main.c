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
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define BOOT_LED_SELF_TEST  0U
#define BOOT_EARLY_LED_TEST 0U
#define BOOT_UPDATE_WINDOW_MS    2000UL   /* 普通上电的 UPDATE 命令窗口 */
#define BOOT_GOTOBOOT_WINDOW_MS  60000UL  /* App 留言复位后的加长窗口 */
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
static int metadata_slot_is_valid(const app_metadata_t *metadata, uint32_t slot)
{
  return metadata != NULL &&
         boot_slot_is_valid(slot) &&
         metadata->slot_state[slot] == SLOT_STATE_VALID;
}

static uint32_t select_active_slot(const app_metadata_t *metadata)
{
  uint32_t active = boot_metadata_active_index(metadata->active_slot);
  uint32_t other = (active == SLOT_A) ? SLOT_B : SLOT_A;

  if (metadata_slot_is_valid(metadata, active))
  {
    return active;
  }
  if (metadata_slot_is_valid(metadata, other))
  {
    return other;
  }
  return active;
}

static uint32_t select_download_slot(const app_metadata_t *metadata,
                                     uint32_t active_slot)
{
  if (!metadata_slot_is_valid(metadata, SLOT_A) &&
      !metadata_slot_is_valid(metadata, SLOT_B))
  {
    return SLOT_A;
  }
  return (active_slot == SLOT_A) ? SLOT_B : SLOT_A;
}

static int slot_is_bootable(const app_metadata_t *metadata, uint32_t slot)
{
  uint32_t base;
  uint32_t end;

  if (!metadata_slot_is_valid(metadata, slot))
  {
    return 0;
  }

  base = boot_slot_base(slot);
  end = boot_slot_end(slot);
  if (!boot_app_is_valid(base, end))
  {
    return 0;
  }

  return boot_crc32_flash(base, metadata->slot[slot].size) ==
         metadata->slot[slot].crc32;
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
  static const uint8_t wait_a[] = "WAIT UPDATE ACTIVE=A DOWNLOAD=A\r\n";
  static const uint8_t wait_ab[] = "WAIT UPDATE ACTIVE=A DOWNLOAD=B\r\n";
  static const uint8_t wait_ba[] = "WAIT UPDATE ACTIVE=B DOWNLOAD=A\r\n";
  static const uint8_t jump_a[] = "TIMEOUT: JUMPING TO APP A\r\n";
  static const uint8_t jump_b[] = "TIMEOUT: JUMPING TO APP B\r\n";
  static const uint8_t fallback[] = "ACTIVE INVALID: FALLBACK TO OTHER SLOT\r\n";
  static const uint8_t invalid[] = "APP INVALID: STAY IN BOOTLOADER\r\n";
  static const uint8_t header_bad[] = "HEADER BAD\r\n";
  static const uint8_t header_timeout[] = "HEADER TIMEOUT\r\n";
  static const uint8_t slot_mismatch[] = "SLOT MISMATCH\r\n";
  static const uint8_t erase_ok[] = "ERASE OK\r\n";
  static const uint8_t erase_fail[] = "ERASE FAIL\r\n";
  static const uint8_t write_done[] = "WRITE DONE: REBOOTING\r\n";
  static const uint8_t write_fail[] = "WRITE FAIL\r\n";
  static const uint8_t crc_fail[] = "CRC FAIL\r\n";
  static const uint8_t metadata_fail[] = "METADATA FAIL\r\n";
  static const uint8_t version_reject[] = "VERSION FAIL\r\n";
  static const uint8_t goto_boot_notice[] = "GOTOBOOT FLAG: EXTENDED UPDATE WINDOW\r\n";

  const uint8_t *status = NULL;
  uint16_t status_length = 0U;
  const uint8_t *wait_msg = wait_a;
  uint16_t wait_len = sizeof(wait_a) - 1U;
  uint32_t first_pass = 1U;
  app_metadata_t metadata;
  uint32_t active_slot;
  uint32_t download_slot;
  uint32_t goto_boot;

  if (!boot_metadata_read(&metadata) || !boot_metadata_is_valid(&metadata))
  {
    boot_metadata_default(&metadata);
  }
  active_slot = select_active_slot(&metadata);
  download_slot = select_download_slot(&metadata, active_slot);

  /* App 留过言就开长窗口：给上位机足够时间发起升级；普通上电仍是 2 秒 */
  goto_boot = boot_goto_boot_requested();
  if (goto_boot != 0U)
  {
    HAL_UART_Transmit(&huart1, goto_boot_notice,
                      sizeof(goto_boot_notice) - 1U, HAL_MAX_DELAY);
  }

  /* 横幅存进指针供失败循环复播：上位机见到 WAIT UPDATE 即可再次发起升级，
     不必按复位重新掐 2 秒窗口 */
  if (active_slot == SLOT_B)
  {
    wait_msg = wait_ba;
    wait_len = sizeof(wait_ba) - 1U;
  }
  else if (download_slot == SLOT_B)
  {
    wait_msg = wait_ab;
    wait_len = sizeof(wait_ab) - 1U;
  }
  HAL_UART_Transmit(&huart1, wait_msg, wait_len, HAL_MAX_DELAY);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* 长窗口只在进门第一轮生效；失败后的重试轮次仍是 2 秒 */
    uint32_t window_ms = (goto_boot != 0U) ?
                         BOOT_GOTOBOOT_WINDOW_MS : BOOT_UPDATE_WINDOW_MS;

    goto_boot = 0U;

    if (boot_wait_update_command(window_ms))
    {
      /* 一旦进入过升级会话就不再自动跳 App：失败后留在 bootloader
         等待重试（与原"失败常驻"行为一致），回旧固件走复位 */
      first_pass = 0U;
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
      else if (header.image_slot != download_slot)
      {
        status = slot_mismatch;
        status_length = sizeof(slot_mismatch) - 1U;
      }
      else if (metadata_slot_is_valid(&metadata, active_slot) &&
               header.version < metadata.slot[active_slot].version)
      {
        status = version_reject;
        status_length = sizeof(version_reject) - 1U;
      }
      else
      {
        app_metadata_t metadata_for_update = metadata;

        metadata_for_update.active_slot =
            (active_slot == SLOT_B) ? METADATA_ACTIVE_B : METADATA_ACTIVE_A;

        if (!boot_metadata_begin_update(&metadata_for_update, download_slot,
                                        header.size, header.crc32,
                                        header.version))
        {
          status = metadata_fail;
          status_length = sizeof(metadata_fail) - 1U;
        }
        else if (!boot_flash_erase_slot(download_slot, header.size))
        {
          status = erase_fail;
          status_length = sizeof(erase_fail) - 1U;
        }
        else
        {
          HAL_UART_Transmit(&huart1, erase_ok,
                            sizeof(erase_ok) - 1U, HAL_MAX_DELAY);

          if (!boot_receive_and_write(download_slot, header.size))
          {
            status = write_fail;
            status_length = sizeof(write_fail) - 1U;
          }
          else if (boot_crc32_flash(boot_slot_base(download_slot), header.size) !=
                   header.crc32)
          {
            status = crc_fail;
            status_length = sizeof(crc_fail) - 1U;
          }
          else if (!boot_metadata_mark_valid(download_slot))
          {
            status = metadata_fail;
            status_length = sizeof(metadata_fail) - 1U;
          }
          else if (download_slot != active_slot && !boot_metadata_switch_active())
          {
            status = metadata_fail;
            status_length = sizeof(metadata_fail) - 1U;
          }
          else
          {
            HAL_UART_Transmit(&huart1, write_done,
                              sizeof(write_done) - 1U, HAL_MAX_DELAY);
            HAL_Delay(500U);
            NVIC_SystemReset();
          }
        }
      }
      /* 落到这里 = 本次升级会话失败：status 已带原因，循环底统一上报后
         继续监听，上位机见到 WAIT UPDATE 横幅即可直接重发 UPDATE 重试 */
    }
    else if (first_pass)
    {
      /* 只有开机第一轮没收到 UPDATE 才尝试跳 App；升级失败后留在
         bootloader 保持可服务，按复位即可回旧固件 */
      uint32_t boot_slot = active_slot;
      uint32_t other_slot = (active_slot == SLOT_A) ? SLOT_B : SLOT_A;
      uint32_t active_ok;
      uint32_t other_ok;

      first_pass = 0U;
      active_ok = slot_is_bootable(&metadata, boot_slot);
      other_ok = slot_is_bootable(&metadata, other_slot);

      if (!active_ok && other_ok)
      {
        HAL_UART_Transmit(&huart1, fallback,
                          sizeof(fallback) - 1U, HAL_MAX_DELAY);
        boot_slot = other_slot;
      }

      if (active_ok || other_ok)
      {
        const uint8_t *jump_msg = (boot_slot == SLOT_A) ? jump_a : jump_b;
        uint16_t jump_len = (boot_slot == SLOT_A) ?
                            (sizeof(jump_a) - 1U) : (sizeof(jump_b) - 1U);

        HAL_UART_Transmit(&huart1, jump_msg, jump_len, HAL_MAX_DELAY);
        HAL_Delay(20U);
        boot_jump_to_app(boot_slot_base(boot_slot), boot_slot_end(boot_slot));
      }

      status = invalid;
      status_length = sizeof(invalid) - 1U;
    }

    /* 升级失败或无可用 App：报一次原因、复播等待横幅，回到循环顶继续
       监听 UPDATE——重试升级不再要求手动复位 */
    if (status != NULL)
    {
      HAL_GPIO_TogglePin(GPIOF, GPIO_PIN_9 | GPIO_PIN_10);
      HAL_UART_Transmit(&huart1, status, status_length, HAL_MAX_DELAY);
      HAL_UART_Transmit(&huart1, wait_msg, wait_len, HAL_MAX_DELAY);
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
