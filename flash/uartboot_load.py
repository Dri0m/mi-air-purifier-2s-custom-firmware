#!/usr/bin/env python3
"""Load and run a small program on an 88MW300 through its UART boot ROM.

The boot ROM has no read-memory command, so this loader sends it a tiny
RAM program (mw300_stub_110000.bin) that bit-bangs the QSPI flash and streams the
contents back over the same UART.  See the datasheet section 4.8 for the
protocol; section 4.12.3 has a worked example that the self-test mirrors.

Safety:
  * The only command bytes this tool can send are 0x55 (detection) and
    0x53 (header-request ACK).
  * 0x51 (erase entire flash) and 0x5E (security/password) are never
    constructed.  Data packets are arbitrary bytes by design.
  * It writes only to SRAM (0x00110000) and jumps there.  It does not
    touch flash, OTP or the boot straps.

Usage:
  uartboot_load.py --port PORT [--baud 115200]
                   [--stub mw300_stub_110000.bin] [--size auto|2M|0x200000]
                   [--out FILE] [--detect-timeout 180]

The tool starts by sending 0x55 across a power cycle, so start it first
and then switch the purifier's 24 V off and on.  More reliable: set
UARTBOOT_GATE=<file>; the tool then holds TX low (so the adapter cannot
back-power the chip) until that file exists, and you switch the power on
right after creating it.
"""
import argparse
import glob
import hashlib
import os
import struct
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is required: python3 -m pip install pyserial")

CHUNK = 500  # data bytes per packet; +4 CRC = 504, under the ROM's 508 max
BASE = 0x00100000
LEN_ADDR = 0x00102000
ENTRY = BASE | 1

HERE = os.path.dirname(os.path.abspath(__file__))


# --------------------------------------------------------------------------
# CRC and packet construction (verified against datasheet 4.12.3)
# --------------------------------------------------------------------------
def crc32_mw(data: bytes) -> int:
    """CRC-32/IEEE, reflected poly 0xEDB88320, init 0, no final xor.

    This is the variant the MW300 ROM uses for CRC mode 3.  It is NOT
    zlib.crc32 (that one inverts both ends)."""
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 if crc & 1 else 0)
    return crc & 0xFFFFFFFF


_CRC_TABLE = []
for _n in range(256):
    _c = _n
    for _ in range(8):
        _c = (_c >> 1) ^ (0xEDB88320 if _c & 1 else 0)
    _CRC_TABLE.append(_c)


def crc32_mw_update(crc: int, data: bytes) -> int:
    """Table-driven crc32_mw, for streaming large dumps."""
    t = _CRC_TABLE
    for byte in data:
        crc = (crc >> 8) ^ t[(crc ^ byte) & 0xFF]
    return crc


def le32(value: int) -> bytes:
    return struct.pack("<I", value & 0xFFFFFFFF)


def data_header(address: int, length: int) -> bytes:
    """Type 0x01 (CRC), CRC mode 3, reserved, address, length, CRC."""
    body = bytes((0x01, 0x03, 0x00, 0x00)) + le32(address) + le32(length)
    return body + le32(crc32_mw(body))


def entry_header(address: int) -> bytes:
    """Type 0x04 (entry with CRC), CRC mode 3, reserved, address, 0, CRC."""
    body = bytes((0x04, 0x03, 0x00, 0x00)) + le32(address) + le32(0)
    return body + le32(crc32_mw(body))


def self_test() -> None:
    # Values straight from the datasheet's sample code-loading session.
    hdr = data_header(0x00110000, 0x0000000C)
    assert hdr == bytes.fromhex("01 03 00 00 00 00 11 00 0c 00 00 00 e8 44 1b 80"), hdr.hex(" ")
    ent = entry_header(0x0011007D)
    assert ent == bytes.fromhex("04 03 00 00 7d 00 11 00 00 00 00 00 01 08 3b 65"), ent.hex(" ")
    assert crc32_mw(bytes.fromhex("00 04 00 20 7d 00 11 00")) == 0xFF025046
    print("self-test OK: CRC-32 and both header layouts match the datasheet")


