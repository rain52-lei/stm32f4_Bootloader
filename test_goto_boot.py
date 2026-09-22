"""GOTOBOOT 触发升级测试：不按复位键，从正在运行的 App 直接进 bootloader 升级。

前置条件：
    - 板子上已烧写带 GOTOBOOT 功能的 App（LED 在闪）；
    - bootloader 也已更新到带备份寄存器标志检查的版本。

流程：
    1. 打开串口，尝试捕获 APP READY（App 起来后只打一次；没等到就盲发）；
    2. 发送 GOTOBOOT → App 写 RTC 备份寄存器标志并复位；
    3. bootloader 看到标志，打印 GOTOBOOT FLAG 行并开 60 秒加长窗口；
    4. 按 flash_send 的正常流程升级指定槽位。

用法：
    python test_goto_boot.py --slot B --version 4
"""

import argparse

from flash_send import (DEFAULT_BIN_A, DEFAULT_BIN_B, open_serial, parse_slot,
                        send_firmware, slot_name, wait_for_any,
                        wait_for_boot_banner)

GOTOBOOT_CMD = b'GOTOBOOT'


def main():
    parser = argparse.ArgumentParser(description='GOTOBOOT 触发升级测试')
    parser.add_argument('--slot', type=parse_slot, required=True,
                        help='镜像链接槽位：A 或 B')
    parser.add_argument('--bin', dest='path', help='固件 bin 路径；省略时按槽选择默认路径')
    parser.add_argument('--version', type=int, default=4, help='固件版本号')
    parser.add_argument('--port', default='COM19', help='串口号，例如 COM18')
    args = parser.parse_args()
    if args.version <= 0:
        raise SystemExit('版本号必须大于 0')

    path = args.path or (DEFAULT_BIN_A if args.slot == 0 else DEFAULT_BIN_B)
    with open(path, 'rb') as file:
        fw = file.read()

    print('固件   :', path)
    print('槽位   :', slot_name(args.slot))
    print('大小   : %d 字节' % len(fw))
    print('版本   : %d' % args.version)

    ser = open_serial(args.port)
    try:
        ready = wait_for_any(ser, ['APP READY'], timeout=3.0)
        if ready:
            print('>>> App 已就绪，发送 GOTOBOOT...')
        else:
            print('>>> 未捕获 APP READY（可能早已在运行），盲发 GOTOBOOT...')

        ser.write(GOTOBOOT_CMD)
        marker = wait_for_any(ser, ['GOTOBOOT FLAG', 'WAIT UPDATE'], timeout=15.0)
        if marker is None:
            print('!! 没等到 bootloader 横幅：确认两侧固件都已更新到 GOTOBOOT 版本')
            return
        print('Bootloader 横幅已出现（%s），开始升级' % marker)

        download_slot = wait_for_boot_banner(ser, timeout=15.0)
        if download_slot is None:
            print('!! 没解析到 DOWNLOAD 槽位')
            return
        if download_slot != args.slot:
            print('!! 下位机要求槽 %s，所选镜像是槽 %s；已停止发送' %
                  (slot_name(download_slot), slot_name(args.slot)))
            return

        send_firmware(ser, fw, args.version, args.slot)
        print('=== GOTOBOOT 触发升级测试完成：全程未按复位键 ===')
    finally:
        ser.close()


if __name__ == '__main__':
    main()
