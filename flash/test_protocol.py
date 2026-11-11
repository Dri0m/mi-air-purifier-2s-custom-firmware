#!/usr/bin/env python3
"""Offline test: drive uartboot_load.Link against a simulated MW300 ROM.

This does not test the SPI stub; it tests the host-side state machine
(Detection ACK -> Header Request -> Data Header -> Data -> Entry -> jump)
and the image/length-word layout.  Run it after changing the loader.
"""
import hashlib
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import uartboot_load as ub  # noqa: E402


def hr(length):
    return bytes((0xA5, length & 0xFF, (length >> 8) & 0xFF,
                  (~length) & 0xFF, ((~length) >> 8) & 0xFF))


class FakeRom:
    """Minimal 88MW300 UART-boot ROM + a stub that prints a known dump."""

    def __init__(self, dump_len=0x2345):
        self.out = bytearray()
        self.state = "detect"
        self.pending = None       # packet length expected after next 0x53
        self.next_len = 16        # what the next Header Request asks for
        self.buf = bytearray()
        self.mem = {}
        self.cur_addr = 0
        self.cur_len = 0
        self.cur_type = 0
        self.entry = None
        self.jumped = False
        self.dump_len = dump_len
        self.dump = bytes((i * 7 + 3) & 0xFF for i in range(dump_len))
        self.error = None

    # transport interface -------------------------------------------------
    def read(self, n, timeout):
        data = bytes(self.out[:n])
        del self.out[:n]
        return data

    def write(self, data):
        for b in data:
            self._byte(b)

    # ROM behaviour -------------------------------------------------------
    def _emit(self, data):
        self.out += data

    def _byte(self, b):
        if self.jumped:
            return
        if self.state == "detect":
            if b == 0x55:
                self.state = "wait_ack"
                self._emit(bytes((0x01, 0x00, 0x41, 0x0A, 0xA3)))
                self._emit(hr(16))
                self.next_len = 16
            return

        if self.state == "jump_wait":
            if b == 0x53:
                self.jumped = True
                self._emit(b"MWDUMPER 1\r\n")
                self._emit(b"JEDEC EF 40 16\r\n")
                self._emit(b"BEGIN %d\r\n" % self.dump_len)
                self._emit(self.dump)
            return

        if self.pending is None:
            if b != 0x53:
                self.error = f"expected 0x53, got 0x{b:02x}"
                return
            self.pending = self.next_len
            self.buf = bytearray()
            if self.pending == 0:
                self.pending = None
                self.state = "jump_wait"
            return

        self.buf.append(b)
        if self.pending and len(self.buf) == self.pending:
            packet = bytes(self.buf)
            self.buf = bytearray()
            self.pending = None
            self._packet(packet)

    def _packet(self, packet):
        if self.next_len == 16:
            ptype = packet[0]
            if ptype in (0x01, 0x02):
                if len(packet) != 16:
                    self.error = f"data header length {len(packet)}"
                    return
                addr = int.from_bytes(packet[4:8], "little")
                length = int.from_bytes(packet[8:12], "little")
                if ptype == 0x01:
                    want = int.from_bytes(packet[12:16], "little")
                    got = ub.crc32_mw(packet[:12])
                    if got != want:
                        self.error = f"data header crc {got:08x} != {want:08x}"
                        return
                self.cur_addr = addr
                self.cur_len = length
                self.cur_type = ptype
                self.next_len = length
                self._emit(hr(length))
                return
            if ptype in (0x04, 0x08):
                if len(packet) != 16:
                    self.error = f"entry header length {len(packet)}"
                    return
                if ptype == 0x04:
                    want = int.from_bytes(packet[12:16], "little")
                    got = ub.crc32_mw(packet[:12])
                    if got != want:
                        self.error = f"entry header crc {got:08x} != {want:08x}"
                        return
                self.entry = int.from_bytes(packet[4:8], "little")
                self.next_len = 0
                self._emit(hr(0))
                self.state = "jump_wait"
                return
            self.error = f"unknown header type 0x{ptype:02x}"
            return

        # data packet
        if len(packet) != self.next_len:
            self.error = f"data packet length {len(packet)} != {self.next_len}"
            return
        if self.cur_type == 0x01:
            want = int.from_bytes(packet[-4:], "little")
            got = ub.crc32_mw(packet[:-4])
            if got != want:
                self.error = f"data crc {got:08x} != {want:08x}"
                return
        payload = packet[:-4]
        for i, byte in enumerate(payload):
            self.mem[self.cur_addr + i] = byte
        self.cur_addr += len(payload)
        self.next_len = 16
        self._emit(hr(16))


def run_test():
    dump_len = 0x2345
    stub = bytes((i * 13 + 1) & 0xFF for i in range(946))
    image = stub + b"\x00" * (0x2000 - len(stub)) + ub.le32(dump_len)
    assert len(image) == 0x2004

    rom = FakeRom(dump_len=dump_len)
    link = ub.Link(rom)

    version, ack = link.detect(2.0)
    assert ack == 0xA3, hex(ack)
    assert version == bytes.fromhex("01 00 41 0a")

    link.load(image)
    assert rom.error is None, rom.error
    assert rom.jumped
    assert rom.entry == ub.ENTRY, hex(rom.entry)

    # The loaded bytes at 0x00100000 must equal the image, including the
    # 4-byte length word at 0x00102000.
    for offset in (0, 1, 500, 0x1000, 0x1FFF, 0x2000, 0x2003):
        got = rom.mem.get(ub.BASE + offset)
        want = image[offset]
        assert got == want, f"mem[0x{ub.BASE + offset:08x}] {got!r} != {want!r}"

    with tempfile.NamedTemporaryFile(delete=False, suffix=".bin") as f:
        out_path = f.name
    try:
        ub.capture(link, out_path, 115200)
        with open(out_path, "rb") as f:
            got = f.read()
        assert got == rom.dump, "captured dump differs"
    finally:
        os.unlink(out_path)

    print("fake-ROM test OK: detect, 0x2004-byte image, entry, and capture")


if __name__ == "__main__":
    ub.self_test()
    run_test()