# --------------------------------------------------------------------------
# Transport and protocol
# --------------------------------------------------------------------------
class SerialTransport:
    def __init__(self, port: str, baud: int):
        self.s = serial.Serial()
        self.s.port = port
        self.s.baudrate = baud
        self.s.timeout = 0.05
        self.s.dtr = False
        self.s.rts = False
        self.s.open()

    def read(self, n: int, timeout: float) -> bytes:
        self.s.timeout = max(0.01, min(timeout, 1.0))
        return self.s.read(n)

    def write(self, data: bytes) -> None:
        self.s.write(data)
        self.s.flush()

    def set_break(self, on: bool) -> None:
        """Hold TX low (UART break).  While TX idles high (3.3 V) it can
        back-power the MW300 through its RX pin, so switching the 24 V off
        does not fully reset the chip and the boot ROM never comes up."""
        self.s.break_condition = bool(on)

    def close(self) -> None:
        self.s.close()


class ProtocolError(RuntimeError):
    pass


class Link:
    def __init__(self, transport):
        self.t = transport
        self.rx = bytearray()

    # -- low level -------------------------------------------------------
    def _fill(self, timeout: float) -> bool:
        data = self.t.read(4096, timeout)
        if data:
            self.rx += data
            return True
        return False

    def read_byte(self, timeout: float):
        end = time.monotonic() + timeout
        while True:
            if self.rx:
                return self.rx.pop(0)
            remaining = end - time.monotonic()
            if remaining <= 0:
                return None
            self._fill(min(0.05, remaining))

    def read_exact(self, n: int, timeout: float) -> bytes:
        out = bytearray()
        end = time.monotonic() + timeout
        while len(out) < n:
            if self.rx:
                take = min(n - len(out), len(self.rx))
                out += self.rx[:take]
                del self.rx[:take]
                continue
            remaining = end - time.monotonic()
            if remaining <= 0:
                break
            self._fill(min(0.05, remaining))
        return bytes(out)

    # -- boot ROM protocol ----------------------------------------------
    def send_command(self, value: int) -> None:
        if value not in (0x55, 0x53):
            raise ProtocolError(f"refusing to send non-protocol byte 0x{value:02x}")
        self.t.write(bytes((value,)))

    def send_packet(self, data: bytes) -> None:
        self.t.write(data)

    # The only ROM version seen on this chip.  Matching it stops random bytes
    # from a still-running stub (e.g. a flash dump stream) counting as an ACK.
    ROM_VERSION = b"\x01\x00\x41\x0a"

    def _scan_for_ack(self):
        """Return (version, ack) if a Detection ACK is in self.rx."""
        i = 0
        while i + 4 < len(self.rx):
            if self.rx[i + 4] in (0xA3, 0xAC) and \
                    (self.ROM_VERSION is None or bytes(self.rx[i:i + 4]) == self.ROM_VERSION):
                version = bytes(self.rx[i:i + 4])
                ack = self.rx[i + 4]
                del self.rx[:i + 5]
                return version, ack
            i += 1
        return None

    def detect(self, timeout: float):
        """Send 0x55 until the ROM answers; return (version, ack) or None.

        One 0x55 is followed by a generous quiet period.  The previous
        handshake fired a byte every ~20 ms, and the ROM then emitted
        several duplicate Header Requests; sending slowly avoids stacking
        up detections."""
        end = time.monotonic() + timeout
        last_notice = 0.0
        set_break = getattr(self.t, "set_break", None)
        gate = os.environ.get("UARTBOOT_GATE")
        if gate and set_break:
            # Gated power cycle: hold TX low (no back-powering) while the
            # user switches the board OFF, until the gate file appears; then
            # idle high and probe continuously, and the user switches ON.
            # The chip then powers up into an idle-high line (a low RX at
            # power-up seems to stop the ROM from auto-bauding).
            set_break(True)
            print(f"holding TX low; waiting for gate file {gate}", flush=True)
            while not os.path.exists(gate) and time.monotonic() < end:
                self._fill(0.1)
                self.rx.clear()
            set_break(False)
            time.sleep(0.05)
            print("gate open: TX idle high, probing (switch the power ON now)", flush=True)
            set_break = None            # continuous probing from here on
        # Alternate BREAK_S of TX held low (no back-powering, so a power
        # cycle during it is a clean reset) with PROBE_S of 0x55 probes.
        BREAK_S, PROBE_S = 3.0, 4.0
        phase_end = time.monotonic()
        probing = True
        while time.monotonic() < end:
            if set_break and time.monotonic() >= phase_end:
                probing = not probing
                set_break(not probing)
                phase_end = time.monotonic() + (PROBE_S if probing else BREAK_S)
                if probing:
                    time.sleep(0.05)        # idle-high before the first 0x55
            if not probing:
                self._fill(0.05)
                self.rx.clear()             # nothing meaningful arrives now
                continue
            self.send_command(0x55)
            # Give the ROM time to auto-baud and answer before poking it
            # again.  The datasheet only requires 10 ms, but a stalled
            # response is cheaper than a duplicate detection.
            quiet_until = min(end, time.monotonic() + 0.20)
            while time.monotonic() < quiet_until:
                self._fill(0.02)
                found = self._scan_for_ack()
                if found is not None:
                    return found
                if len(self.rx) > 4096:
                    # Keep the tail; an ACK may be split across reads.
                    del self.rx[:len(self.rx) - 1024]
            now = time.monotonic()
            if now - last_notice > 15:
                last_notice = now
                print(".", end="", flush=True)
        if set_break:
            set_break(False)
        print()
        return None

    def read_header_request(self, timeout: float) -> int:
        """Read 0xA5 LEN LEN~ until a valid request; return LEN."""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            b = self.read_byte(max(0.1, end - time.monotonic()))
            if b is None:
                break
            if b != 0xA5:
                continue
            rest = self.read_exact(4, max(0.2, end - time.monotonic()))
            if len(rest) < 4:
                break
            length = rest[0] | (rest[1] << 8)
            check = rest[2] | (rest[3] << 8)
            if (length ^ check) == 0xFFFF:
                return length
            # Not a real request; put the bytes back and keep scanning.
            self.rx = bytearray(rest) + self.rx
        raise ProtocolError("timed out waiting for a Header Request packet")

    def load(self, image: bytes, base: int = BASE, entry: int = ENTRY) -> None:
        chunks = [image[i:i + CHUNK] for i in range(0, len(image), CHUNK)]
        sent = 0
        while True:
            length = self.read_header_request(30.0)
            if length == 0:
                self.send_command(0x53)
                print(f"\nROM accepted entry 0x{entry:08x}; jumping")
                return
            if length == 16:
                self.send_command(0x53)
                if sent < len(chunks):
                    payload = chunks[sent]
                    self.send_packet(data_header(base + sent * CHUNK,
                                                 len(payload) + 4))
                    sent += 1
                    if sent % 8 == 0 or sent == len(chunks):
                        print(f"\rloading {sent * CHUNK if sent < len(chunks) else len(image)}"
                              f"/{len(image)} bytes", end="", flush=True)
                else:
                    self.send_packet(entry_header(entry))
                continue
            # Otherwise the ROM is asking for the data packet announced by
            # the previous Data Header.
            if sent == 0:
                raise ProtocolError(f"unexpected length request {length}")
            payload = chunks[sent - 1]
            if length != len(payload) + 4:
                raise ProtocolError(
                    f"ROM asked for {length} bytes, expected {len(payload) + 4}")
            self.send_command(0x53)
            self.send_packet(payload + le32(crc32_mw(payload)))

    # -- output from the loaded program ---------------------------------
    def read_line(self, timeout: float):
        out = bytearray()
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            b = self.read_byte(end - time.monotonic())
            if b is None:
                break
            out.append(b)
            if b == 0x0A:
                return bytes(out)
        return bytes(out) if out else None


