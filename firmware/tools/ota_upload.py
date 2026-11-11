#!/usr/bin/env python3
"""ota_upload.py - upload an MRVL firmware image to the purifier over HTTP.

The device runs src/ota/ota_server.c:  POST /update (raw MRVL body, headers
Content-Length, X-OTA-Password, X-OTA-MD5), POST /confirm (X-OTA-Password),
GET /version.

  ota_upload.py --host 192.168.1.50 --password PW build/purifier.mrvl
  ota_upload.py --host 192.168.1.50 --version
  ota_upload.py --host ... --password PW --no-confirm IMAGE   # leave it in trial
  ota_upload.py --host ... --password PW --confirm-only       # confirm a trial

An upload waits for the reboot (--wait, default 90 s), checks /version (new
slot running, in trial, image timestamp == this file's) and then confirms the
trial with POST /confirm, retrying while the device says "too_early".
Without the confirm the trial image only stays if it later confirms itself
(>= 5 min up with an HA API session); otherwise it reverts after 10 min.

The image is checked locally first (MRVL magic/signature, segment CRCs,
size <= 0x80000); --force skips that (the device verifies again anyway).
Exit status 0 = device accepted and verified the image, came back on the new
slot and (unless --no-confirm) confirmed it; 1 = anything else.
"""
import argparse
import hashlib
import json
import os
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wmpt  # noqa: E402  (verify_mrvl, crc32_ieee)

SLOT_SIZE = 0x80000
DEFAULT_PORT = 8266
CHUNK = 4096


def http_get(host, port, path, timeout=5.0):
    with socket.create_connection((host, port), timeout=timeout) as s:
        s.sendall(f"GET {path} HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n\r\n".encode())
        return read_response(s, timeout)


def read_response(s, timeout):
    """Read a full response (server closes).  Returns (status, json|text)."""
    s.settimeout(timeout)
    data = b""
    while True:
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            break
        except ConnectionResetError:
            break
        if not chunk:
            break
        data += chunk
        status, body, complete = parse_response(data)
        if complete and status != 100:
            break
    status, body, _ = parse_response(data)
    return status, body


def parse_response(data):
    # Skip interim 100 responses.
    while data.startswith(b"HTTP/1.1 100"):
        end = data.find(b"\r\n\r\n")
        if end < 0:
            return 100, None, False
        data = data[end + 4:]
    end = data.find(b"\r\n\r\n")
    if end < 0:
        return None, None, False
    head = data[:end].decode("latin-1").split("\r\n")
    try:
        status = int(head[0].split()[1])
    except (IndexError, ValueError):
        return None, None, False
    clen = None
    for h in head[1:]:
        k, _, v = h.partition(":")
        if k.strip().lower() == "content-length":
            clen = int(v.strip())
    body = data[end + 4:]
    complete = clen is not None and len(body) >= clen
    text = body[:clen] if clen is not None else body
    try:
        return status, json.loads(text.decode()), complete
    except ValueError:
        return status, text.decode("latin-1"), complete


def show_version(host, port):
    try:
        st, body = http_get(host, port, "/version")
    except OSError as e:
        print(f"version: {e}")
        return None
    if st != 200 or not isinstance(body, dict):
        print(f"version: HTTP {st} {body}")
        return None
    trial = ""
    if body.get("trial"):
        trial = f" TRIAL (unconfirmed, reverts in {body.get('confirm_left_s')} s"
        if "trial_up_s" in body:
            trial += (f"; up {body.get('trial_up_s')} s, /confirm in {body.get('confirm_in_s')} s, "
                      f"HA session {'seen' if body.get('api_seen') else 'not seen'}"
                      f"{'' if body.get('self_confirm') else ', no self-confirm'}")
        trial += ")"
    print(f"device: running slot {body.get('running')} @{body.get('running_addr')}, "
          f"next boot {body.get('next_boot')}, version {body.get('version')} "
          f"({body.get('build')}), image time {body.get('image_time')}, "
          f"gens A={body.get('gen_a')} B={body.get('gen_b')}{trial}")
    return body


def http_post_empty(host, port, path, password, timeout=10.0):
    with socket.create_connection((host, port), timeout=timeout) as s:
        s.sendall((f"POST {path} HTTP/1.1\r\nHost: {host}\r\nX-OTA-Password: {password}\r\n"
                   "Content-Length: 0\r\nConnection: close\r\n\r\n").encode())
        return read_response(s, timeout)


