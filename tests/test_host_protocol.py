"""No hardware writes: deterministic host-side protocol regression tests."""
import contextlib
import io
import socket
import struct
import unittest
from unittest.mock import patch
import zlib

import flash_send as host
from wifi_upgrade_server import TcpLink


class Clock:
    def __init__(self):
        self.value = 0.0

    def now(self):
        self.value += 0.001
        return self.value


class PacketPeer:
    def __init__(self, clock, drop_ack=(), nack_once=False):
        self.clock = clock
        self.rx = bytearray()
        self.expected = 0
        self.pending = None
        self.committed = {}
        self.drop_ack = set(drop_ack)
        self.nack_once = nack_once
        self.data_writes = 0
        self.header_writes = 0

    def reply(self, status, sequence):
        self.rx.extend(struct.pack('<BI', status, sequence))

    def write(self, data):
        if len(data) == 14 and data.startswith(host.CHUNK_SYNC):
            self.header_writes += 1
            seq, length, crc = struct.unpack('<III', data[2:])
            if self.committed.get(seq) == (length, crc):
                self.reply(host.BOOT_REPLY_ACK, seq)
            elif seq == self.expected:
                self.pending = (seq, length, crc)
                self.reply(host.BOOT_REPLY_READY, seq)
            else:
                self.reply(host.BOOT_REPLY_NACK, self.expected)
        else:
            self.data_writes += 1
            seq, length, crc = self.pending
            self.pending = None
            if self.nack_once:
                self.nack_once = False
                self.reply(host.BOOT_REPLY_NACK, seq)
                return len(data)
            assert len(data) == length
            assert zlib.crc32(data) & 0xFFFFFFFF == crc
            self.committed[seq] = (length, crc)
            self.expected += 1
            if seq in self.drop_ack:
                self.drop_ack.remove(seq)
            else:
                self.reply(host.BOOT_REPLY_ACK, seq)
        return len(data)

    def read(self, size=1):
        if not self.rx:
            self.clock.value += 0.1
            return b''
        # Deliberately short-read every response byte.
        result = bytes(self.rx[:1])
        del self.rx[:1]
        return result


class BytesLink:
    def __init__(self, data):
        self.rx = bytearray(data)

    def read(self, size=1):
        result = bytes(self.rx[:min(size, 1)])
        del self.rx[:len(result)]
        return result


class HostProtocolTests(unittest.TestCase):
    def packet_run(self, drops=(), nack=False, count=1):
        clock = Clock()
        peer = PacketPeer(clock, drops, nack)
        with patch('flash_send.time.monotonic', clock.now), \
                contextlib.redirect_stdout(io.StringIO()):
            for sequence in range(count):
                self.assertTrue(host.send_packet(peer, sequence,
                    bytes([sequence + 3]) * 32))
        return peer

    def test_normal_packet(self):
        peer = self.packet_run()
        self.assertEqual((peer.header_writes, peer.data_writes), (1, 1))

    def test_lost_ack_repeats_only_header(self):
        peer = self.packet_run(drops=(0,))
        self.assertEqual((peer.header_writes, peer.data_writes), (2, 1))

    def test_lost_intermediate_and_final_ack(self):
        peer = self.packet_run(drops=(0, 1), count=2)
        self.assertEqual((peer.header_writes, peer.data_writes), (4, 2))

    def test_nack_does_resend_data(self):
        peer = self.packet_run(nack=True)
        self.assertEqual((peer.header_writes, peer.data_writes), (2, 2))

    def test_old_reply_does_not_advance_new_packet(self):
        clock = Clock()
        peer = PacketPeer(clock)
        peer.reply(host.BOOT_REPLY_ACK, 999)
        with patch('flash_send.time.monotonic', clock.now):
            self.assertTrue(host.send_packet(peer, 0, b'firmware'))
        self.assertEqual(peer.data_writes, 1)

    def test_retry_exhaustion(self):
        clock = Clock()
        peer = PacketPeer(clock)
        with patch('flash_send.time.monotonic', clock.now), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertFalse(host.send_packet(peer, 7, b'x'))
        self.assertEqual(peer.header_writes, host.MAX_RETRIES)
        self.assertEqual(peer.data_writes, 0)

    def test_five_byte_reply_can_be_short_read(self):
        link = BytesLink(b'\r\n' + struct.pack('<BI', host.BOOT_REPLY_READY, 12))
        self.assertEqual(host.read_chunk_reply_value(link),
                         (host.BOOT_REPLY_READY, 12))

    def test_banner_is_read_once_after_gotoboot(self):
        link = BytesLink(b'GOTOBOOT FLAG: EXTENDED UPDATE WINDOW\r\n'
                         b'WAIT UPDATE ACTIVE=A DOWNLOAD=B\r\n')
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(host.wait_for_boot_banner(link), host.SLOT_B)

    def test_firmware_slot_and_ranges(self):
        fw = struct.pack('<II', 0x20020000, 0x08020009) + b'valid body'
        host.validate_firmware(fw, 1, host.SLOT_A)
        for version, slot, data in [(0, 0, fw), (2**32, 0, fw),
                (1, 2, fw), (1, 1, fw), (1, 0, b''),
                (1, 0, b'x' * (host.APP_MAX_SIZE + 1))]:
            with self.assertRaises(ValueError):
                host.validate_firmware(data, version, slot)

    def test_tcp_adapter_keeps_excess_bytes(self):
        left, right = socket.socketpair()
        try:
            link = TcpLink(left)
            right.sendall(b'abcdef')
            self.assertEqual(link.read(2), b'ab')
            self.assertEqual(link.read(4), b'cdef')
            self.assertEqual(link.write(b'OK'), 2)
            self.assertEqual(right.recv(2), b'OK')
        finally:
            left.close()
            right.close()

    def test_tcp_disconnect_is_not_silent_timeout(self):
        left, right = socket.socketpair()
        try:
            link = TcpLink(left)
            right.close()
            with self.assertRaises(ConnectionError):
                link.read(1)
        finally:
            left.close()
            right.close()


if __name__ == '__main__':
    unittest.main()