def parse_size(text: str) -> int:
    text = text.strip().lower()
    if text in ("auto", "0", "0x0"):
        return 0
    mult = 1
    if text.endswith("k"):
        mult, text = 1024, text[:-1]
    elif text.endswith("m"):
        mult, text = 1024 * 1024, text[:-1]
    elif text.endswith("g"):
        mult, text = 1024 * 1024 * 1024, text[:-1]
    return int(text, 0) * mult


def build_image(stub_path: str, dump_len: int) -> bytes:
    with open(stub_path, "rb") as f:
        code = f.read()
    if not code:
        sys.exit(f"empty stub: {stub_path}")
    if len(code) > 0x2000:
        sys.exit(f"stub is {len(code)} bytes; it must be < 0x2000")
    # The loader zero-fills the gap, which also clears the stub's .bss.
    image = code + b"\x00" * (0x2000 - len(code)) + le32(dump_len)
    assert len(image) == 0x2004
    return image


def probe_after_jump(link: "Link", seconds: float = 2.0):
    """Poke 0x55 after the entry ACK and see if the ROM answers again.

    If it does, the chip fell back into UART boot mode instead of running
    the loaded image.  If it does not, the jump happened (the image may
    still be hung, but the ROM is no longer in control)."""
    time.sleep(0.3)
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        link.send_command(0x55)
        time.sleep(0.05)
        link._fill(0.02)
        found = link._scan_for_ack()
        if found is not None:
            return found
    return None