def confirm(host, port, password, limit=120.0, expect_slot=None):
    """POST /confirm until it is accepted (retries on too_early / busy /
    connection errors).  Returns True if the device ends up confirmed."""
    t_end = time.time() + limit
    while True:
        try:
            st, body = http_post_empty(host, port, "/confirm", password)
        except OSError as e:
            st, body = None, str(e)
        txt = json.dumps(body) if isinstance(body, dict) else body
        if st == 200 and isinstance(body, dict) and body.get("ok"):
            if body.get("confirmed"):
                print(f"confirm: HTTP 200 {txt}")
                if expect_slot and body.get("running") != expect_slot:
                    print(f"confirm: WARNING confirmed slot {body.get('running')}, "
                          f"expected {expect_slot}")
                    return False
                print("CONFIRMED: the new image is permanent now")
                return True
            print(f"confirm: HTTP 200 {txt} - device is not in a trial, nothing to confirm")
            if expect_slot is None:
                return True
            # an earlier /confirm whose reply was lost, or the fallback
            # self-confirm, may already have made the expected slot permanent
            try:
                _, v = http_get(host, port, "/version")
            except OSError:
                v = None
            if isinstance(v, dict) and v.get("running") == expect_slot and not v.get("trial"):
                print(f"CONFIRMED: slot {expect_slot} is running and permanent")
                return True
            return False
        wait = None
        if st == 409 and isinstance(body, dict) and body.get("error") == "too_early":
            wait = body.get("retry_in_ms", 1000) / 1000.0 + 0.5
            print(f"confirm: too early, retrying in {wait:.1f} s")
        elif st == 503 or st is None:
            wait = 2.0
            print(f"confirm: {('HTTP 503 busy' if st == 503 else txt)}, retrying")
        elif st == 404:
            print("confirm: HTTP 404 - this firmware has no POST /confirm "
                  "(older image: it confirms itself after 60 s)")
            return False
        else:
            print(f"confirm: HTTP {st} {txt}")
            return False
        if time.time() + wait > t_end:
            print("confirm: giving up (the trial image reverts unless it confirms itself)")
            return False
        time.sleep(wait)


def mrvl_time(img):
    return struct.unpack_from("<I", img, 8)[0] if len(img) >= 12 else None


def check_trial(body, expect_slot, img_time):
    """/version after the reboot looks like our image in trial?  Returns
    (ok, needs_confirm)."""
    ok = True
    if expect_slot and body.get("running") != expect_slot:
        print(f"check: FAIL running slot {body.get('running')}, expected {expect_slot}")
        ok = False
    if img_time is not None and body.get("image_time") != img_time:
        print(f"check: FAIL image time {body.get('image_time')} != uploaded image's {img_time}")
        ok = False
    if body.get("table") not in (None, "ok"):
        print(f"check: FAIL partition table: {body.get('table')}")
        ok = False
    if not ok:
        return False, False
    if not body.get("trial"):
        print("check: new image runs but is not in a trial (already confirmed)")
        return True, False
    if body.get("confirm") != "post":
        print("check: new image has no POST /confirm (older firmware); it confirms itself")
        return True, False
    print(f"check: OK - slot {body.get('running')}, image time {body.get('image_time')}, trial")
    return True, True


def progress(sent, total, last):
    pct = sent * 100 // total
    if sys.stdout.isatty():
        sys.stdout.write(f"\r  sent {sent}/{total} bytes ({pct}%)")
        sys.stdout.flush()
        if sent == total:
            sys.stdout.write("\n")
        return pct
    if pct // 10 != last // 10 or sent == total:
        print(f"  sent {sent}/{total} bytes ({pct}%)")
    return pct


def upload(host, port, password, img, md5_hex, timeout):
    hdr = (f"POST /update HTTP/1.1\r\nHost: {host}\r\n"
           f"Content-Type: application/octet-stream\r\n"
           f"Content-Length: {len(img)}\r\nX-OTA-Password: {password}\r\n"
           f"X-OTA-MD5: {md5_hex}\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n")
    s = socket.create_connection((host, port), timeout=timeout)
    try:
        s.sendall(hdr.encode())
        # Wait briefly for "100 Continue" or an early rejection (401, 409, 413...).
        s.settimeout(2.0)
        early = b""
        try:
            while b"\r\n\r\n" not in early:
                c = s.recv(1024)
                if not c:
                    break
                early += c
        except socket.timeout:
            pass                                    # old server: just send
        if early and not early.startswith(b"HTTP/1.1 100"):
            s.settimeout(timeout)
            rest = early
            st, body, complete = parse_response(rest)
            if not complete:
                st, body = read_response_prefixed(s, rest, timeout)
            return st, body
        s.settimeout(timeout)
        sent, last = 0, -1
        t0 = time.time()
        try:
            while sent < len(img):
                n = s.send(img[sent:sent + CHUNK])
                sent += n
                last = progress(sent, len(img), last)
        except (BrokenPipeError, ConnectionResetError) as e:
            print(f"\n  connection dropped after {sent} bytes: {e}")
        dt = max(time.time() - t0, 1e-3)
        print(f"  {sent} bytes in {dt:.1f} s ({sent / dt / 1024:.0f} KiB/s), "
              "waiting for verification...")
        return read_response_prefixed(s, early if early.startswith(b"HTTP/1.1 100") else b"",
                                      timeout)
    finally:
        s.close()


