#include "boot_metadata.h"
#include "boot_update.h"
#include "stm32f4xx_hal.h"

int boot_metadata_begin_update(uint32_t app_size, uint32_t app_crc32, uint32_t app_version)
{
  if(app_size == 0U || app_size > APP_MAX_SIZE)
	{
    return 0;
	}

  FLASH_EraseInitTypeDef erase = {0};
	uint32_t sector_error;
	erase.TypeErase = FLASH_TYPEERASE_SECTORS;
  erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
  erase.Sector = METADATA_SECTOR;
  erase.NbSectors = 1U;
	
	if(HAL_FLASH_Unlock() != HAL_OK)
	{
		return 0;
	}
	
	if(HAL_FLASHEx_Erase(&erase, &sector_error) != HAL_OK)
	{
		HAL_FLASH_Lock(); //上锁
		return 0;
	}
  
  app_metadata_t metadata;
  metadata.magic       =  METADATA_MAGIC;
  metadata.app_size    =  app_size;
  metadata.app_crc32   =  app_crc32;
	metadata.app_version =  app_version;
  metadata.state       =  APP_STATE_UPDATING; 
	
	if(HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, METADATA_BASE, metadata.magic) != HAL_OK)
	{
		HAL_FLASH_Lock(); //上锁
		return 0;		
	}
	if(HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, METADATA_BASE + 4U, metadata.app_size) != HAL_OK)
	{
		HAL_FLASH_Lock(); //上锁
		return 0;		
	}
	if(HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, METADATA_BASE + 8U, metadata.app_crc32) != HAL_OK)
	{
		HAL_FLASH_Lock(); //上锁
		return 0;		
	}
  if(HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, METADATA_BASE + 12U, metadata.app_version) != HAL_OK)
	{
		HAL_FLASH_Lock(); //上锁
		return 0;		
	}
	if(HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, METADATA_BASE + 16U, metadata.state) != HAL_OK)
	{
		HAL_FLASH_Lock(); //上锁
		return 0;		
	}
	
	HAL_FLASH_Lock();
	return 1;
}

int boot_metadata_mark_valid(void)
{
	if(HAL_FLASH_Unlock() != HAL_OK)
	{
		return 0;
	}
	
	if(HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, METADATA_BASE+16U, APP_STATE_VALID) != HAL_OK)
	{
		HAL_FLASH_Lock();
		return 0;
	}
	
	HAL_FLASH_Lock();
	
	return 1;
}

int boot_metadata_read(app_metadata_t *metadata)
{
	if(metadata == NULL)
	{
		return 0;
	}
	
	const app_metadata_t *temp = (const app_metadata_t*)METADATA_BASE;

  *metadata = *temp;	
	
	return 1;
}

int boot_metadata_is_valid(const app_metadata_t *metadata)
{
  if(metadata == NULL ||
		 metadata->magic != METADATA_MAGIC    ||
	   metadata->app_version == 0U           ||
	   metadata->state != APP_STATE_VALID   ||
	   metadata->app_size ==0U              ||
	   metadata->app_size > APP_MAX_SIZE)
	{
    return 0;
	}	
	return 1;
}



