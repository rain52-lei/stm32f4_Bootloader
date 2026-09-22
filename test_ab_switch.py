"""A/B 双分区硬件在环验收脚本。

用法：
    python test_ab_switch.py normal --version 10
    python test_ab_switch.py mismatch --version 11
    python test_ab_switch.py abort --version 12

normal：读取 DOWNLOAD 槽，自动选择对应镜像并正常升级。
mismatch：故意声明相反槽，期望 SLOT MISMATCH，且不擦除 Flash。
abort：进入非活动槽升级后不发分包，等待 WRITE FAIL；随后人工复位，
       期望旧活动槽仍能打印 JUMPING TO APP 并运行。
"""

import argparse
import time
import zlib

from flash_send import (DEFAULT_BIN_A, DEFAULT_BIN_B, FW_VERSION, SLOT_A,
                        SLOT_B, open_serial, send_firmware,
                        send_firmware_header, slot_name, wait_for_any,
                        wait_for_boot_banner)


def path_for_slot(slot, path_a, path_b):
    return path_a if slot == SLOT_A else path_b


def read_firmware(path):
    with open(path, 'rb') as file:
        return file.read()


def test_normal(ser, download_slot, args):
    path = path_for_slot(download_slot, args.bin_a, args.bin_b)
    fw = read_firmware(path)
    print('>>> 正常升级槽 %s：%s' % (slot_name(download_slot), path))
    return send_firmware(ser, fw, args.version, download_slot)


def test_mismatch(ser, download_slot, args):
    wrong_slot = SLOT_B if download_slot == SLOT_A else SLOT_A
    path = path_for_slot(wrong_slot, args.bin_a, args.bin_b)
    fw = read_firmware(path)
    ser.write(b'UPDATE')
    send_firmware_header(ser, len(fw), zlib.crc32(fw) & 0xFFFFFFFF,
                         args.version, wrong_slot)
    reply = wait_for_any(ser, ['SLOT MISMATCH', 'ERASE OK', 'HEADER BAD'], 5)
    if reply == 'SLOT MISMATCH':
        print('=== 测试通过：下位机要求 %s，错误声明 %s 被显式拒绝 ===' %
              (slot_name(download_slot), slot_name(wrong_slot)))
        return True
    print('!! 预期 SLOT MISMATCH，实际：%s' % (reply or '超时'))
    return False


def test_abort(ser, download_slot, args):
    path = path_for_slot(download_slot, args.bin_a, args.bin_b)
    fw = read_firmware(path)
    ser.write(b'UPDATE')
    send_firmware_header(ser, len(fw), zlib.crc32(fw) & 0xFFFFFFFF,
                         args.version, download_slot)
    reply = wait_for_any(ser, ['ERASE OK', 'VERSION FAIL', 'SLOT MISMATCH'], 10)
    if reply != 'ERASE OK':
        print('!! 未进入写入阶段：%s' % (reply or '超时'))
        return False
    print('>>> 已擦除非活动槽 %s，故意不发送任何分包。' % slot_name(download_slot))
    if wait_for_any(ser, ['WRITE FAIL'], 20) != 'WRITE FAIL':
        print('!! 未等到 WRITE FAIL')
        return False
    print('=== 写入会话已失败；请按复位键，验证旧活动槽继续启动 ===')
    reply = wait_for_any(ser, ['JUMPING TO APP A', 'JUMPING TO APP B',
                               'APP INVALID'], 20)
    if reply in ('JUMPING TO APP A', 'JUMPING TO APP B'):
        print('=== 灵魂测试通过：升级失败后旧固件仍可启动（%s） ===' % reply)
        return True
    print('!! 旧固件未恢复启动：%s' % (reply or '超时'))
    return False


def build_parser():
    parser = argparse.ArgumentParser(description='A/B Bootloader HIL 验收')
    parser.add_argument('scenario', choices=['normal', 'mismatch', 'abort'])
    parser.add_argument('--version', type=int, default=FW_VERSION)
    parser.add_argument('--bin-a', default=DEFAULT_BIN_A)
    parser.add_argument('--bin-b', default=DEFAULT_BIN_B)
    parser.add_argument('--port', default=None)
    return parser


def main():
    args = build_parser().parse_args()
    ser = open_serial(args.port) if args.port else open_serial()
    try:
        print('>>> 请按一下板上复位键...')
        download_slot = wait_for_boot_banner(ser)
        if download_slot not in (SLOT_A, SLOT_B):
            print('!! 未解析到 DOWNLOAD 槽')
            return
        print('当前下载槽：%s' % slot_name(download_slot))
        if args.scenario == 'normal':
            test_normal(ser, download_slot, args)
        elif args.scenario == 'mismatch':
            test_mismatch(ser, download_slot, args)
        else:
            test_abort(ser, download_slot, args)
    finally:
        ser.close()


if __name__ == '__main__':
    main()
