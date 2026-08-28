#ifndef __BOOT_JUMP_H
#define __BOOT_JUMP_H

int boot_app_is_valid(void);
void boot_jump_to_app(void);
int boot_wait_update_command(void);

#endif
