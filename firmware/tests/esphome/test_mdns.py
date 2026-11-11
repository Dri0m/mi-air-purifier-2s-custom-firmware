#!/usr/bin/env python3
"""mDNS test of the host server: python-zeroconf browse + resolve (what Home
Assistant's zeroconf discovery uses), a legacy unicast query from an
ephemeral port, and macOS dns-sd if available.

    python test_mdns.py --binary build/host/esphome/esphome_host
"""
import argparse
import asyncio
import shutil
import socket
import struct
import subprocess
import time

from zeroconf import ServiceStateChange
from zeroconf.asyncio import AsyncServiceBrowser, AsyncServiceInfo, AsyncZeroconf

NAME = "air-purifier-2s-test"
TYPE = "_esphomelib._tcp.local."


async def zeroconf_test(encrypted=False):
    azc = AsyncZeroconf()
    found = asyncio.Event()

    def on_change(zeroconf, service_type, name, state_change):
        if name == f"{NAME}.{TYPE}" and state_change is ServiceStateChange.Added:
            found.set()

    browser = AsyncServiceBrowser(azc.zeroconf, TYPE, handlers=[on_change])
    await asyncio.wait_for(found.wait(), 10)
    print(f"  ok  zeroconf browse found {NAME}.{TYPE}")
    info = AsyncServiceInfo(TYPE, f"{NAME}.{TYPE}")
    assert await info.async_request(azc.zeroconf, 3000), "resolve failed"
    props = {k.decode(): (v or b"").decode() for k, v in info.properties.items()}
    assert info.port == 6053, info.port
    assert info.server == f"{NAME}.local.", info.server
    assert props["mac"] == "0200002a0253" and props["network"] == "wifi", props
    assert props["version"] == "2026.10.2" and props["friendly_name"] == "Air Purifier 2S (host test)"
    assert props["platform"] == "MW300" and props["board"] == "mi-air-purifier-2s"
    if encrypted:
        assert props["api_encryption"] == "Noise_NNpsk0_25519_ChaChaPoly_SHA256", props
    else:
        assert "api_encryption" not in props, props
    addrs = info.parsed_addresses()
    assert addrs, "no A record"
    print(f"  ok  zeroconf resolve: {info.server}:{info.port} {addrs} TXT={props}")
    await browser.async_cancel()
    await azc.async_close()
    return addrs[0]


def legacy_unicast(ip):
    """One-shot query from a random port (RFC 6762 6.7): unicast reply with
    the query ID and question echoed."""
    q = struct.pack(">HHHHHH", 0x1234, 0, 1, 0, 0, 0)
    for label in (NAME, "local"):
        q += bytes([len(label)]) + label.encode()
    q += b"\x00" + struct.pack(">HH", 1, 1)  # A, IN
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(2)
    s.sendto(q, ("224.0.0.251", 5353))
    end = time.monotonic() + 3
    while time.monotonic() < end:
        try:
            data, src = s.recvfrom(1500)
        except socket.timeout:
            break
        rid, flags, qd, an = struct.unpack(">HHHH", data[:8])
        if rid == 0x1234 and flags & 0x8000:
            assert qd == 1 and an == 1
            assert data[-4:] == socket.inet_aton(ip), data[-4:]
            print(f"  ok  legacy unicast A query answered by {src[0]}: {ip}")
            return
    raise AssertionError("no legacy unicast answer")


def dns_sd_test():
    if not shutil.which("dns-sd"):
        print("  --  dns-sd not available, skipped")
        return
    p = subprocess.Popen(["dns-sd", "-B", "_esphomelib._tcp", "local."],
                         stdout=subprocess.PIPE, text=True)
    time.sleep(3)
    p.terminate()
    out = p.communicate()[0]
    assert NAME in out, out
    print("  ok  dns-sd -B _esphomelib._tcp lists the device (via mDNSResponder)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    a = ap.parse_args()
    srv = subprocess.Popen([a.binary, "-n", NAME], stdin=subprocess.PIPE,
                           stdout=subprocess.PIPE, text=True)
    try:
        assert srv.stdout.readline().strip() == "READY"
        print("mDNS")
        ip = asyncio.run(zeroconf_test())
        legacy_unicast(ip)
        dns_sd_test()
    finally:
        srv.terminate()
        srv.wait()
    # encrypted: TXT api_encryption=<protocol>, as ESPHome's mdns component
    srv = subprocess.Popen([a.binary, "-n", NAME, "-k", "px8k3NnPq7M6ZK2Ylxy0b6t1rWkqKkS2Qp0K0zZKX9E="],
                           stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    try:
        assert srv.stdout.readline().strip() == "READY"
        print("mDNS, encryption key set")
        asyncio.run(zeroconf_test(encrypted=True))
        print("ALL PASSED (mDNS)")
    finally:
        srv.terminate()
        srv.wait()


if __name__ == "__main__":
    main()
