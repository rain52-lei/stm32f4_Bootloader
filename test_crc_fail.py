"""测试 CRC32 保护：发送正确固件数据，但故意发送错误的头 CRC32。

预期：所有数据发送完后，Bootloader 返回 CRC FAIL，且不会复位进入 App。
测试结束后运行 flash_send.py 进行一次正常升级即可恢复。
"""

import struct
import sys
import time
import zlib

import serial

from flash_send import (BAUD, BOOT_REPLY_ACK, BOOT_REPLY_READY, CHUNK,
                        DEFAULT_BIN, FW_MAGIC, FW_VERSION, PORT,
                        read_chunk_reply, send_chunk_header, wait_for)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BIN
    with open(path, "rb") as f:
        fw = f.read()

    size = len(fw)
    real_crc = zlib.crc32(fw) & 0xFFFFFFFF
    bad_crc = real_crc ^ 0x00000001
    print("固件大小: %d 字节" % size)
    print("真实 CRC: %08X" % real_crc)
    print("错误 CRC: %08X" % bad_crc)

    ser = serial.Serial(port=None, baudrate=BAUD, timeout=2)
    ser.dtr = False
    ser.rts = False
    ser.port = PORT
    ser.open()
    ser.reset_input_buffer()

    print(">>> 请按一下板上复位键...")
    if not wait_for(ser, "WAIT UPDATE", timeout=15):
        print("!! 没等到 Bootloader 提示")
        ser.close()
        return

    ser.write(b"UPDATE")
    ser.write(struct.pack("<IIII", FW_MAGIC, size, bad_crc, FW_VERSION))
    if not wait_for(ser, "ERASE OK", timeout=10):
        print("!! 没等到 ERASE OK")
        ser.close()
        return

    sent = 0
    sequence = 0
    while sent < size:
        chunk = fw[sent:sent + CHUNK]
        send_chunk_header(ser, sequence, len(chunk),
                          zlib.crc32(chunk) & 0xFFFFFFFF)
        if not read_chunk_reply(ser, BOOT_REPLY_READY, sequence):
            print("!! 第 %d 包未收到 READY" % sequence)
            ser.close()
            return
        ser.write(chunk)
        if not read_chunk_reply(ser, BOOT_REPLY_ACK, sequence):
            print("!! 第 %d 包未收到 ACK" % sequence)
            ser.close()
            return
        sent += len(chunk)
        sequence += 1
        print("\r进度: %d / %d" % (sent, size), end="")
    print()

    if wait_for(ser, "CRC FAIL", timeout=5):
        print("=== 测试通过：CRC 错误已被 Bootloader 拦住 ===")
    else:
        print("!! 没等到 CRC FAIL，请检查串口输出")

    ser.close()


if __name__ == "__main__":
    main()
