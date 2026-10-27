/* Execute the real boot_flash/update/metadata C against a mapped mock Flash.
   This tests software state transitions, not STM32 electrical power-loss behavior. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "boot_flash.h"
#include "boot_update.h"
#include "boot_metadata.h"

UART_HandleTypeDef huart1;
static unsigned program_count, erase_count, erase_sector;
static int fail_erase, fail_program, fail_tx;
static uint32_t tick;
static uint8_t input[8192];
static size_t input_len, input_pos;
static uint8_t reply_status[128];
static uint32_t reply_sequence[128];
static size_t reply_count;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while(0)

uint32_t HAL_GetTick(void) { return tick++; }
HAL_StatusTypeDef HAL_FLASH_Unlock(void) { return HAL_OK; }
HAL_StatusTypeDef HAL_FLASH_Lock(void) { return HAL_OK; }
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *e, uint32_t *error)
{
    static const uint32_t offset[] = {0,0x4000,0x8000,0xC000,0x10000,
        0x20000,0x40000,0x60000,0x80000,0xA0000,0xC0000,0xE0000,0x100000};
    erase_count++; erase_sector=e->Sector;
    if (fail_erase) { *error=e->Sector; return HAL_ERROR; }
    CHECK(e->Sector+e->NbSectors <= 12);
    memset((void *)(uintptr_t)(0x08000000+offset[e->Sector]),0xFF,
           offset[e->Sector+e->NbSectors]-offset[e->Sector]);
    return HAL_OK;
}
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t type, uint32_t address, uint64_t value)
{
    uint32_t *word=(uint32_t *)(uintptr_t)address;
    (void)type;
    CHECK(address>=0x0800C000 && address+4<=0x08100000);
    program_count++;
    if (fail_program) return HAL_ERROR;
    if ((*word & (uint32_t)value)!=(uint32_t)value) return HAL_ERROR;
    *word &= (uint32_t)value;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_UART_Receive(UART_HandleTypeDef *u, uint8_t *data,
                                    uint16_t length, uint32_t timeout)
{
    size_t available=input_len-input_pos;
    size_t count=available<length?available:length;
    (void)u;
    memcpy(data,input+input_pos,count); input_pos+=count;
    if (count<length) { tick+=timeout; return HAL_TIMEOUT; }
    tick++; return HAL_OK;
}
HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *u,const uint8_t *data,
                                     uint16_t length,uint32_t timeout)
{
    (void)u; (void)timeout;
    if (fail_tx) return HAL_ERROR;
    CHECK(length==5 && reply_count<128);
    reply_status[reply_count]=data[0];
    memcpy(&reply_sequence[reply_count],data+1,4); reply_count++;
    return HAL_OK;
}
static void reset_io(void)
{
    input_len=input_pos=reply_count=0;
    fail_tx=fail_erase=fail_program=0;
    program_count=erase_count=0; tick=0;
}
static void append(const void *data,size_t length)
{
    CHECK(input_len+length<=sizeof(input));
    memcpy(input+input_len,data,length); input_len+=length;
}
static void header(uint32_t seq,const uint8_t *data,uint32_t length,int corrupt)
{
    uint8_t sync[]={0xAA,0x55};
    firmware_chunk_header_t h={seq,length,boot_crc32_buffer(data,length)};
    if (corrupt) h.crc32^=1;
    append(sync,2); append(&h,sizeof(h));
}
static void seal(app_metadata_t *m)
{
    m->header_crc32=boot_crc32_buffer((uint8_t *)m,offsetof(app_metadata_t,slot_state));
}
static void seed(unsigned copy,uint32_t generation)
{
    app_metadata_t m;
    boot_metadata_default(&m);
    m.generation=generation;
    m.slot[SLOT_A].size=256; m.slot[SLOT_A].version=10;
    m.slot[SLOT_A].crc32=0x12345678; m.slot_state[SLOT_A]=SLOT_STATE_VALID;
    seal(&m);
    memcpy((void *)(uintptr_t)(copy?METADATA_1_BASE:METADATA_0_BASE),&m,sizeof(m));
}
int main(void)
{
    uint8_t data[300];
    app_metadata_t current,stale,after;
    unsigned i, acks;
    void *flash=VirtualAlloc((void *)(uintptr_t)0x08000000,0x100000,
                             MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    CHECK(flash==(void *)(uintptr_t)0x08000000);
    memset(flash,0xFF,0x100000);
    for(i=0;i<sizeof(data);i++) data[i]=(uint8_t)(i*17+3);

    CHECK(boot_crc32_buffer((const uint8_t *)"123456789",9)==0xCBF43926);
    CHECK(boot_crc32_buffer(data,0)==0);
    firmware_header_t fw={FW_MAGIC,8,0,1,SLOT_B};
    CHECK(boot_header_is_valid(&fw)); fw.image_slot=2; CHECK(!boot_header_is_valid(&fw));
    fw.image_slot=SLOT_A; fw.size=7; CHECK(!boot_header_is_valid(&fw));
    puts("PASS CRC vectors and firmware header ranges");

    reset_io();
    CHECK(!boot_flash_write_chunk(SLOT_A,SLOT_A_END-4,data,5));
    CHECK(!boot_flash_write_chunk(SLOT_A,SLOT_B_BASE,data,4));
    CHECK(!boot_flash_write_chunk(SLOT_B,SLOT_A_BASE,data,4));
    CHECK(!boot_flash_write_chunk(2,SLOT_B_BASE,data,4));
    CHECK(!boot_flash_write_chunk(SLOT_B,SLOT_B_BASE+1,data,4));
    CHECK(program_count==0);
    CHECK(boot_flash_write_chunk(SLOT_B,SLOT_B_END-4,data,3));
    CHECK(*((uint8_t *)(uintptr_t)SLOT_B_END-1)==0xFF);
    puts("PASS slot boundaries, alignment, and final-word padding");

    memset(flash,0xFF,0x100000); reset_io();
    /* Stream includes last-committed header repetitions, without data. */
    header(0,data,256,0); append(data,256);
    header(0,data,256,0);
    header(1,data+256,44,0); append(data+256,44);
    header(1,data+256,44,0); /* final ACK replay */
    CHECK(boot_receive_and_write(SLOT_B,300));
    CHECK(memcmp((void *)(uintptr_t)SLOT_B_BASE,data,300)==0);
    CHECK(program_count==75); /* two duplicates must not program again */
    acks=0;
    for(i=0;i<reply_count;i++) if(reply_status[i]==BOOT_REPLY_ACK) acks++;
    CHECK(acks==4 && reply_count==6);
    CHECK(reply_status[2]==BOOT_REPLY_ACK && reply_sequence[2]==0);
    CHECK(reply_status[5]==BOOT_REPLY_ACK && reply_sequence[5]==1);
    puts("PASS intermediate/final lost-ACK replay without double Flash programming");

    memset(flash,0xFF,0x100000); reset_io();
    header(0,data,44,1); append(data,44);
    header(0,data,44,0); append(data,44);
    CHECK(boot_receive_and_write(SLOT_A,44)); CHECK(program_count==11);
    CHECK(reply_status[1]==BOOT_REPLY_NACK);
    puts("PASS CRC NACK then same-sequence recovery");

    reset_io(); fail_tx=1;
    CHECK(!boot_receive_and_write(SLOT_A,4)); CHECK(program_count==0);
    reset_io(); fail_program=1;
    header(0,data,4,0); append(data,4);
    CHECK(!boot_receive_and_write(SLOT_B,4)); CHECK(program_count==1);
    puts("PASS failed replies and Flash errors abort the session");

    memset(flash,0xFF,0x100000); reset_io();
    seed(0,10);
    CHECK(boot_metadata_read(&current)); stale=current;
    CHECK(boot_metadata_begin_update(&current,SLOT_B,300,0x87654321,11));
    CHECK(erase_sector==4);
    CHECK(boot_metadata_read(&after) && after.generation==11);
    CHECK(after.slot_state[SLOT_A]==SLOT_STATE_VALID);
    CHECK(after.slot_state[SLOT_B]==SLOT_STATE_UPDATING);
    i=erase_count;
    CHECK(!boot_metadata_begin_update(&stale,SLOT_B,300,0,12)); CHECK(erase_count==i);
    CHECK(boot_metadata_begin_update(&after,SLOT_B,300,0x87654321,12));
    CHECK(erase_sector==3);
    CHECK(boot_metadata_read(&current) && current.generation==12);
    CHECK(boot_metadata_mark_valid(SLOT_B));
    CHECK(boot_metadata_switch_active());
    CHECK(boot_metadata_read(&current));
    CHECK(current.slot_state[SLOT_B]==SLOT_STATE_VALID);
    CHECK(boot_metadata_active_index(current.active_slot)==SLOT_B);
    puts("PASS consecutive sessions preserve old slot and reject stale RAM snapshot");

    memset((void *)(uintptr_t)METADATA_0_BASE,0xFF,sizeof(app_metadata_t));
    CHECK(boot_metadata_read(&current) && current.generation==11);
    memset((void *)(uintptr_t)METADATA_1_BASE,0xFF,sizeof(app_metadata_t));
    CHECK(!boot_metadata_read(&current));
    boot_metadata_default(&current);
    CHECK(boot_metadata_begin_update(&current,SLOT_A,32,0,1)); CHECK(erase_sector==3);
    puts("PASS single-copy fallback and deterministic first-write after both invalid");

    memset(flash,0xFF,0x100000); seed(0,0xFFFFFFFFU); seed(1,0);
    CHECK(boot_metadata_read(&current) && current.generation==0);
    current.active_slot=METADATA_ACTIVE_A;
    fail_erase=1;
    CHECK(!boot_metadata_begin_update(&current,SLOT_B,300,0,12));
    CHECK(boot_metadata_read(&after) && after.generation==0);
    puts("PASS generation wraparound and failed metadata erase preserve trusted copy");
    VirtualFree(flash,0,MEM_RELEASE);
    puts("ALL NATIVE FIRMWARE TESTS PASSED");
    return 0;
}
