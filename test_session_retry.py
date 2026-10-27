"""HIL: abort an upgrade, then upgrade again without resetting the MCU."""
import argparse
import zlib

from flash_send import (DEFAULT_BIN_A, DEFAULT_BIN_B, open_serial, parse_slot,
                        send_firmware, send_firmware_header, validate_firmware,
                        wait_for_any, wait_for_boot_banner)


def main():
    parser = argparse.ArgumentParser(description='升级失败后不复位连续重试')
    parser.add_argument('--slot', type=parse_slot, required=True)
    parser.add_argument('--version', type=int, required=True)
    parser.add_argument('--port', default='COM19')
    parser.add_argument('--bin', dest='path')
    args = parser.parse_args()
    path = args.path or (DEFAULT_BIN_A if args.slot == 0 else DEFAULT_BIN_B)
    with open(path, 'rb') as file:
        fw = file.read()
    validate_firmware(fw, args.version, args.slot)
    ser = open_serial(args.port)
    try:
        print('>>> 请按一次复位；第一轮故意不发分包，之后不要再复位')
        if wait_for_boot_banner(ser) != args.slot:
            raise SystemExit('DOWNLOAD 与所选槽不一致')
        ser.write(b'UPDATE')
        send_firmware_header(ser, len(fw), zlib.crc32(fw) & 0xFFFFFFFF,
                             args.version, args.slot)
        if wait_for_any(ser, ['ERASE OK', 'VERSION FAIL', 'SLOT MISMATCH'], 10) != 'ERASE OK':
            raise SystemExit('第一轮未进入写入阶段')
        if wait_for_any(ser, ['WRITE FAIL'], 25) != 'WRITE FAIL':
            raise SystemExit('未等到故意中断后的 WRITE FAIL')
        if wait_for_boot_banner(ser, 10) != args.slot:
            raise SystemExit('失败后未正确重新宣布 DOWNLOAD 槽')
        print('>>> 未复位，开始第二轮完整升级')
        if not send_firmware(ser, fw, args.version, args.slot):
            raise SystemExit('第二轮重试失败')
        print('=== 不复位连续重试成功；建议再检查 Metadata generation 与旧槽保留 ===')
    finally:
        ser.close()


if __name__ == '__main__':
    main()
