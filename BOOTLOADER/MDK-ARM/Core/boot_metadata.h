#ifndef BOOT_METADATA_H
#define BOOT_METADATA_H


#include <stdint.h>

#define METADATA_SECTOR       4U
#define METADATA_MAGIC        0x4D455441UL
#define METADATA_BASE         0x08010000UL
#define METADATA_END          0x08020000UL

#define APP_STATE_EMPTY       0xFFFFFFFFUL
#define APP_STATE_UPDATING    0x7FFFFFFFUL
#define APP_STATE_VALID       0x3FFFFFFFUL

typedef struct{
	uint32_t magic;
	uint32_t app_size;
	uint32_t app_crc32;
	uint32_t app_version;
	uint32_t state;
}app_metadata_t;

int boot_metadata_begin_update(uint32_t app_size, uint32_t app_crc32, uint32_t app_version);
int boot_metadata_mark_valid(void);
int boot_metadata_read(app_metadata_t *metadata);
int boot_metadata_is_valid(const app_metadata_t *metadata);

#endif


