#ifndef BOOT_FLASH_H
#define BOOT_FLASH_H

#include <stdint.h>

#define APP_FIRST_SECTOR  5U
#define APP_SECTOR_COUNT  7U
#define APP_SECTOR_SIZE   0x00020000UL
#define APP_BASE      0x08020000UL
#define APP_END       0x08100000UL
/* 包接收的长度 */
#define FW_CHUNK_SIZE  256U
/* 包最大发送次数 */
#define FW_CHUNK_MAX_RETRIES  3U

int boot_flash_erase_app(uint32_t app_size);
int boot_flash_write_chunk(uint32_t address, const uint8_t *data, uint32_t length);
int boot_receive_chunk(uint8_t *buffer, uint32_t length);
int boot_receive_and_write(uint32_t total_size);

#endif
