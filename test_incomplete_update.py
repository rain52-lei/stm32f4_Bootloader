"""测试升级中断保护：只发送 UPDATE 和固件头，不发送固件数据。

预期：Bootloader 擦除 App 后，等待数据超时，Metadata 保持 UPDATING。
随后手动按复位键，应看到 APP INVALID: STAY IN BOOTLOADER。
测试结束后运行 flash_send.py 进行一次正常升级即可恢复。
"""

import struct
import sys
import time
import zlib

import serial

from flash_send import BAUD, DEFAULT_BIN, FW_MAGIC, FW_VERSION, PORT, wait_for


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BIN
    with open(path, "rb") as f:
        fw = f.read()

    size = len(fw)
    crc = zlib.crc32(fw) & 0xFFFFFFFF
    print("固件大小: %d 字节" % size)
    print("真实 CRC: %08X" % crc)

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
    print(">>> 已发送 UPDATE + 16 字节固件头；故意不发送固件数据。")

    if not wait_for(ser, "ERASE OK", timeout=20):
        print("!! 20 秒内没等到 ERASE OK。请在 Keil Memory 查看 0x08010000：")
        print("   若第 4 个字为 7FFFFFFF，说明 UPDATING 已写入，按复位即可测试保护。")
        print("   若全是 FF，则需要检查新 Bootloader 是否已 Rebuild + Download。")
        ser.close()
        return

    print(">>> 已确认 Metadata 为 UPDATING，App 区已开始升级。")
    print(">>> 等待 Bootloader 的接收超时提示...")
    if wait_for(ser, "WRITE FAIL", timeout=6):
        print("=== 测试完成：请按复位键，预期看到 APP INVALID ===")
    else:
        print("!! 未等到 WRITE FAIL；仍可按复位键检查 APP INVALID")

    ser.close()


if __name__ == "__main__":
    main()
