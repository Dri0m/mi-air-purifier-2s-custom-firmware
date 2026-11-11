"""MW300 UART boot ROM handshake: send the auto-baud byte 0x55, print the reply.

Per the 88MW300 datasheet the ROM answers 0x55 with 4 bytes of ROM version and
an ACK: 0xA3 = no password (OTP security off), 0xAC = password required.

This tool can only ever send 0x55. The ROM also has an "erase entire flash"
command (0x51); send() refuses every byte except 0x55.

usage: uartboot_hello.py PORT_GLOB [BAUD] [SECONDS]
Keeps sending for SECONDS (default 2), so it can run across a power cycle.
"""
import glob, sys, time
import serial

ALLOWED = {0x55}

if len(sys.argv) < 2:
    sys.exit(__doc__)
pattern = sys.argv[1]
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
secs = float(sys.argv[3]) if len(sys.argv) > 3 else 2
ports = sorted(glob.glob(pattern)) or ([pattern] if not any(c in pattern for c in "*?[") else [])
if not ports:
    sys.exit(f"no port matches {pattern}")

s = serial.Serial()
s.port, s.baudrate, s.timeout = ports[0], baud, 0.02
s.dtr = s.rts = False
s.open()


def send(b):
    if b not in ALLOWED:
        raise SystemExit(f"refusing to send 0x{b:02x}")
    s.write(bytes([b]))


got = bytearray()
i = 0
end = time.time() + secs
while time.time() < end:
    send(0x55)
    i += 1
    time.sleep(0.015)
    got += s.read(64)
    if len(got) >= 5 and got[-1] in (0xA3, 0xAC):
        break
s.close()
print(f"port {ports[0]}, {i} x 0x55 sent, reply: {got.hex(' ') or '(nothing)'}")
if got[-1:] == b"\xa3":
    print("ACK 0xA3: boot ROM is listening, no password (OTP security off)")
elif got[-1:] == b"\xac":
    print("ACK 0xAC: boot ROM wants a password (OTP security on)")
