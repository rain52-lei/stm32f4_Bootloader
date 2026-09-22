"""ESP8266 WiFi 透传远程升级服务器（路线 A：bootloader 零改动）。

用法：
    python wifi_upgrade_server.py --slot B --version 5

前提：
    1. ESP8266 已用 AT 指令配好并保存透传链路（连你的 WiFi、TCP 指向
       本机 IP 和 LISTEN_PORT），上电自动连回本服务器；
    2. 板上跑着带 GOTOBOOT 的 App，或 bootloader 正处于升级等待窗口；
    3. Windows 防火墙放行本端口（首次运行会弹窗，勾"允许"）。

流程：等模块连上 → 轮询发 GOTOBOOT 直到 bootloader 横幅出现 →
解析 DOWNLOAD 槽 → 复用 flash_send 的停等协议发固件 → 等 APP READY 确认。
全程不接 USB 线、不按复位键。
"""

import argparse
import re
import socket
import time

from flash_send import (DEFAULT_BIN_A, DEFAULT_BIN_B, parse_slot, send_firmware,
                        slot_name, wait_for_any)

LISTEN_PORT = 9000
GOTOBOOT_CMD = b'GOTOBOOT'


def local_ip():
    """取本机局域网 IP（不真正发包），给 AT+SAVETRANSMISSIONLINK 用。"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(('8.8.8.8', 80))
        return s.getsockname()[0]
    except OSError:
        return socket.gethostbyname(socket.gethostname())
    finally:
        s.close()


class TcpLink:
    """把 TCP 连接包装成 pyserial 风格的 read()/write()，
    flash_send 里的 wait_for_any / read_exact / send_firmware 原样复用。"""

    def __init__(self, conn):
        self._conn = conn
        self._conn.settimeout(0.05)
        self._buf = bytearray()

    def read(self, size=1):
        deadline = time.time() + 0.2
        while len(self._buf) < size and time.time() < deadline:
            try:
                data = self._conn.recv(4096)
            except socket.timeout:
                continue
            if not data:
                raise ConnectionError('ESP8266 断开了 TCP 连接')
            self._buf.extend(data)
        out = bytes(self._buf[:size])
        del self._buf[:size]
        return out

    def write(self, data):
        self._conn.sendall(data)


def wait_download_slot(link, timeout=15.0):
    """从字符流里扫 DOWNLOAD=A/B。不用 flash_send.wait_for_boot_banner：
    它按整行解析，而轮询 GOTOBOOT 阶段可能已把横幅行吃掉一半。"""
    buf = b''
    deadline = time.time() + timeout
    while time.time() < deadline:
        buf += link.read(1)
        buf = buf[-128:]
        match = re.search(rb'DOWNLOAD=([AB])', buf)
        if match:
            return 0 if match.group(1) == b'A' else 1
    return None


def main():
    parser = argparse.ArgumentParser(description='ESP8266 WiFi 远程升级服务器')
    parser.add_argument('--slot', type=parse_slot, required=True,
                        help='镜像链接槽位：A 或 B')
    parser.add_argument('--bin', dest='path', help='固件 bin 路径；省略时按槽选择默认路径')
    parser.add_argument('--version', type=int, default=5, help='固件版本号')
    parser.add_argument('--port', type=int, default=LISTEN_PORT,
                        help='TCP 监听端口（默认 %d，需与 AT 配置一致）' % LISTEN_PORT)
    args = parser.parse_args()
    if args.version <= 0:
        raise SystemExit('版本号必须大于 0')

    path = args.path or (DEFAULT_BIN_A if args.slot == 0 else DEFAULT_BIN_B)
    with open(path, 'rb') as file:
        fw = file.read()

    print('固件   :', path)
    print('槽位   : %s    大小: %d 字节    版本: %d' %
          (slot_name(args.slot), len(fw), args.version))
    print('本机 IP: %s （AT+CIPSTART 应指向它，端口 %d）' % (local_ip(), args.port))

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('0.0.0.0', args.port))
    srv.listen(1)
    print('>>> 等待 ESP8266 连入（给板子上电或复位模块）...')
    conn, addr = srv.accept()
    srv.close()
    print('>>> 模块已连接：%s:%d' % addr)

    link = TcpLink(conn)
    try:
        # 轮询 GOTOBOOT：App 在跑就重启进 boot；boot 在等窗口则被忽略。
        # 覆盖"App 早已运行/横幅已错过"等各种时序，最坏等到 boot 跳 App 后生效
        deadline = time.time() + 40.0
        while time.time() < deadline:
            link.write(GOTOBOOT_CMD)
            if wait_for_any(link, ['GOTOBOOT FLAG', 'WAIT UPDATE'], 3.0):
                break
        else:
            print('!! 40 秒内没等到 bootloader 横幅：检查模块透传方向/波特率')
            return

        download_slot = wait_download_slot(link)
        if download_slot is None:
            print('!! 没解析到 DOWNLOAD 槽位')
            return
        if download_slot != args.slot:
            print('!! 下位机要求槽 %s，所选镜像是槽 %s；已停止发送' %
                  (slot_name(download_slot), slot_name(args.slot)))
            return

        if not send_firmware(link, fw, args.version, args.slot):
            return

        # 板子自动复位后模块的 TCP 仍在，等新 App 报到即闭环
        if wait_for_any(link, ['APP READY'], timeout=20.0):
            print('=== WiFi 远程升级成功：新固件已运行，全程未接 USB、未按复位 ===')
        else:
            print('=== 固件已写入，但 20 秒内没等到 APP READY；可复位后观察 ===')
    except (ConnectionError, OSError) as exc:
        print('!! TCP 连接异常：%s' % exc)
    finally:
        conn.close()


if __name__ == '__main__':
    main()
