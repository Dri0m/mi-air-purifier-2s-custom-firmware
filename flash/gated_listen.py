"""usage: gated_listen.py PORT OUT.log GATEFILE SECONDS

Hold UART TX low (break) until a gate file exists, then idle high and
record RX for N seconds.  Avoids back-powering the target while it is off."""
import os, sys, time, serial
port, out, gate, secs = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4])
s = serial.Serial(); s.port = port; s.baudrate = 115200; s.timeout = 0.1
s.dtr = s.rts = False; s.open()
s.break_condition = True
print("holding TX low; waiting for gate", flush=True)
while not os.path.exists(gate):
    s.read(4096)
s.break_condition = False
print("gate open: TX idle high, recording (power ON now)", flush=True)
end = time.time() + secs
with open(out, "ab") as f:
    while time.time() < end:
        d = s.read(4096)
        if d:
            f.write(d); f.flush()
s.close()
print("done", flush=True)