def _save_rx_debug(out_path: str, raw: bytes) -> None:
    path = out_path + ".rx.raw"
    with open(path, "wb") as f:
        f.write(raw)
    print(f"\nraw RX ({len(raw)} bytes) saved to {path}")
    if raw:
        print(f"first {min(128, len(raw))} bytes: {raw[:128].hex(' ')}")


def capture(link: Link, out_path: str, baud: int) -> None:
    print("waiting for the stub's banner ...")
    started = time.monotonic()
    expected = None
    raw = bytearray()
    try:
        while time.monotonic() - started < 15:
            line = link.read_line(10.0)
            if line is None:
                break
            raw += line
            text = line.decode("ascii", "replace").rstrip("\r\n")
            if text:
                print(f"  stub: {text}")
            if "BEGIN " in text:
                try:
                    expected = int(text.split("BEGIN ", 1)[1].split()[0])
                except (IndexError, ValueError):
                    raise ProtocolError(f"bad BEGIN line: {text!r}")
                break
            if "ERROR" in text:
                raise ProtocolError(f"stub reported: {text}")
        if expected is None:
            raise ProtocolError("stub did not send a BEGIN line (no flash or bad wiring)")

        print(f"capturing {expected} bytes ({expected / 1048576:.1f} MiB), "
              f"about {expected * 10 / baud / 60:.1f} min ...")
        got = 0
        digest = hashlib.sha256()
        crc = 0
        last = time.monotonic()
        with open(out_path, "wb") as f:
            while got < expected:
                chunk = link.read_exact(min(65536, expected - got), 60.0)
                if not chunk:
                    raise ProtocolError(f"stream stopped after {got} of {expected} bytes")
                f.write(chunk)
                digest.update(chunk)
                crc = crc32_mw_update(crc, chunk)
                got += len(chunk)
                if time.monotonic() - last > 2:
                    last = time.monotonic()
                    pct = 100.0 * got / expected
                    print(f"\r  {got}/{expected} bytes ({pct:5.1f}%)", end="", flush=True)
        print(f"\nwrote {out_path} ({got} bytes)")
        print(f"sha256 {digest.hexdigest()}")
        tail = b""
        for _ in range(3):
            line = link.read_line(5.0)
            if not line:
                break
            tail += line
            if b"DONE" in line:
                break
        text = tail.decode("ascii", "replace")
        if "DONE CRC " in text:
            want = int(text.split("DONE CRC ", 1)[1].split()[0], 16)
            if want == crc:
                print(f"CRC OK {crc:08x} (matches the stub's own CRC)")
            else:
                raise ProtocolError(f"CRC mismatch: stub {want:08x}, received {crc:08x}")
        else:
            print(f"no DONE CRC line from the stub (got {tail!r})")
    except ProtocolError:
        if raw:
            _save_rx_debug(out_path, bytes(raw))
        raise


