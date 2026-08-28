"""
串口固件发送上位机 —— 配套本课程 Bootloader（STM32F407 探索者板）

用法：
    python flash_send.py                    # 默认 App，默认版本
    python flash_send.py 3                  # 默认 App，版本 3
    python flash_send.py 别的固件.bin        # 指定固件，默认版本
    python flash_send.py 别的固件.bin 3      # 指定固件，版本 3

流程与 Bootloader 约定一致：
    UPDATE 命令 → 16 字节固件头(小端，含版本号) → ERASE OK
    → [12 字节分包头 + 分包数据 + OK] × N → WRITE DONE
"""

import sys
import time
import struct
import zlib

import serial

# ---------------- 需要按实际情况修改的两项 ----------------
PORT = 'COM18'        # ← 改成你的 CH340 串口号（设备管理器里看）
DEFAULT_BIN = r'G:\Boot\APP\BOOTLoader\MDK-ARM\BOOTLoader\BOOTLoader.bin'
# ---------------------------------------------------------

BAUD = 115200
CHUNK = 256
FW_MAGIC = 0x55AA1234
FW_VERSION = 3
MAX_RETRIES = 3
BOOT_REPLY_ACK = 0x06
BOOT_REPLY_NACK = 0x15
BOOT_REPLY_READY = 0x16


def wait_for(ser, marker, timeout=5.0):
    """等待串口输出中出现指定文本，超时返回 False"""
    buf = b''
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            byte = ser.read(1)
        except serial.SerialException as exc:
            print('\n!! 串口连接在等待期间中断：%s' % exc)
            return False
        if byte:
            buf += byte
            if marker.encode() in buf:
                return True
    return False


def wait_for_any(ser, markers, timeout=5.0):
    """等待任意一条 Bootloader 文本提示，返回命中的文本或 None。"""
    buf = b''
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            byte = ser.read(1)
        except serial.SerialException as exc:
            print('\n!! 串口连接在等待期间中断：%s' % exc)
            return None
        if byte:
            buf += byte
            for marker in markers:
                if marker.encode() in buf:
                    return marker
    return None


def read_chunk_reply(ser, expected_status, expected_sequence, timeout=5.0):
    """读取 5 字节二进制回复：状态字节 + 小端 sequence。"""
    deadline = time.time() + timeout
    reply_statuses = (BOOT_REPLY_ACK, BOOT_REPLY_NACK, BOOT_REPLY_READY)

    while time.time() < deadline:
        try:
            raw_status = ser.read(1)
        except serial.SerialException as exc:
            print('\n!! 串口连接在等待期间中断：%s' % exc)
            return False

        if not raw_status:
            continue

        status = raw_status[0]
        if status not in reply_statuses:
            # 忽略 ERASE OK 等文本提示残留的 CR/LF 字节。
            continue

        payload = ser.read(4)
        if len(payload) != 4:
            return False

        sequence = struct.unpack('<I', payload)[0]
        return status == expected_status and sequence == expected_sequence

    return False


def main():
    args = sys.argv[1:]
    path = DEFAULT_BIN
    version = FW_VERSION

    if len(args) == 1:
        if args[0].isdigit():
            version = int(args[0])
        else:
            path = args[0]
    elif len(args) == 2:
        path = args[0]
        try:
            version = int(args[1])
        except ValueError:
            print('!! 版本号必须是十进制整数')
            return
    elif len(args) > 2:
        print('用法：python flash_send.py [固件.bin] [版本号]')
        return

    if version <= 0:
        print('!! 版本号必须大于 0')
        return

    with open(path, 'rb') as f:
        fw = f.read()
    size = len(fw)
    crc = zlib.crc32(fw) & 0xFFFFFFFF

    print('固件   :', path)
    print('大小   : %d 字节' % size)
    print('CRC32  : %08X' % crc)
    print('版本   : %d' % version)

    ser = serial.Serial(port=None, baudrate=BAUD, timeout=2)
    ser.dtr = False                     # 探索者板必须关，否则 MCU 被按在复位里
    ser.rts = False
    ser.port = PORT
    ser.open()
    ser.reset_input_buffer()

    print('>>> 请按一下板上复位键...')
    if not wait_for(ser, 'WAIT UPDATE'):
        print('!! 没等到 Bootloader 提示，检查 PORT / 波特率 / DTR-RTS')
        ser.close()
        return

    ser.write(b'UPDATE')
    ser.write(struct.pack('<IIII', FW_MAGIC, size, crc, version))
    print('已发送 UPDATE + 16 字节固件头，等待擦除（约 1 秒）...')

    reply = wait_for_any(ser,
                         ['ERASE OK', 'VERSION FAIL', 'HEADER BAD',
                          'METADATA FAIL', 'ERASE FAIL'],
                         timeout=10)
    if reply != 'ERASE OK':
        print('!! Bootloader 未进入写入阶段：%s' % (reply or '超时'))
        ser.close()
        return

    sent = 0
    sequence = 0
    while sent < size:
        chunk = fw[sent:sent + CHUNK]
        chunk_crc = zlib.crc32(chunk) & 0xFFFFFFFF

        success = False
        for attempt in range(1, MAX_RETRIES + 1):
            # 每包：序号、数据长度、本包 CRC32，全部为小端 uint32_t。
            ser.write(struct.pack('<III', sequence, len(chunk), chunk_crc))

            if not read_chunk_reply(ser, BOOT_REPLY_READY, sequence):
                print('\n!! 第 %d 包未收到 READY（第 %d/%d 次）' %
                      (sequence, attempt, MAX_RETRIES))
                continue

            ser.write(chunk)
            if read_chunk_reply(ser, BOOT_REPLY_ACK, sequence):
                success = True
                break

            print('\n!! 第 %d 包未收到 ACK，将重试' % sequence)

        if not success:
            print('\n!! 第 %d 包重试 %d 次仍失败，升级中断' %
                  (sequence, MAX_RETRIES))
            ser.close()
            return

        sent += len(chunk)
        sequence += 1
        print('\r进度  : %d / %d' % (sent, size), end='')
    print()

    if wait_for(ser, 'WRITE DONE'):
        print('=== 升级完成，板子将自动复位并启动新固件 ===')
    else:
        print('!! 数据发完但没等到 WRITE DONE')

    ser.close()


if __name__ == '__main__':
    main()
