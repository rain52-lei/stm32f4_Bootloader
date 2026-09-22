"""STM32F407 A/B Bootloader 串口升级上位机。

用法：
    python flash_send.py --slot A --version 3
    python flash_send.py --slot B --bin G:\\path\\app_b.bin --version 4

协议：UPDATE + 20 字节固件头（magic/size/crc/version/image_slot），
随后按 AA55 + 12 字节分包头 + 数据的停等式 READY/ACK/NACK 流程发送。
"""

import argparse
import re
import struct
import time
import zlib

import serial

PORT = 'COM19'
DEFAULT_BIN_A = r'G:\Boot\APP\BOOTLoader\MDK-ARM\APP_A\app_a.bin'
DEFAULT_BIN_B = r'G:\Boot\APP\BOOTLoader\MDK-ARM\APP_B\app_b.bin'
# 兼容旧测试脚本；新代码应按槽使用 DEFAULT_BIN_A/DEFAULT_BIN_B。
DEFAULT_BIN = DEFAULT_BIN_A

BAUD = 115200
CHUNK = 256
FW_MAGIC = 0x55AA1234
FW_VERSION = 3
MAX_RETRIES = 3
SLOT_A = 0
SLOT_B = 1
BOOT_REPLY_ACK = 0x06
BOOT_REPLY_NACK = 0x15
BOOT_REPLY_READY = 0x16
CHUNK_SYNC = b'\xAA\x55'


def slot_name(slot):
    return 'A' if slot == SLOT_A else 'B'


def parse_slot(value):
    value = str(value).strip().upper()
    if value == 'A':
        return SLOT_A
    if value == 'B':
        return SLOT_B
    raise argparse.ArgumentTypeError('槽位必须是 A 或 B')


def wait_for(ser, marker, timeout=5.0):
    """等待串口输出中出现指定文本，超时返回 False。"""
    return wait_for_any(ser, [marker], timeout) == marker


def wait_for_any(ser, markers, timeout=5.0):
    """等待任意一条 Bootloader 文本提示，返回命中的文本或 None。"""
    buf = b''
    encoded = [(marker, marker.encode()) for marker in markers]
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            data = ser.read(1)
        except serial.SerialException as exc:
            print('\n!! 串口连接在等待期间中断：%s' % exc)
            return None
        if not data:
            continue
        buf += data
        if len(buf) > 512:
            buf = buf[-512:]
        for marker, marker_bytes in encoded:
            if marker_bytes in buf:
                return marker
    return None


def wait_for_boot_banner(ser, timeout=15.0):
    """读取启动 banner，返回下位机声明的 DOWNLOAD 槽；无法解析返回 None。"""
    buf = b''
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            data = ser.read(1)
        except serial.SerialException as exc:
            print('\n!! 串口连接在等待期间中断：%s' % exc)
            return None
        if not data:
            continue
        buf += data
        if b'\n' not in buf:
            continue
        lines = buf.split(b'\n')
        buf = lines[-1]
        for raw_line in lines[:-1]:
            line = raw_line.decode(errors='ignore').strip()
            if 'WAIT UPDATE' not in line:
                continue
            print('Bootloader:', line)
            match = re.search(r'DOWNLOAD=([AB])', line)
            if match:
                return parse_slot(match.group(1))
            return None
    return None


def read_exact(ser, length, timeout=5.0):
    """在总超时内读满指定长度，避免短读导致二进制回复失步。"""
    data = bytearray()
    deadline = time.time() + timeout
    while len(data) < length and time.time() < deadline:
        try:
            part = ser.read(length - len(data))
        except serial.SerialException as exc:
            print('\n!! 串口连接在读取期间中断：%s' % exc)
            return None
        if part:
            data.extend(part)
    return bytes(data) if len(data) == length else None


def read_chunk_reply_value(ser, timeout=5.0):
    """读取一条 5 字节二进制回复，返回 (status, sequence) 或 None。"""
    reply_statuses = (BOOT_REPLY_ACK, BOOT_REPLY_NACK, BOOT_REPLY_READY)
    deadline = time.time() + timeout
    while time.time() < deadline:
        raw_status = read_exact(ser, 1, max(0.01, deadline - time.time()))
        if raw_status is None:
            return None
        status = raw_status[0]
        if status not in reply_statuses:
            continue
        payload = read_exact(ser, 4, max(0.01, deadline - time.time()))
        if payload is None:
            return None
        return status, struct.unpack('<I', payload)[0]
    return None


def read_chunk_reply(ser, expected_status, expected_sequence, timeout=5.0):
    reply = read_chunk_reply_value(ser, timeout)
    return reply == (expected_status, expected_sequence)