def pick_port(pattern: str) -> str:
    ports = sorted(glob.glob(pattern))
    if not ports and not any(c in pattern for c in "*?["):
        return pattern              # e.g. COM5 on Windows: not a file path
    if not ports:
        sys.exit(f"no serial port matches {pattern}")
    if len(ports) > 1:
        print("multiple ports match:", ", ".join(ports), file=sys.stderr)
        sys.exit("pass --port with the exact device")
    return ports[0]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=not ("--self-test" in sys.argv),
                    help="serial port or glob, e.g. /dev/ttyUSB0, /dev/cu.usbserial-*, COM5")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--stub", default=os.path.join(HERE, "mw300_stub_110000.bin"))
    ap.add_argument("--base", default="0x00110000",
                    help="SRAM load address; the entry point is base|1 (default: %(default)s)")
    ap.add_argument("--size", default="auto",
                    help="dump length: auto, 2M, 4M, 0x400000 ... (default: auto)")
    ap.add_argument("--out", default=None,
                    help="output file (default: backup/mw300-flash-<time>.bin, gitignored)")
    ap.add_argument("--detect-timeout", type=float, default=180.0,
                    help="seconds to keep sending 0x55 across the power cycle")
    ap.add_argument("--entry", default=None,
                    help="override the entry address (default: base|1)")
    ap.add_argument("--probe-seconds", type=float, default=2.0,
                    help="how long to send 0x55 after the jump (default: %(default)s)")
    ap.add_argument("--probe-only", action="store_true",
                    help="execution test: load, jump, probe for the ROM, report, exit (no capture)")
    ap.add_argument("--listen", type=float, default=0,
                    help="after the jump, just record raw RX for this many seconds (no capture)")
    ap.add_argument("--self-test", action="store_true",
                    help="check CRC/packet construction against the datasheet and exit")
    args = ap.parse_args()

    if args.self_test:
        self_test()
        return
    self_test()

    dump_len = parse_size(args.size)
    image = build_image(args.stub, dump_len)
    base = int(args.base, 0)
    entry = int(args.entry, 0) if args.entry else base | 1
    port = pick_port(args.port)

    if args.out is None:
        stamp = time.strftime("%Y%m%d-%H%M%S")
        out_dir = os.path.abspath(os.path.join(HERE, "..", "backup"))
        os.makedirs(out_dir, exist_ok=True)
        args.out = os.path.join(out_dir, f"mw300-flash-{stamp}.bin")

    print(f"stub   {args.stub} ({len(image)} bytes image)")
    print(f"port   {port} at {args.baud} 8N1")
    print(f"load   0x{base:08x}, entry 0x{entry:08x}")
    print(f"output {args.out}")
    print(f"size   {'auto (from JEDEC ID)' if dump_len == 0 else hex(dump_len)}")
    print()
    if os.environ.get("UARTBOOT_GATE"):
        print("Gated start: keep the 24 V OFF until the gate file exists (see the README).")
    else:
        print("Power-cycle the purifier's 24 V now (off, wait 2 s, on).  If the ROM does not\n"
              "answer, use the gated start (UARTBOOT_GATE, see the README).")
    print(f"Sending 0x55 for up to {args.detect_timeout:.0f} s ...")

    try:
        transport = SerialTransport(port, args.baud)
    except serial.SerialException as exc:
        sys.exit(f"cannot open {port}: {exc}")
    link = Link(transport)
    try:
        result = link.detect(args.detect_timeout)
        if result is None:
            sys.exit("\nno Detection ACK: check TX/RX wiring and that the BOOT straps are grounded")
        version, ack = result
        print(f"\nROM version {version.hex(' ')}  ack 0x{ack:02x}")
        if ack == 0xAC:
            sys.exit("0xAC: OTP security is on and the ROM wants a password; stopping")
        print("0xA3: no password, loading the stub")

        link.load(image, base=base, entry=entry)

        if args.listen:
            raw = bytearray(link.rx)
            end = time.monotonic() + args.listen
            while time.monotonic() < end:
                raw += transport.read(4096, 0.2)
            path = args.out + ".listen.raw"
            with open(path, "wb") as f:
                f.write(raw)
            print(f"listened {args.listen:.0f} s, {len(raw)} bytes -> {path}")
            print(raw.decode("ascii", "replace"))
            return

        if args.probe_only:
            back = probe_after_jump(link, args.probe_seconds)
            if back is None:
                print(f"PROBE: no ROM answer within {args.probe_seconds:.0f} s after the jump")
                sys.exit(3)
            v, a = back
            print(f"PROBE: ROM answered 0x55 again (version {v.hex(' ')} ack 0x{a:02x})")
            sys.exit(0)

        capture(link, args.out, args.baud)
    except (ProtocolError, serial.SerialException) as exc:
        print(f"\nFAILED: {exc}", file=sys.stderr)
        sys.exit(1)
    finally:
        transport.close()


if __name__ == "__main__":
    main()
