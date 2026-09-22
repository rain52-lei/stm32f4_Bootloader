"""测试单包 CRC：坏包应收到 NACK，同序号重发后应恢复并完成升级。"""

import sys
import zlib

from flash_send import (BOOT_REPLY_ACK, BOOT_REPLY_NACK, BOOT_REPLY_READY,
                        CHUNK, DEFAULT_BIN_A, DEFAULT_BIN_B, FW_VERSION,
                        SLOT_A, open_serial, read_chunk_reply,
                        send_chunk_header, send_firmware_header, send_packet,
                        wait_for, wait_for_any, wait_for_boot_banner)


def main():
    version = int(sys.argv[2]) if len(sys.argv) > 2 else FW_VERSION

    ser = open_serial()
    try:
        print('>>> 请按一下板上复位键...')
        download_slot = wait_for_boot_banner(ser)
        if download_slot is None:
            print('!! 未解析到 DOWNLOAD 槽')
            return
        path = sys.argv[1] if len(sys.argv) > 1 else (
            DEFAULT_BIN_A if download_slot == SLOT_A else DEFAULT_BIN_B)
        with open(path, 'rb') as file:
            fw = file.read()

        size = len(fw)
        crc = zlib.crc32(fw) & 0xFFFFFFFF
        first = fw[:CHUNK]
        bad_chunk_crc = (zlib.crc32(first) & 0xFFFFFFFF) ^ 1
        print('固件: %s（槽 %s，跟随 DOWNLOAD）' %
              (path, 'A' if download_slot == SLOT_A else 'B'))

        ser.write(b'UPDATE')
        send_firmware_header(ser, size, crc, version, download_slot)
        if wait_for_any(ser, ['ERASE OK', 'VERSION FAIL', 'SLOT MISMATCH'], 10) != 'ERASE OK':
            print('!! 未进入写入阶段')
            return

        send_chunk_header(ser, 0, len(first), bad_chunk_crc)
        if not read_chunk_reply(ser, BOOT_REPLY_READY, 0):
            print('!! 错误 CRC 的包头未收到 READY(0)')
            return
        ser.write(first)
        if not read_chunk_reply(ser, BOOT_REPLY_NACK, 0):
            print('!! 坏包未收到 NACK(0)')
            return
        print('=== 坏包在写 Flash 前被 NACK 拦截 ===')

        if not send_packet(ser, 0, first):
            print('!! 第 0 包重发失败')
            return

        sent = len(first)
        sequence = 1
        while sent < size:
            chunk = fw[sent:sent + CHUNK]
            if not send_packet(ser, sequence, chunk):
                print('!! 第 %d 包失败' % sequence)
                return
            sent += len(chunk)
            sequence += 1

        if wait_for(ser, 'WRITE DONE', 8):
            print('=== 测试通过：NACK → 同序号重发 → 完整升级成功 ===')
        else:
            print('!! 未等到 WRITE DONE')
    finally:
        ser.close()


if __name__ == '__main__':
    main()
