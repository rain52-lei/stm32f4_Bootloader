"""测试分包序号：第 0 个包故意发送 sequence=1。

预期：Bootloader 期待 sequence=0，因此在写 Flash 前拒绝并输出 WRITE FAIL。
测试后请运行 flash_send.py 正常升级恢复 App。
"""

import struct
import sys
import zlib

import serial

from flash_send import (BAUD, CHUNK, DEFAULT_BIN, FW_MAGIC, FW_VERSION, PORT,
                        send_chunk_header, wait_for)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BIN
    with open(path, "rb") as f:
        fw = f.read()

    size = len(fw)
    crc = zlib.crc32(fw) & 0xFFFFFFFF
    chunk = fw[:CHUNK]
    chunk_crc = zlib.crc32(chunk) & 0xFFFFFFFF

    print("固件大小 : %d 字节" % size)
    print("版本     : %d" % FW_VERSION)
    print("第 0 包应有序号: 0")
    print("故意发送序号  : 1")

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
    ser.write(struct.pack("<IIII", FW_MAGIC, size, crc, FW_VERSION))
    if not wait_for(ser, "ERASE OK", timeout=10):
        print("!! 没等到 ERASE OK")
        ser.close()
        return

    # 数据、长度、CRC 均正确，只有 sequence 从 0 故意改成 1。
    send_chunk_header(ser, 1, len(chunk), chunk_crc)
    ser.write(chunk)
    print(">>> 已发送第一个包，但 sequence=1（应为 0）。")

    if wait_for(ser, "WRITE FAIL", timeout=5):
        print("=== 测试通过：错误序号已在写 Flash 前被拦截 ===")
    else:
        print("!! 未等到 WRITE FAIL，请查看串口输出")

    ser.close()


if __name__ == "__main__":
    main()
