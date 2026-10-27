"""HIL: discard the first ACK for packet 0 and for the final packet.

Run only against the updated Bootloader:
    python test_ack_loss.py --slot B --version 12 --port COM19
This performs a real upgrade. Software discards ACKs to mimic reply loss;
the MCU should ACK repeated headers without programming their data again.
"""
import argparse

from flash_send import (BOOT_REPLY_ACK, BOOT_REPLY_NACK, BOOT_REPLY_READY,
                        CHUNK, DEFAULT_BIN_A, DEFAULT_BIN_B, open_serial,
                        parse_slot, read_exact, send_firmware,
                        validate_firmware, wait_for_boot_banner)


class DropAckLink:
    def __init__(self, link, sequences):
        self.link = link
        self.pending = bytearray()
        self.drop = set(sequences)
        self.dropped = set()

    def write(self, data):
        return self.link.write(data)

    def read(self, size=1):
        if not self.pending:
            first = self.link.read(1)
            if not first:
                return b''
            if first[0] in (BOOT_REPLY_ACK, BOOT_REPLY_NACK, BOOT_REPLY_READY):
                payload = read_exact(self.link, 4, 1.0)
                if payload is None:
                    raise ConnectionError('测试代理收到不完整二进制回复')
                seq = int.from_bytes(payload, 'little')
                if first[0] == BOOT_REPLY_ACK and seq in self.drop:
                    self.drop.remove(seq)
                    self.dropped.add(seq)
                    print('\n>>> 测试注入：丢弃 ACK(%d)，等待上位机重发包头' % seq)
                    return b''
                self.pending.extend(first + payload)
            else:
                self.pending.extend(first)
        result = bytes(self.pending[:size])
        del self.pending[:len(result)]
        return result


def main():
    parser = argparse.ArgumentParser(description='真实串口 ACK 丢失恢复测试')
    parser.add_argument('--slot', type=parse_slot, required=True)
    parser.add_argument('--version', type=int, required=True)
    parser.add_argument('--port', default='COM19')
    parser.add_argument('--bin', dest='path')
    args = parser.parse_args()
    path = args.path or (DEFAULT_BIN_A if args.slot == 0 else DEFAULT_BIN_B)
    with open(path, 'rb') as file:
        fw = file.read()
    validate_firmware(fw, args.version, args.slot)
    last = (len(fw) - 1) // CHUNK
    ser = open_serial(args.port)
    try:
        print('>>> 请按复位；测试会真实写入槽 %s' % ('A' if args.slot == 0 else 'B'))
        if wait_for_boot_banner(ser) != args.slot:
            raise SystemExit('DOWNLOAD 与所选槽不一致')
        link = DropAckLink(ser, (0, last))
        success = send_firmware(link, fw, args.version, args.slot)
        if not success or link.drop:
            raise SystemExit('测试失败：未完整恢复或未实际丢弃目标 ACK')
        print('=== ACK 丢失恢复通过，已丢弃的序号：%s ===' % sorted(link.dropped))
    finally:
        ser.close()


if __name__ == '__main__':
    main()
