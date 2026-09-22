"""测试升级中断保护：只发送 UPDATE 和固件头，不发送固件数据。

预期：Bootloader 擦除非活动槽后等待数据超时，Metadata 保持 UPDATING。
随后手动按复位键，旧活动槽应照常启动（JUMPING TO APP A/B）——
这正是 A/B 方案的核心保证。测试结束后跑一次 normal 升级即可恢复。
"""

import struct
import sys
import time
import zlib

import serial

from flash_send import (BAUD, DEFAULT_BIN_A, DEFAULT_BIN_B, FW_VERSION, PORT,
                        SLOT_A, send_firmware_header, wait_for,
                        wait_for_boot_banner)


def main():
    ser = serial.Serial(port=None, baudrate=BAUD, timeout=2)
    ser.dtr = False
    ser.rts = False
    ser.port = PORT
    ser.open()
    ser.reset_input_buffer()

    print(">>> 请按一下板上复位键...")
    download_slot = wait_for_boot_banner(ser, timeout=15)
    if download_slot is None:
        print("!! 没等到带 DOWNLOAD 槽位的 Bootloader 提示")
        ser.close()
        return
    path = sys.argv[1] if len(sys.argv) > 1 else (
        DEFAULT_BIN_A if download_slot == SLOT_A else DEFAULT_BIN_B)
    with open(path, "rb") as f:
        fw = f.read()

    size = len(fw)
    crc = zlib.crc32(fw) & 0xFFFFFFFF
    print("固件: %s（槽 %s，跟随 DOWNLOAD）" %
          (path, "A" if download_slot == SLOT_A else "B"))
    print("固件大小: %d 字节" % size)
    print("真实 CRC: %08X" % crc)

    ser.write(b"UPDATE")
    send_firmware_header(ser, size, crc, FW_VERSION, download_slot)
    print(">>> 已发送 UPDATE + 20 字节固件头；故意不发送固件数据。")

    if not wait_for(ser, "ERASE OK", timeout=20):
        print("!! 20 秒内没等到 ERASE OK。请在 Keil Memory 查看 0x08010000：")
        print("   若第 4 个字为 7FFFFFFF，说明 UPDATING 已写入，按复位即可测试保护。")
        print("   若全是 FF，则需要检查新 Bootloader 是否已 Rebuild + Download。")
        ser.close()
        return

    print(">>> 已确认 Metadata 为 UPDATING，App 区已开始升级。")
    print(">>> 等待 Bootloader 的接收超时提示（约 15 秒，3 轮扫描超时）...")
    if wait_for(ser, "WRITE FAIL", timeout=25):
        print("=== 测试完成：请按复位键，预期旧活动槽照常启动（JUMPING TO APP）===")
    else:
        print("!! 未等到 WRITE FAIL；仍可按复位键验证旧活动槽启动")

    ser.close()


if __name__ == "__main__":
    main()
