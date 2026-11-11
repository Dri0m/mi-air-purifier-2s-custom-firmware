#!/usr/bin/env python3
"""End-to-end: tests/ota/ota_host_server (real ota_server.c + ota core on a
simulated flash, POSIX sockets on 127.0.0.1) driven by tools/ota_upload.py.
The flash is generated with `wmpt.py mkflash` (independent of the C tests)."""
import argparse
import os
import socket
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.normpath(os.path.join(HERE, "..", ".."))
TOOLS = os.path.join(FW, "tools")
PW = "e2e-pass"
failures = 0


def check(cond, what):
    global failures
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        failures += 1


def mrvl_len(d):
    n = struct.unpack_from("<I", d, 12)[0]
    return max(struct.unpack_from("<I", d, 24 + 20 * i)[0] + struct.unpack_from("<I", d, 28 + 20 * i)[0]
               for i in range(n))


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def upload(port, *args):
    cmd = [sys.executable, os.path.join(TOOLS, "ota_upload.py"), "--host", "127.0.0.1",
           "--port", str(port)] + list(args)
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    return r.returncode, r.stdout + r.stderr


def start_server(server, flash, port, mode):
    args = [server, flash, str(port), PW] + ([mode] if mode else [])
    srv = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log = []
    t_end = time.time() + 10
    while time.time() < t_end:
        line = srv.stdout.readline()
        log.append(line)
        if line.startswith("LISTENING"):
            break
    check(any(l.startswith("LISTENING") for l in log), f"server listening ({mode or 'no HA'})")
    return srv, log


def stop_server(srv, log):
    srv.terminate()
    try:
        rest, _ = srv.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        srv.kill()
        rest, _ = srv.communicate()
    log.append(rest or "")
    return "".join(log)


def version(port):
    rc, out = upload(port, "--version")
    return out


