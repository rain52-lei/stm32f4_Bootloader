#include "boot_flash.h"
#include "boot_update.h"
#include "stm32f4xx_hal.h"
#include "usart.h"

static uint8_t buffer[FW_CHUNK_SIZE];  //每次串口接收的 256 字节 RAM 缓冲区

int boot_flash_erase_app(uint32_t app_size)
{
	FLASH_EraseInitTypeDef erase = {0};
	uint32_t sector_error = 0;
	uint32_t nb_sectors;
	
	if(app_size == 0UL)
	{
     return 0;		
	}
	
	nb_sectors = (app_size + APP_SECTOR_SIZE - 1UL) / APP_SECTOR_SIZE;
	if(nb_sectors > APP_SECTOR_COUNT)
	{
		return 0;
	}
	
	if(HAL_FLASH_Unlock() != HAL_OK)
	{
		return 0;
	}
	
	erase.TypeErase     = FLASH_TYPEERASE_SECTORS;  /*按扇区擦除*/
	erase.Banks         = FLASH_BANK_1;             /*F407只有BANK1*/
	erase.Sector        = APP_FIRST_SECTOR;         /*从Sector5开始*/
	erase.NbSectors     = nb_sectors;               /*擦除nb_scetors个*/
	erase.VoltageRange  = FLASH_VOLTAGE_RANGE_3;    /*3.3V供电*/
	
	if(HAL_FLASHEx_Erase(&erase, &sector_error) != HAL_OK)
	{
		HAL_FLASH_Lock();
		return 0;
	}
	
	HAL_FLASH_Lock();
	return 1;
}

/* 每次向flash的address的地址写入长度为length的*data的数据
     从 data 指向的 RAM 数据开始，
     取 length 个字节，
     写入从 address 开始的 Flash，
     成功返回 1，失败返回 0        */
int boot_flash_write_chunk(uint32_t address, const uint8_t *data, uint32_t length)
{
	uint32_t word;
	
	if(data == NULL  || 
		 length == 0U  || 
	   address < APP_BASE  ||
     address >= APP_END  || 
	   length > (APP_END - address) ||
	   address %4U != 0 )              //(address & 3U) != 0U
	{
		return 0;
	}
	
	if(HAL_FLASH_Unlock() != HAL_OK)
	{
    return 0;
	}		//解锁FLASH
	
	for(uint32_t i = 0U; i<(length / 4U); i++)
	{
		
		word = ((uint32_t)data[0]) |                   // *data | 
		       ((uint32_t)data[1] << 8U) |             // *(data + 1) << 8U  | 
		       ((uint32_t)data[2] << 16U) |            // *(data + 2) << 16U | 
		       ((uint32_t)data[3] << 24U);            // *(data + 3) << 24U ; 
		
		if(HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address, word) != HAL_OK)
		{
			    HAL_FLASH_Lock();   //上锁FLASH
			    return 0;
		}else{
					address += 4U;
		      data = data + 4U;
		}
	}
	
	  uint32_t remain = length % 4U;
	
	  if(remain != 0U)
	  {
	  	word = 0xFFFFFFFFUL;
 
	    for(uint32_t i = 0U ; i < remain; i++)
      {
        word &= ~(0xFFUL << (8U * i));
	    	word |= ((uint32_t)data[i] << (8U * i));
      } 	
		  
		  if(HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address, word) != HAL_OK)
		  {
		  	    HAL_FLASH_Lock();   //上锁FLASH
		  	    return 0;
		  }
	  }		
	
	if(HAL_FLASH_Lock() != HAL_OK)
	{
		return 0;
	}
	return 1;
}

/* 从 USART1 接收 length 个字节，
   放到 buffer 指向的 RAM 中，
   成功返回 1，失败返回 0     */
int boot_receive_chunk(uint8_t *buffer, uint32_t length)
{
	if(buffer == NULL ||
		 length == 0    ||
	   length > FW_CHUNK_SIZE)
	{
		return 0;
	}
	
	if(HAL_UART_Receive(&huart1, buffer, length, 3000U) != HAL_OK)
	{
		return 0;
	}
	
	return 1;
}

/* 将接受的数据写入FLASH中 */
int boot_receive_and_write(uint32_t total_size)
{
    uint32_t received = 0U;
    uint32_t expected_sequence = 0U;

    while (received < total_size)
    {
        uint32_t remain = total_size - received;
        uint32_t chunk_size =
            (remain > FW_CHUNK_SIZE) ? FW_CHUNK_SIZE : remain;

        uint32_t retry_count = 0U;
        uint8_t chunk_success = 0U;

        while (retry_count < FW_CHUNK_MAX_RETRIES)
        {
            firmware_chunk_header_t chunk_header;

            /* 接收分包头失败 */
            if (!boot_receive_chunk_header(&chunk_header))
            {
                retry_count++;
                boot_send_chunk_reply(BOOT_REPLY_NACK,
                                       expected_sequence);
                continue;
            }

            /* 检查序号和长度 */
            if (!boot_chunk_header_is_valid(&chunk_header,
                                            expected_sequence) ||
                chunk_header.length != chunk_size)
            {
                retry_count++;
                boot_send_chunk_reply(BOOT_REPLY_NACK,
                                       expected_sequence);
                continue;
            }

            /* 包头正确，允许上位机发送数据 */
            if (!boot_send_chunk_reply(BOOT_REPLY_READY,
                                       expected_sequence))
            {
                return 0;
            }

            /* 接收数据 */
            if (!boot_receive_chunk(buffer, chunk_header.length))
            {
                retry_count++;
                boot_send_chunk_reply(BOOT_REPLY_NACK,
                                       expected_sequence);
                continue;
            }

            /* 检查本包 CRC */
            if (boot_crc32_buffer(buffer, chunk_header.length) !=
                chunk_header.crc32)
            {
                retry_count++;
                boot_send_chunk_reply(BOOT_REPLY_NACK,
                                       expected_sequence);
                continue;
            }

            /* 写入 Flash */
            if (!boot_flash_write_chunk(APP_BASE + received,
                                         buffer,
                                         chunk_header.length))
            {
                retry_count++;
                boot_send_chunk_reply(BOOT_REPLY_NACK,
                                       expected_sequence);
                continue;
            }

            /* 本包成功 */
            if (!boot_send_chunk_reply(BOOT_REPLY_ACK,
                                       expected_sequence))
            {
                return 0;
            }

            received += chunk_header.length;
            expected_sequence++;
            chunk_success = 1U;
            break;
        }

        if (!chunk_success)
        {
            return 0;
        }
    }

    return 1;
}
