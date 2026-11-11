#!/usr/bin/env python3
"""Remote logs (R3) over the real client library: aioesphomeapi's
subscribe_logs (what Home Assistant uses) and the aioesphomeapi-logs CLI,
against the host server (tests/esphome/host_main.c), which puts a few
"[boot] ..." lines into the log ring before it starts serving.

    python test_logs.py --binary build/host/esphome/esphome_host
"""
import argparse
import asyncio
import os
import subprocess
import sys
import time

from aioesphomeapi import APIClient, LogLevel

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_api import HOST, KEY, PORT, PASSED, Server, ok, wait_for  # noqa: E402


async def run(binary, key=None):
    tag = "noise" if key else "plaintext"
    srv = Server(binary, *(["-k", key] if key else []))
    try:
        cli = APIClient(HOST, PORT, None, client_info="logs test", noise_psk=key)
        await cli.connect(login=True)
        logs = []
        unsub = cli.subscribe_logs(lambda m: logs.append(m), log_level=LogLevel.LOG_LEVEL_VERY_VERBOSE,
                                   dump_config=True)
        msgs = lambda: [m.message.decode() for m in logs]  # noqa: E731
        await wait_for(lambda: any("servers started" in m for m in msgs()), what="history replay")
        m = msgs()
        i = m.index("[I][boot] main reached")
        assert m[i + 1] == "[W][net] wlan_connect failed -5 (will retry)", m
        assert logs[i + 1].level == LogLevel.LOG_LEVEL_WARN
        assert m[i + 2] == "[I][app] servers started", m
        assert any(x.startswith("[C][api]: ") for x in m)       # dump_config
        ok(f"{tag}: subscribe_logs replays the boot history with levels")

        srv.cmd("log [ota] slot B confirmed")
        await wait_for(lambda: "[I][ota] slot B confirmed" in msgs(), what="live line")
        srv.cmd("log *** ASSERT tasks.c:1 at 5s")
        await wait_for(lambda: "[E] *** ASSERT tasks.c:1 at 5s" in msgs(), what="error line")
        assert logs[-1].level == LogLevel.LOG_LEVEL_ERROR
        ok(f"{tag}: live console lines streamed, error level detected")

        # a WARN-level second client: no info lines, in replay or live
        cli2 = APIClient(HOST, PORT, None, client_info="logs test 2", noise_psk=key)
        await cli2.connect(login=True)
        logs2 = []
        cli2.subscribe_logs(lambda m: logs2.append(m), log_level=LogLevel.LOG_LEVEL_WARN)
        srv.cmd("log [app] info only")
        srv.cmd("log [net] wlan: link lost (4)")
        await wait_for(lambda: any(b"link lost" in x.message for x in logs2), what="client2 warn")
        await wait_for(lambda: "[I][app] info only" in msgs(), what="client1 info")
        assert all(x.level <= LogLevel.LOG_LEVEL_WARN for x in logs2), logs2
        assert any(b"wlan_connect failed" in x.message for x in logs2)
        ok(f"{tag}: per-client level filter (WARN client: {len(logs2)} lines)")

        # flood far beyond the ring in one go: dropped notice, newest kept,
        # the connection stays healthy
        srv.cmd("flood 400")
        await wait_for(lambda: any("flood line 000399" in x for x in msgs()), what="flood tail")
        assert any("bytes dropped" in x for x in msgs()), "no drop notice"
        await asyncio.wait_for(cli.device_info(), 3)
        ok(f"{tag}: 25 KB burst -> drop notice, newest lines delivered, API still answers")
        unsub()
        await cli2.disconnect()
        await cli.disconnect()
    finally:
        srv.stop()


def cli_logs(binary):
    """The aioesphomeapi-logs tool (same library, its own connect flow)."""
    exe = os.path.join(os.path.dirname(sys.executable), "aioesphomeapi-logs")
    if not os.path.exists(exe):
        print("  skip aioesphomeapi-logs not installed next to", sys.executable)
        return
    srv = Server(binary)
    p = subprocess.Popen([exe, HOST], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         env={**os.environ, "PYTHONUNBUFFERED": "1"})
    try:
        out, end = [], time.monotonic() + 10
        os.set_blocking(p.stdout.fileno(), False)
        while time.monotonic() < end and not any("servers started" in x for x in out):
            line = p.stdout.readline()
            if line:
                out.append(line.rstrip())
            else:
                time.sleep(0.05)
        assert any("[boot] main reached" in x for x in out), out
        assert any("servers started" in x for x in out), out
        ok("aioesphomeapi-logs CLI shows the replayed boot log")
    finally:
        p.terminate()
        p.wait(timeout=5)
        srv.stop()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    a = ap.parse_args()
    asyncio.run(run(a.binary))
    asyncio.run(run(a.binary, KEY))
    cli_logs(a.binary)
    print(f"ALL PASSED ({len(PASSED)} checks)")


if __name__ == "__main__":
    sys.exit(main())
