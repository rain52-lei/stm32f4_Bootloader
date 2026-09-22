#ifndef __BOOT_JUMP_H
#define __BOOT_JUMP_H

#include <stdint.h>

int boot_app_is_valid(uint32_t app_base, uint32_t app_end);
void boot_jump_to_app(uint32_t app_base, uint32_t app_end);
int boot_goto_boot_requested(void);
int boot_wait_update_command(uint32_t timeout_ms);

#endif
