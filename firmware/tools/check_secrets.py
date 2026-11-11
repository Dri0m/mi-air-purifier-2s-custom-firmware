#!/usr/bin/env python3
"""check_secrets.py SECRETS_H EXAMPLE_H - sanity-check secrets.h (make check).

Never prints a value.  Exit 1 only for a setting that would leave the device
unusable (an API key the firmware would reject, which disables the API)."""
import base64
import binascii
import re
import sys


def defines(path):
    return dict(re.findall(r'^[ \t]*#define[ \t]+(\w+)[ \t]+"([^"]*)"', open(path).read(), re.M))


s, e = defines(sys.argv[1]), defines(sys.argv[2])
bad = [k for k in ("WIFI_SSID", "WIFI_PASSWORD", "OTA_PASSWORD") if s.get(k) == e.get(k)]
if bad:
    print("WARNING: secrets.h still has template values for " + ", ".join(bad) +
          " (no Wi-Fi = no OTA; image will auto-revert after the trial timeout)")
else:
    print("secrets.h: customized")

key = s.get("API_ENCRYPTION_KEY", "")
if not key:
    print("WARNING: API_ENCRYPTION_KEY is empty: the Home Assistant API is unencrypted" +
          ("" if s.get("API_PASSWORD") else " and has no password"))
else:
    try:
        ok = len(base64.b64decode(key, validate=True)) == 32
    except (binascii.Error, ValueError):
        ok = False
    if not ok:
        sys.exit("ERROR: API_ENCRYPTION_KEY is not base64 of 32 bytes (openssl rand -base64 32); "
                 "the firmware would disable the API and Home Assistant could never connect")
    print("API_ENCRYPTION_KEY: valid")