def send_firmware_header(ser, size, crc32_val, version, image_slot):
    """固件头唯一出口：5 个 uint32_t，小端，共 20 字节。"""
    ser.write(struct.pack('<IIIII', FW_MAGIC, size, crc32_val,
                          version, image_slot))


def send_chunk_header(ser, sequence, length, crc32_val):
    """分包头唯一出口：AA55 + 3 个 uint32_t，小端。"""
    ser.write(CHUNK_SYNC + struct.pack('<III', sequence, length, crc32_val))


def send_packet(ser, sequence, chunk, max_retries=MAX_RETRIES):
    """发送一包并执行 READY/ACK 重试，成功返回 True。"""
    chunk_crc = zlib.crc32(chunk) & 0xFFFFFFFF
    for attempt in range(1, max_retries + 1):
        send_chunk_header(ser, sequence, len(chunk), chunk_crc)
        if not read_chunk_reply(ser, BOOT_REPLY_READY, sequence):
            print('\n!! 第 %d 包未收到 READY（第 %d/%d 次）' %
                  (sequence, attempt, max_retries))
            continue
        ser.write(chunk)
        if read_chunk_reply(ser, BOOT_REPLY_ACK, sequence):
            return True
        print('\n!! 第 %d 包未收到 ACK，将重试' % sequence)
    return False


def open_serial(port=PORT):
    ser = serial.Serial(port=None, baudrate=BAUD, timeout=0.2)
    ser.dtr = False
    ser.rts = False
    ser.port = port
    ser.open()
    ser.reset_input_buffer()
    return ser


def send_firmware(ser, fw, version, image_slot):
    size = len(fw)
    crc = zlib.crc32(fw) & 0xFFFFFFFF

    ser.write(b'UPDATE')
    send_firmware_header(ser, size, crc, version, image_slot)
    reply = wait_for_any(ser, ['ERASE OK', 'VERSION FAIL', 'HEADER BAD',
                               'SLOT MISMATCH', 'METADATA FAIL', 'ERASE FAIL'],
                         timeout=10)
    if reply != 'ERASE OK':
        print('!! Bootloader 未进入写入阶段：%s' % (reply or '超时'))
        return False

    sent = 0
    sequence = 0
    while sent < size:
        chunk = fw[sent:sent + CHUNK]
        if not send_packet(ser, sequence, chunk):
            print('\n!! 第 %d 包重试 %d 次仍失败，升级中断' %
                  (sequence, MAX_RETRIES))
            return False
        sent += len(chunk)
        sequence += 1
        print('\r进度  : %d / %d' % (sent, size), end='')
    print()

    if wait_for(ser, 'WRITE DONE', timeout=8):
        print('=== 升级完成，板子将自动复位并启动新固件 ===')
        return True
    print('!! 数据发完但没等到 WRITE DONE')
    return False


def build_parser():
    parser = argparse.ArgumentParser(description='STM32F407 A/B Bootloader 串口升级')
    parser.add_argument('--slot', type=parse_slot, required=True,
                        help='镜像链接槽位：A 或 B')
    parser.add_argument('--bin', dest='path', help='固件 bin 路径；省略时按槽选择默认路径')
    parser.add_argument('--version', type=int, default=FW_VERSION, help='固件版本号')
    parser.add_argument('--port', default=PORT, help='串口号，例如 COM18')
    return parser


def main():
    args = build_parser().parse_args()
    if args.version <= 0:
        raise SystemExit('版本号必须大于 0')

    path = args.path or (DEFAULT_BIN_A if args.slot == SLOT_A else DEFAULT_BIN_B)
    with open(path, 'rb') as file:
        fw = file.read()

    print('固件   :', path)
    print('槽位   :', slot_name(args.slot))
    print('大小   : %d 字节' % len(fw))
    print('CRC32  : %08X' % (zlib.crc32(fw) & 0xFFFFFFFF))
    print('版本   : %d' % args.version)

    ser = open_serial(args.port)
    try:
        print('>>> 请按一下板上复位键...')
        download_slot = wait_for_boot_banner(ser)
        if download_slot is None:
            print('!! 没等到带 DOWNLOAD 槽位的 Bootloader 提示')
            return
        if download_slot != args.slot:
            print('!! 下位机当前要求槽 %s，但所选镜像是槽 %s；已停止发送' %
                  (slot_name(download_slot), slot_name(args.slot)))
            return
        send_firmware(ser, fw, args.version, args.slot)
    finally:
        ser.close()


if __name__ == '__main__':
    main()