def read_response_prefixed(s, prefix, timeout):
    s.settimeout(timeout)
    data = prefix
    while True:
        st, body, complete = parse_response(data)
        if complete and st != 100:
            return st, body
        try:
            c = s.recv(4096)
        except (socket.timeout, ConnectionResetError):
            break
        if not c:
            break
        data += c
    st, body, _ = parse_response(data)
    return st, body


def wait_back(host, port, old, limit):
    """Returns the new /version body, or None."""
    print(f"waiting up to {limit} s for the device to come back...")
    t_end = time.time() + limit
    time.sleep(2.0)
    while time.time() < t_end:
        try:
            st, body = http_get(host, port, "/version", timeout=2.0)
            if st == 200 and isinstance(body, dict):
                if body.get("update_pending"):
                    pass                            # old image, not rebooted yet
                elif old and body.get("running") == old.get("running"):
                    print(f"WARNING: device still runs slot {body.get('running')} "
                          "(update reverted or not applied)")
                    show_version(host, port)
                    return None
                else:
                    return show_version(host, port)
        except OSError:
            pass
        time.sleep(1.0)
    print("device did not come back in time")
    return None


def main():
    ap = argparse.ArgumentParser(description="Upload an MRVL image to the purifier (POST /update).")
    ap.add_argument("image", nargs="?")
    ap.add_argument("--host", required=True)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--password", default=os.environ.get("OTA_PASSWORD"))
    ap.add_argument("--version", action="store_true", help="only show GET /version")
    ap.add_argument("--force", action="store_true", help="skip local image checks")
    ap.add_argument("--md5", help="override the MD5 sent (testing)")
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--wait", type=float, nargs="?", const=90.0, default=90.0,
                    help="seconds to wait for the reboot (default 90)")
    ap.add_argument("--no-confirm", action="store_true",
                    help="do not POST /confirm: leave the new image in trial "
                         "(self-confirm or revert test)")
    ap.add_argument("--confirm-only", action="store_true",
                    help="no upload: confirm the trial image that is running now")
    ap.add_argument("--confirm-timeout", type=float, default=240.0,
                    help="seconds to keep retrying POST /confirm (default 240; "
                         "the device accepts it from 150 s trial uptime)")
    a = ap.parse_args()

    if a.version:
        return 0 if show_version(a.host, a.port) else 1
    if a.confirm_only:
        if a.image or a.no_confirm:
            ap.error("--confirm-only takes no image and no --no-confirm")
        if not a.password:
            ap.error("--password (or $OTA_PASSWORD) required")
        body = show_version(a.host, a.port)
        if body is None:
            return 1
        if not body.get("trial"):
            print("device is not in a trial: nothing to confirm")
            return 0
        if body.get("confirm") != "post":
            print("this firmware has no POST /confirm (older image: it confirms itself)")
            return 1
        ok = confirm(a.host, a.port, a.password, a.confirm_timeout, body.get("running"))
        show_version(a.host, a.port)
        return 0 if ok else 1
    if not a.image:
        ap.error("image required (or --version / --confirm-only)")
    if not a.password:
        ap.error("--password (or $OTA_PASSWORD) required")

    img = open(a.image, "rb").read()
    md5 = hashlib.md5(img).hexdigest()
    print(f"image {a.image}: {len(img)} bytes, md5 {md5}")
    if not a.force:
        if len(img) > SLOT_SIZE:
            print(f"refusing: {len(img)} bytes > slot size {SLOT_SIZE:#x}")
            return 1
        ok, txt = wmpt.verify_mrvl(img, SLOT_SIZE)
        if not ok:
            print(f"refusing: local check failed: {txt}  (use --force to send anyway)")
            return 1
        print("  local check OK: " + txt.split("\n")[0])

    before = show_version(a.host, a.port)
    try:
        st, body = upload(a.host, a.port, a.password, img, a.md5 or md5, a.timeout)
    except OSError as e:
        print(f"upload failed: {e}")
        return 1
    print(f"result: HTTP {st} {json.dumps(body) if isinstance(body, dict) else body}")
    if st != 200 or not isinstance(body, dict) or not body.get("ok"):
        return 1
    expect = body.get("next_boot")
    print(f"device verified the image; next boot slot {expect}, "
          f"rebooting in {body.get('reboot_in_ms')} ms"
          + (" (boots as a trial)" if body.get("needs_confirm") else ""))
    if not a.wait:
        print("not waiting: the trial is NOT confirmed (self-confirm or revert)")
        return 0
    after = wait_back(a.host, a.port, before, a.wait)
    if after is None:
        return 1
    ok, needs = check_trial(after, expect, mrvl_time(img))
    if not ok:
        print("NOT confirming: /version does not match the uploaded image "
              "(the trial reverts on its own)")
        return 1
    if not needs:
        return 0
    if a.no_confirm:
        print("--no-confirm: left in trial; it confirms itself after >= 5 min up with an "
              f"HA API session, otherwise reverts in {after.get('confirm_left_s')} s")
        return 0
    ok = confirm(a.host, a.port, a.password, a.confirm_timeout, expect)
    show_version(a.host, a.port)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
