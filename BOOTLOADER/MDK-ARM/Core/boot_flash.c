#include "boot_flash.h"
#include "boot_metadata.h"
#include "boot_update.h"
#include "stm32f4xx_hal.h"
#include "usart.h"

static uint8_t buffer[FW_CHUNK_SIZE];

int boot_slot_is_valid(uint32_t slot)
{
    return slot == SLOT_A || slot == SLOT_B;
}

uint32_t boot_slot_base(uint32_t slot)
{
    if (slot == SLOT_A)
    {
        return SLOT_A_BASE;
    }
    if (slot == SLOT_B)
    {
        return SLOT_B_BASE;
    }
    return 0U;
}

uint32_t boot_slot_end(uint32_t slot)
{
    if (slot == SLOT_A)
    {
        return SLOT_A_END;
    }
    if (slot == SLOT_B)
    {
        return SLOT_B_END;
    }
    return 0U;
}

int boot_flash_erase_slot(uint32_t slot, uint32_t app_size)
{
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t sector_error = 0U;
    uint32_t first_sector;
    uint32_t sector_count;
    uint32_t nb_sectors;

    if (!boot_slot_is_valid(slot) || app_size == 0U || app_size > APP_MAX_SIZE)
    {
        return 0;
    }

    first_sector = (slot == SLOT_A) ? SLOT_A_FIRST_SECTOR : SLOT_B_FIRST_SECTOR;
    sector_count = (slot == SLOT_A) ? SLOT_A_SECTOR_COUNT : SLOT_B_SECTOR_COUNT;
    nb_sectors = (app_size + APP_SECTOR_SIZE - 1U) / APP_SECTOR_SIZE;
    if (nb_sectors > sector_count)
    {
        return 0;
    }

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        return 0;
    }

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Banks = FLASH_BANK_1;
    erase.Sector = first_sector;
    erase.NbSectors = nb_sectors;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    if (HAL_FLASHEx_Erase(&erase, &sector_error) != HAL_OK)
    {
        HAL_FLASH_Lock();
        return 0;
    }

    HAL_FLASH_Lock();
    return 1;
}

int boot_flash_write_chunk(uint32_t slot, uint32_t address,
                           const uint8_t *data, uint32_t length)
{
    uint32_t word;
    uint32_t base = boot_slot_base(slot);
    uint32_t end = boot_slot_end(slot);

    if (base == 0U || data == NULL || length == 0U || address < base ||
        address >= end || length > (end - address) ||
        (address % 4U) != 0U)
    {
        return 0;
    }

    /* The last partial word is still a four-byte Flash operation. */
    if ((length % 4U) != 0U &&
        (4U - (length % 4U)) > (end - address - length))
    {
        return 0;
    }

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        return 0;
    }

    for (uint32_t i = 0U; i < (length / 4U); i++)
    {
        word = ((uint32_t)data[0]) |
               ((uint32_t)data[1] << 8U) |
               ((uint32_t)data[2] << 16U) |
               ((uint32_t)data[3] << 24U);

        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address, word) != HAL_OK)
        {
            HAL_FLASH_Lock();
            return 0;
        }
        if (*(volatile const uint32_t *)address != word)
        {
            HAL_FLASH_Lock();
            return 0;
        }
        address += 4U;
        data += 4U;
    }

    {
        uint32_t remain = length % 4U;
        if (remain != 0U)
        {
            word = 0xFFFFFFFFUL;
            for (uint32_t i = 0U; i < remain; i++)
            {
                word &= ~(0xFFUL << (8U * i));
                word |= ((uint32_t)data[i] << (8U * i));
            }
            if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address, word) != HAL_OK)
            {
                HAL_FLASH_Lock();
                return 0;
            }
            if (*(volatile const uint32_t *)address != word)
            {
                HAL_FLASH_Lock();
                return 0;
            }
        }
    }

    return (HAL_FLASH_Lock() == HAL_OK) ? 1 : 0;
}

int boot_receive_chunk(uint8_t *target, uint32_t length)
{
    if (target == NULL || length == 0U || length > FW_CHUNK_SIZE)
    {
        return 0;
    }

    return (HAL_UART_Receive(&huart1, target, length, 3000U) == HAL_OK) ? 1 : 0;
}

static int chunk_matches(const firmware_chunk_header_t *header,
                          const firmware_chunk_header_t *committed)
{
    return header->sequence == committed->sequence &&
           header->length == committed->length &&
           header->crc32 == committed->crc32;
}

