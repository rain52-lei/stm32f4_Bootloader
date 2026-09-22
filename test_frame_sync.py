"""测试帧同步：AA 55 同步字的三个验收场景。

三个场景依次占用第 0、1、2 包，之后所有包按正常两阶段握手发送，
整次升级应以 WRITE DONE 收尾（同时证明写入 Flash 的数据未错位）。

场景 1 垃圾前缀（第 0 包）：包头前混入 30 个杂字节（保证不含 AA/55），
    Bootloader 应跳过垃圾、锁定真同步字，正常 READY → ACK。
场景 2 假锁定自愈（第 1 包）：包头前注入"AA 55 + 12 字节假包头（序号=999）"，
    Bootloader 会假锁定 → 校验拒绝（NACK）→ 继续扫描到真同步字 → READY → ACK。
    注意：假锁定产生的 NACK 先于 READY 到达，上位机必须容忍后再继续，
    所以这里不能直接复用 send_packet（它收到第一个非 READY 就放弃了）。
场景 3 半包超时（第 2 包）：包头 → READY → 只发 50/256 字节，
    Bootloader 数据接收超时回 NACK(2)；随后重发完整包 → ACK。

用法：
    python test_frame_sync.py               # 默认固件、默认版本
    python test_frame_sync.py 固件.bin 5    # 指定固件和版本（版本防回滚被拒就加大）
"""

import struct
import sys
import zlib

import serial

from flash_send import (BAUD, BOOT_REPLY_ACK, BOOT_REPLY_NACK,
                        BOOT_REPLY_READY, CHUNK, CHUNK_SYNC, DEFAULT_BIN_A,
                        DEFAULT_BIN_B, FW_VERSION, PORT, SLOT_A,
                        read_chunk_reply, send_chunk_header,
                        send_firmware_header, wait_for, wait_for_any,
                        wait_for_boot_banner)

HALF_CHUNK = 50       # 场景 3 故意只发这么多字节，制造数据接收超时


def send_packet(ser, sequence, chunk):
    """两阶段握手发送一个包：包头 → READY → 数据 → ACK。成功返回 True。"""
    chunk_crc = zlib.crc32(chunk) & 0xFFFFFFFF
    send_chunk_header(ser, sequence, len(chunk), chunk_crc)
    if not read_chunk_reply(ser, BOOT_REPLY_READY, sequence):
        print('\n!! 第 %d 包未收到 READY' % sequence)
        return False
    ser.write(chunk)
    return read_chunk_reply(ser, BOOT_REPLY_ACK, sequence)


def wait_ready_tolerant(ser, sequence, max_replies=3):
    """等待 READY，容忍途中先到达的 NACK（假锁定被拒后 Bootloader 会继续扫描自愈）。"""
    for _ in range(max_replies):
        if read_chunk_reply(ser, BOOT_REPLY_READY, sequence):
            return True
    return False


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

    if size < 3 * CHUNK:
        print('!! 固件太小（%d 字节），三个场景至少需要 %d 个包' % (size, 3))
        ser.close()
        return

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

    # ---- 场景 1（第 0 包）：垃圾前缀 ----
    junk = bytes(range(1, 31))          # 1..30，保证不含 0xAA / 0x55
    print('--- 场景 1：包头前注入 %d 个杂字节 ---' % len(junk))
    ser.write(junk)
    if not send_packet(ser, 0, fw[:CHUNK]):
        print('!! 场景 1 失败：垃圾前缀后未完成第 0 包')
        ser.close()
        return
    print('=== 场景 1 通过：垃圾被跳过，同步字锁定成功 ===')

    # ---- 场景 2（第 1 包）：假锁定自愈 ----
    fake_header = struct.pack('<III', 999, CHUNK, 0x12345678)
    junk2 = CHUNK_SYNC + fake_header + b'\x00' * 8   # 假包头序号必然不匹配
    real_chunk = fw[CHUNK:2 * CHUNK]
    real_crc = zlib.crc32(real_chunk) & 0xFFFFFFFF
    print('--- 场景 2：包头前注入 AA55 + 假包头（序号=999） ---')
    ser.write(junk2)
    send_chunk_header(ser, 1, len(real_chunk), real_crc)
    if not wait_ready_tolerant(ser, 1):
        print('!! 场景 2 失败：假锁定后未自愈到 READY(1)')
        ser.close()
        return
    ser.write(real_chunk)
    if not read_chunk_reply(ser, BOOT_REPLY_ACK, 1):
        print('!! 场景 2 失败：数据未收到 ACK(1)')
        ser.close()
        return
    print('=== 场景 2 通过：假锁定被 NACK 拒绝后自愈，第 1 包正常写入 ===')

    # ---- 场景 3（第 2 包）：半包超时 ----
    chunk2 = fw[2 * CHUNK:3 * CHUNK]
    crc2 = zlib.crc32(chunk2) & 0xFFFFFFFF
    print('--- 场景 3：包头 → READY → 只发 %d/%d 字节 ---' % (HALF_CHUNK, len(chunk2)))
    send_chunk_header(ser, 2, len(chunk2), crc2)
    if not read_chunk_reply(ser, BOOT_REPLY_READY, 2):
        print('!! 场景 3 失败：未收到 READY(2)')
        ser.close()
        return
    ser.write(chunk2[:HALF_CHUNK])      # 制造数据接收超时（约 3 秒后 NACK）
    if read_chunk_reply(ser, BOOT_REPLY_NACK, 2):
        print('=== Bootloader 数据接收超时，如期收到 NACK(2) ===')
    else:
        print('!! 场景 3 失败：半包后未收到 NACK(2)')
        ser.close()
        return
    print('--- 重发完整第 2 包 ---')
    if not send_packet(ser, 2, chunk2):
        print('!! 场景 3 失败：重发后未完成第 2 包')
        ser.close()
        return
    print('=== 场景 3 通过：半包被拦截，重发完整包成功 ===')

    # ---- 其余包正常发送 ----
    sent = 3 * CHUNK
    sequence = 3
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
        print('=== 全部场景通过：WRITE DONE，板子将复位并运行新固件 ===')
    else:
        print('!! 数据发完但没等到 WRITE DONE')

    ser.close()


if __name__ == '__main__':
    main()