def mkflash(tools, flash, imgs):
    subprocess.run([sys.executable, os.path.join(tools, "wmpt.py"), "mkflash", flash,
                    "--appfw-a", imgs["A"], "--appfw-b", imgs["B"]], check=True,
                   capture_output=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", required=True)
    ap.add_argument("--images", required=True)
    ap.add_argument("--work", required=True)
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)

    imgs = {}
    for s in "AB":
        raw = open(os.path.join(a.images, f"appfw{s}.raw"), "rb").read()
        imgs[s] = os.path.join(a.work, f"img{s}.mrvl")
        open(imgs[s], "wb").write(raw[:mrvl_len(raw)])
    bad = bytearray(open(imgs["A"], "rb").read())
    bad[0x30000] ^= 0x55
    badp = os.path.join(a.work, "bad.mrvl")
    open(badp, "wb").write(bad)

    # Server timings (tests/ota/host.mk): /confirm from 2 s, self-confirm at
    # 4 s with an API session of 1.5 s, revert at 9 s.
    TIMEOUT_S = 9.0

    # ---- 1: no HA session: only POST /confirm confirms ----
    flash = os.path.join(a.work, "flash.bin")
    mkflash(TOOLS, flash, imgs)
    port = free_port()
    srv, log = start_server(a.server, flash, port, None)
    try:
        out = version(port)
        check("running slot A" in out and "TRIAL" in out and "/confirm in" in out,
              "GET /version -> slot A, trial (stock-made table: never confirmed)")

        rc, out = upload(port, "--password", "wrong", "--confirm-only")
        check(rc == 1 and "HTTP 401" in out, "--confirm-only, wrong password -> 401")
        time.sleep(3.2)                             # server lockout
        rc, out = upload(port, "--password", PW, "--confirm-only")
        check(rc == 0 and "CONFIRMED" in out, "--confirm-only confirms the running trial")
        rc, out = upload(port, "--password", PW, "--confirm-only")
        check(rc == 0 and "nothing to confirm" in out, "--confirm-only on a confirmed image: no-op")

        rc, out = upload(port, "--password", "wrong", imgs["A"])
        check(rc == 1 and "HTTP 401" in out, "wrong password -> 401")
        time.sleep(3.2)

        rc, out = upload(port, "--password", PW, badp)
        check(rc == 1 and "refusing: local check failed" in out, "corrupt image refused locally")
        rc, out = upload(port, "--password", PW, "--force", badp)
        check(rc == 1 and "HTTP 422" in out and "segment_crc_mismatch" in out,
              "corrupt image (--force) -> device 422")

        rc, out = upload(port, "--password", PW, "--md5", "0" * 32, imgs["A"])
        check(rc == 1 and "md5_mismatch" in out, "wrong MD5 -> 422")

        rc, out = upload(port, "--password", PW, "--wait", "20", imgs["A"])
        check(rc == 0 and "running slot B" in out and "check: OK" in out and
              "CONFIRMED" in out, "good upload -> reboot into B, checked, confirmed")
        if rc != 0:
            print(out)
        rc, out = upload(port, "--password", PW, "--wait", "20", imgs["B"])
        check(rc == 0 and "running slot A" in out and "CONFIRMED" in out,
              "second upload -> back to A, confirmed")
        if rc != 0:
            print(out)
        time.sleep(TIMEOUT_S + 1)
        check("running slot A" in version(port) and "TRIAL" not in version(port),
              "confirmed image stays past the timeout")

        rc, out = upload(port, "--password", PW, "--no-confirm", "--wait", "20", imgs["A"])
        check(rc == 0 and "running slot B" in out and "left in trial" in out
              and "CONFIRMED" not in out, "--no-confirm: B left in trial")
        time.sleep(TIMEOUT_S + 2)
        out = version(port)
        check("running slot A" in out and "TRIAL" not in out,
              "unconfirmed trial without HA session reverted to A")

        rc, out = upload(port, "--password", PW, "--no-confirm", "--wait", "20", imgs["A"])
        check(rc == 0 and "left in trial" in out, "--no-confirm again: B in trial")
        rc, out = upload(port, "--password", PW, "--confirm-only")
        check(rc == 0 and "CONFIRMED" in out, "--confirm-only confirms it later")
        time.sleep(TIMEOUT_S + 1)
        check("running slot B" in version(port), "confirmed trial stays on B")
    finally:
        text = stop_server(srv, log)
    check("boot2 fallback" not in text and "HANG" not in text, "boot2 never needed its fallback")
    check(text.count("CONFIRMED after") == 4,
          f"4 confirms over HTTP (got {text.count('CONFIRMED after')})")
    check(text.count("REBOOT") == 5, f"4 uploads + 1 revert = 5 reboots (got {text.count('REBOOT')})")
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "wmpt.py"), "validate", flash],
                       capture_output=True, text=True)
    check(r.returncode == 0, "wmpt.py validate final flash")
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "wmpt.py"), "show", flash],
                       capture_output=True, text=True)
    check("boot2 boots appfw entry [3]" in r.stdout, "final flash boots slot B")
    if failures:
        print("server log:\n" + text)
        print(r.stdout)

    # ---- 2: HA session present: the trial confirms itself (fallback) ----
    flash2 = os.path.join(a.work, "flash2.bin")
    mkflash(TOOLS, flash2, imgs)
    port = free_port()
    srv, log = start_server(a.server, flash2, port, "--api")
    try:
        time.sleep(5.5)
        out = version(port)
        check("running slot A" in out and "TRIAL" not in out, "initial trial self-confirmed (HA session)")
        rc, out = upload(port, "--password", PW, "--no-confirm", "--wait", "20", imgs["A"])
        check(rc == 0 and "left in trial" in out, "--no-confirm with HA: B in trial")
        time.sleep(TIMEOUT_S + 1)
        out = version(port)
        check("running slot B" in out and "TRIAL" not in out,
              "trial with an HA session confirmed itself, no revert")
    finally:
        text = stop_server(srv, log)
    check(text.count("CONFIRMED after") == 2 and text.count("REBOOT") == 1,
          "two self-confirms, one reboot")
    if failures:
        print("server log:\n" + text)
    print("test-ota-e2e:", "PASS" if not failures else f"{failures} FAILED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