/* Do not commit metadata/reset immediately after the final ACK. The host
   must have a bounded chance to repeat a header if that ACK was lost. */
static int finish_chunk_replies(const firmware_chunk_header_t *committed)
{
    uint32_t start = HAL_GetTick();
    uint32_t replies = 0U;

    while ((HAL_GetTick() - start) < FW_FINAL_ACK_GRACE_MS)
    {
        firmware_chunk_header_t header;
        uint32_t elapsed = HAL_GetTick() - start;
        if (elapsed >= FW_FINAL_ACK_GRACE_MS)
        {
            break;
        }
        if (!boot_receive_chunk_header_timeout(&header,
                FW_FINAL_ACK_GRACE_MS - elapsed))
        {
            break;
        }
        if (chunk_matches(&header, committed))
        {
            if (!boot_send_chunk_reply(BOOT_REPLY_ACK, committed->sequence))
            {
                return 0;
            }
            if (++replies >= FW_CHUNK_MAX_RETRIES)
            {
                break;
            }
        }
    }
    return 1;
}

int boot_receive_and_write(uint32_t slot, uint32_t total_size)
{
    uint32_t received = 0U;
    uint32_t expected_sequence = 0U;
    uint32_t base = boot_slot_base(slot);
    uint32_t end = boot_slot_end(slot);
    firmware_chunk_header_t committed = {0};
    uint32_t replay_count = 0U;

    if (base == 0U || total_size == 0U || total_size > APP_MAX_SIZE ||
        total_size > (end - base))
    {
        return 0;
    }

    while (received < total_size)
    {
        uint32_t remain = total_size - received;
        uint32_t chunk_size = (remain > FW_CHUNK_SIZE) ? FW_CHUNK_SIZE : remain;
        uint32_t retry_count = 0U;
        uint8_t chunk_success = 0U;

        while (retry_count < FW_CHUNK_MAX_RETRIES)
        {
            firmware_chunk_header_t chunk_header;

            if (!boot_receive_chunk_header(&chunk_header))
            {
                retry_count++;
                if (!boot_send_chunk_reply(BOOT_REPLY_NACK, expected_sequence))
                {
                    return 0;
                }
                continue;
            }

            /* A repeated header for the last committed packet needs only ACK.
               Never receive its data again and never program Flash twice. */
            if (expected_sequence != 0U &&
                chunk_matches(&chunk_header, &committed))
            {
                if (++replay_count > FW_CHUNK_MAX_RETRIES ||
                    !boot_send_chunk_reply(BOOT_REPLY_ACK, committed.sequence))
                {
                    return 0;
                }
                continue;
            }

            if (!boot_chunk_header_is_valid(&chunk_header, expected_sequence) ||
                chunk_header.length != chunk_size)
            {
                retry_count++;
                if (!boot_send_chunk_reply(BOOT_REPLY_NACK, expected_sequence))
                {
                    return 0;
                }
                continue;
            }

            if (!boot_send_chunk_reply(BOOT_REPLY_READY, expected_sequence))
            {
                return 0;
            }

            if (!boot_receive_chunk(buffer, chunk_header.length))
            {
                retry_count++;
                if (!boot_send_chunk_reply(BOOT_REPLY_NACK, expected_sequence))
                {
                    return 0;
                }
                continue;
            }

            if (boot_crc32_buffer(buffer, chunk_header.length) != chunk_header.crc32)
            {
                retry_count++;
                if (!boot_send_chunk_reply(BOOT_REPLY_NACK, expected_sequence))
                {
                    return 0;
                }
                continue;
            }

            if (!boot_flash_write_chunk(slot, base + received, buffer,
                                          chunk_header.length))
            {
                /* A Flash failure may have partially programmed a word.
                   Restart the session with erase instead of rewriting it. */
                boot_send_chunk_reply(BOOT_REPLY_NACK, expected_sequence);
                return 0;
            }

            if (!boot_send_chunk_reply(BOOT_REPLY_ACK, expected_sequence))
            {
                return 0;
            }

            received += chunk_header.length;
            committed = chunk_header;
            expected_sequence++;
            replay_count = 0U;
            chunk_success = 1U;
            break;
        }

        if (!chunk_success)
        {
            return 0;
        }
    }

    return finish_chunk_replies(&committed);
}
