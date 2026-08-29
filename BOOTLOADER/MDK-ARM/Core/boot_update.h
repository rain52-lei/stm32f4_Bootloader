#ifndef BOOT_UPDATE_H
#define BOOT_UPDATE_H

#include <stdint.h>
#include "stm32f4xx_hal.h"
#include "usart.h"

/* 升级包头的固定身份标记，由上位机和 Bootloader 共同遵守。 */
#define FW_MAGIC  0x55AA1234UL
/* APP的最大写入长度 */
#define APP_MAX_SIZE  0x000E0000UL
/* crc检验的多项式 */
#define CRC32_POLY   0xEDB88320UL
/* 包回应 跟 应答 */
#define BOOT_REPLY_ACK    0x06U
#define BOOT_REPLY_NACK   0x15U
#define BOOT_REPLY_READY  0x16U

#define CHUNK_SYNC0           0xAAU
#define CHUNK_SYNC1           0x55U
#define SYNC_SCAN_TIMEOUT_MS  5000U

/*
 * 串口升级时最先传输的 16 字节。
 * 四个成员都是 uint32_t，因此此结构体的大小为 16 字节。
 */
typedef struct
{
    uint32_t magic;
    uint32_t size;
    uint32_t crc32;
	  uint32_t version;
} firmware_header_t;

typedef struct 
{
	uint32_t sequence;
	uint32_t length;
	uint32_t crc32;
}firmware_chunk_header_t;

int boot_receive_header(firmware_header_t *header);
int boot_receive_chunk_header(firmware_chunk_header_t *header);
int boot_header_is_valid(const firmware_header_t *header);
int boot_chunk_header_is_valid(const firmware_chunk_header_t *header,uint32_t expected_sequence);
int boot_send_chunk_reply(uint8_t status, uint32_t sequence);
uint32_t boot_crc32_buffer(const uint8_t *data, uint32_t length);
uint32_t boot_crc32_flash(uint32_t address, uint32_t length);

#endif
