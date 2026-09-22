"""测试 NACK 重传：第 0 包第一次故意把分包头里"声明的 CRC"错一位，
Bootloader 应在写 Flash 前回 NACK(0)；随后重发正确 CRC 的同一包，
应收到 ACK(0)，之后整份固件正常发完，升级仍然成功。

预期时间线：
    分包头(CRC 错一位) → READY(0) → 数据 → NACK(0)   ← 第一击，故意失败
    分包头(CRC 正确)   → READY(0) → 数据 → ACK(0)    ← 重发，成功
    第 1..N 包正常两阶段握手 → WRITE DONE → 板子自动复位

用法：
    python test_retry.py               # 默认固件、默认版本
    python test_retry.py 固件.bin 5    # 指定固件和版本

注意：若 Bootloader 因版本防回滚拒绝（VERSION FAIL），请把版本参数
改得不小于板上当前版本。
"""

import struct
import sys
import zlib

import serial

from flash_send import (BAUD, BOOT_REPLY_ACK, BOOT_REPLY_NACK,
                        BOOT_REPLY_READY, CHUNK, DEFAULT_BIN_A, DEFAULT_BIN_B,
                        FW_VERSION, PORT, SLOT_A, read_chunk_reply,
                        send_chunk_header, send_firmware_header, wait_for,
                        wait_for_any, wait_for_boot_banner)


def send_packet(ser, sequence, chunk):
    """两阶段握手发送一个包：包头 → READY → 数据 → ACK。成功返回 True。"""
    chunk_crc = zlib.crc32(chunk) & 0xFFFFFFFF

    send_chunk_header(ser, sequence, len(chunk), chunk_crc)
    if not read_chunk_reply(ser, BOOT_REPLY_READY, sequence):
        print('\n!! 第 %d 包未收到 READY' % sequence)
        return False

    ser.write(chunk)
    return read_chunk_reply(ser, BOOT_REPLY_ACK, sequence)


def main():
    version = int(sys.argv[2]) if len(sys.argv) > 2 else FW_VERSION

    ser = serial.Serial(port=None, baudrate=BAUD, timeout=2)
    ser.dtr = False                     # 探索者板必须关，否则 MCU 被按在复位里
    ser.rts = False
    ser.port = PORT
    ser.open()
    ser.reset_input_buffer()

    print('>>> 请按一下板上复位键...')
    download_slot = wait_for_boot_banner(ser)
    if download_slot is None:
        print('!! 没等到带 DOWNLOAD 槽位的 Bootloader 提示，检查 PORT / 波特率 / DTR-RTS')
        ser.close()
        return
    path = sys.argv[1] if len(sys.argv) > 1 else (
        DEFAULT_BIN_A if download_slot == SLOT_A else DEFAULT_BIN_B)

    with open(path, 'rb') as f:
        fw = f.read()
    size = len(fw)
    crc = zlib.crc32(fw) & 0xFFFFFFFF

    print('固件   : %s（槽 %s，跟随 DOWNLOAD）' %
          (path, 'A' if download_slot == SLOT_A else 'B'))
    print('大小   : %d 字节' % size)
    print('版本   : %d' % version)

    ser.write(b'UPDATE')
    send_firmware_header(ser, size, crc, version, download_slot)
    reply = wait_for_any(ser,
                         ['ERASE OK', 'VERSION FAIL', 'HEADER BAD',
                          'METADATA FAIL', 'ERASE FAIL'],
                         timeout=10)
    if reply != 'ERASE OK':
        print('!! 未进入写入阶段：%s（版本被拒就加版本参数）' % (reply or '超时'))
        ser.close()
        return

    first = fw[:CHUNK]
    good_crc = zlib.crc32(first) & 0xFFFFFFFF

    print('--- 第 0 包第 1 次发送：声明的 CRC 故意翻转最低位 ---')
    send_chunk_header(ser, 0, len(first), good_crc ^ 1)
    if not read_chunk_reply(ser, BOOT_REPLY_READY, 0):
        print('!! 未收到 READY(0)——分包头阶段就被拒了？')
        ser.close()
        return
    ser.write(first)                    # 数据本身是完好的
    if read_chunk_reply(ser, BOOT_REPLY_NACK, 0):
        print('=== 收到 NACK(0)：坏包在写 Flash 前被拦截 ===')
    else:
        print('!! 预期的 NACK(0) 没出现（收到别的或超时）')
        ser.close()
        return

    print('--- 第 0 包重发：CRC 正确 ---')
    if not send_packet(ser, 0, first):
        print('!! 重发后未收到 ACK(0)')
        ser.close()
        return
    print('=== 收到 ACK(0)：重传被接受，进度正常推进 ===')

    sent = len(first)
    sequence = 1
    while sent < size:
        chunk = fw[sent:sent + CHUNK]
        if not send_packet(ser, sequence, chunk):
            print('\n!! 第 %d 包失败，升级中断' % sequence)
            ser.close()
            return
        sent += len(chunk)
        sequence += 1
        print('\r进度  : %d / %d' % (sent, size), end='')
    print()

    if wait_for(ser, 'WRITE DONE'):
        print('=== 测试通过：NACK → 重传 → ACK → 升级成功 ===')
    else:
        print('!! 数据发完但没等到 WRITE DONE')

    ser.close()


if __name__ == '__main__':
    main()
