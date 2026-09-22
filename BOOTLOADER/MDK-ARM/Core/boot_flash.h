#ifndef BOOT_FLASH_H
#define BOOT_FLASH_H

#include <stdint.h>

/* STM32F407ZG 1 MB Flash layout (all end addresses are exclusive). */
#define SLOT_A_BASE          0x08020000UL
#define SLOT_A_END           0x080A0000UL
#define SLOT_A_FIRST_SECTOR  5U
#define SLOT_A_SECTOR_COUNT  4U

#define SLOT_B_BASE          0x080A0000UL
#define SLOT_B_END           0x08100000UL
#define SLOT_B_FIRST_SECTOR  9U
#define SLOT_B_SECTOR_COUNT  3U

#define APP_FLASH_END        SLOT_B_END
#define APP_SECTOR_SIZE      0x00020000UL
#define APP_MAX_SIZE         0x00060000UL

/* Compatibility alias for code that still refers to the original slot A. */
#define APP_BASE             SLOT_A_BASE
#define APP_END              APP_FLASH_END

#define FW_CHUNK_SIZE         256U
#define FW_CHUNK_MAX_RETRIES  3U

int boot_slot_is_valid(uint32_t slot);
uint32_t boot_slot_base(uint32_t slot);
uint32_t boot_slot_end(uint32_t slot);
int boot_flash_erase_slot(uint32_t slot, uint32_t app_size);
int boot_flash_write_chunk(uint32_t address, const uint8_t *data, uint32_t length);
int boot_receive_chunk(uint8_t *buffer, uint32_t length);
int boot_receive_and_write(uint32_t slot, uint32_t total_size);

#endif
