#include "boot_update.h"
#include "boot_flash.h"

/* 串口接收 16 字节 → 成功返回 1，失败返回 0 */
int boot_receive_header(firmware_header_t *header)
{
	if(header == NULL)
	{
		return 0;
	}
	if(HAL_UART_Receive(&huart1,(uint8_t *)header,sizeof(*header),1000U) == HAL_OK)
	{
		return 1;
	}
	return 0;
}

/* 接收包头 
    每一包都接收12字节分包头 */
int boot_receive_chunk_header(firmware_chunk_header_t *header)
{
	if(header == NULL)
	{
		return 0;
	}
	if(HAL_UART_Receive(&huart1,(uint8_t *)header,sizeof(*header),1000U) == HAL_OK)
	{
		return 1;
	}
	return 0;	
}


int boot_chunk_header_is_valid(const firmware_chunk_header_t *header,uint32_t expected_sequence)
{
  if(header == NULL     ||
		 header->sequence != expected_sequence ||
	   header->length == 0U                  ||
	   header->length > FW_CHUNK_SIZE)
  {
    return 0;
  }	
	return 1;
}


/*  1. header->magic 是否等于 FW_MAGIC
    2. header->size 是否在合法范围内 */
int boot_header_is_valid(const firmware_header_t *header)
{
	if(header == NULL)
	{
		return 0;
	}
	if(header->magic == FW_MAGIC && header->size <= APP_MAX_SIZE && header->size != 0 && header->version != 0)
	{
		return 1;
	}
	return 0;
}

/* 输入：RAM 缓冲区地址 + 长度
   输出：这段 RAM 数据的 CRC32  */
uint32_t boot_crc32_buffer(const uint8_t *data, uint32_t length)
{
	if(data == NULL)
	{
		return 0;
	}
	
	uint32_t crc = 0xFFFFFFFFUL;
	
  for(uint32_t i = 0U; i < length; i++)
	{
		crc ^= data[i];
		for(uint32_t bit = 0U; bit < 8U; bit++)
		{
			if((crc & 1UL) != 0UL)
			{
				crc = (crc >> 1U) ^ CRC32_POLY;
			}else{
				crc >>= 1U;
			}
		}
	}
	return crc ^ 0xFFFFFFFFUL;
}

/*  从 Flash 的 address 开始读 length 个字节，
    返回这段数据的 CRC32。                 */
uint32_t boot_crc32_flash(uint32_t address, uint32_t length)
{
	return boot_crc32_buffer((const uint8_t *)address, length);
}

/*  把 status 放到 5 字节回复的第 0 字节
    把 sequence 放到后 4 字节
    通过 USART1 发出去
    成功返回 1，失败返回 0               */
int boot_send_chunk_reply(uint8_t status, uint32_t sequence)
{
	if(status != BOOT_REPLY_ACK && status != BOOT_REPLY_NACK && status != BOOT_REPLY_READY)
	{
		return 0;
	}
	
	uint8_t reply[5];
	reply[0] = status;
	for(uint8_t i = 1U;i < 5U;i++)
	{
		reply[i] = (uint8_t)(sequence >> ((i - 1U) * 8U));
	}
	
	if(HAL_UART_Transmit(&huart1, reply, sizeof(reply),HAL_MAX_DELAY) != HAL_OK)
	{
		return 0;
	}
	return 1;
}


