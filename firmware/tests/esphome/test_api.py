#!/usr/bin/env python3
"""Integration test of the host ESPHome API server (tests/esphome/host_main.c).

Part 1 uses the official client library, aioesphomeapi (the same code Home
Assistant uses), and follows HA's connect sequence; it runs twice, plaintext
and with Noise encryption (noise_psk).  Part 2 uses raw sockets for framing
edge cases that a well-behaved client never produces.  Part 4 checks the
encryption rejects (wrong key, plaintext client, Noise vs plaintext server).

    python test_api.py --binary build/host/esphome/esphome_host
"""
import argparse
import asyncio
import queue
import socket
import struct
import subprocess
import sys
import threading
import time

from aioesphomeapi import (
    APIClient,
    BinarySensorInfo,
    BinarySensorState,
    ButtonInfo,
    EncryptionPlaintextAPIError,
    EntityCategory,
    FanInfo,
    FanState,
    InvalidAuthAPIError,
    InvalidEncryptionKeyAPIError,
    LogLevel,
    NumberInfo,
    NumberMode,
    NumberState,
    RequiresEncryptionAPIError,
    SelectInfo,
    SelectState,
    SensorInfo,
    SensorState,
    SensorStateClass,
    SwitchInfo,
    SwitchState,
    TextSensorInfo,
    TextSensorState,
)

HOST, PORT = "127.0.0.1", 6053
NAME = "air-purifier-2s-test"
PASSED = []
KEY = "px8k3NnPq7M6ZK2Ylxy0b6t1rWkqKkS2Qp0K0zZKX9E="   # test key, not a secret
WRONG_KEY = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8="
NOISE_PSK = None   # set while part 1 runs encrypted


def ok(msg):
    PASSED.append(msg)
    print(f"  ok  {msg}")


class Server:
    """Runs the host binary; collects stdout and the stub's stderr log."""

    def __init__(self, binary, *args):
        self.p = subprocess.Popen(
            [binary, "-n", NAME, *args],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, bufsize=1,
        )
        self.out = queue.Queue()
        self.stub_log = []
        threading.Thread(target=self._pump, args=(self.p.stdout, self.out.put), daemon=True).start()
        threading.Thread(target=self._pump, args=(self.p.stderr, self._err), daemon=True).start()
        line = self.out.get(timeout=5)
        assert line == "READY", line
        # wait until the port accepts
        for _ in range(100):
            try:
                socket.create_connection((HOST, PORT), timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.05)

    def _err(self, line):
        if line.startswith("STUB "):
            self.stub_log.append(line)
        else:
            print("  [server]", line)

    @staticmethod
    def _pump(f, sink):
        for line in f:
            sink(line.rstrip("\n"))

    def cmd(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()

    def state(self):
        self.cmd("state")
        line = self.out.get(timeout=2)
        assert line.startswith("STATE "), line
        return dict(kv.split("=") for kv in line.split()[1:])

    def stop(self):
        self.p.terminate()
        self.p.wait(timeout=5)
        assert self.p.returncode in (0, -15), f"server exit {self.p.returncode}"


async def wait_for(pred, timeout=3.0, what="condition"):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pred():
            return
        await asyncio.sleep(0.02)
    raise AssertionError(f"timeout waiting for {what}")


class Tracker:
    def __init__(self):
        self.latest = {}
        self.history = []

    def __call__(self, st):
        self.latest[st.key] = st
        self.history.append(st)


async def ha_connect(password=None, client_info="Home Assistant test"):
    """Same calls, same order as homeassistant/components/esphome/manager.py."""
    cli = APIClient(HOST, PORT, password, client_info=client_info, noise_psk=NOISE_PSK)
    await cli.connect(login=True)
    info = await cli.device_info()
    caps = await cli.device_capabilities_compat(info)
    ents, services = await cli.list_entities_services()
    tr = Tracker()
    # HA sends SubscribeStates + SubscribeHomeassistantServices +
    # SubscribeHomeAssistantStates in one packet
    cli.subscribe_home_assistant_states_and_services(
        on_state=tr, on_service_call=lambda *a: None, on_state_sub=lambda *a: None)
    return cli, info, caps, ents, services, tr


async def part1(binary, key=None):
    global NOISE_PSK
    NOISE_PSK = key
    print("part 1: aioesphomeapi" + (" with Noise encryption (noise_psk)" if key else ", plaintext"))
    srv = Server(binary, *(["-k", key] if key else []))
    try:
        cli, info, caps, ents, services, tr = await ha_connect()
        assert cli.api_version.major == 1 and cli.api_version.minor == 19, cli.api_version
        ok(f"hello: API {cli.api_version.major}.{cli.api_version.minor}")
        assert info.api_encryption_supported is bool(key)
        if key:
            ok("Noise handshake done; device_info api_encryption_supported = true")
        assert info.name == NAME
        assert info.model == "Mi Air Purifier 2S" and info.manufacturer == "Xiaomi"
        assert info.friendly_name == "Air Purifier 2S (host test)"
        assert info.mac_address == "02:00:00:2A:02:53", info.mac_address
        assert info.esphome_version == "2026.10.2"
        assert info.project_name == "Xiaomi.Mi Air Purifier 2S" and info.project_version == "0.1.0-host"
        assert not info.uses_password
        ok(f"device_info: {info.name} {info.mac_address} {info.model} esphome={info.esphome_version}")
        assert not caps.wizard.configured
        ok("device_capabilities (empty) answered")

        by_id = {e.object_id: e for e in ents}
        assert len(ents) == 27 and not services, [e.object_id for e in ents]
        assert len({e.key for e in ents}) == 27, "duplicate keys"
        fan = by_id["purifier"]
        assert isinstance(fan, FanInfo) and fan.supports_speed and fan.supported_speed_count == 15
        assert fan.supported_preset_modes == ["Auto", "Silent", "Favorite"]
        assert fan.name == ""
        expect_sensors = {
            "pm25": ("PM2.5", "µg/m³", "pm25", 0, SensorStateClass.MEASUREMENT),
            "temperature": ("Temperature", "°C", "temperature", 1, SensorStateClass.MEASUREMENT),
            "humidity": ("Humidity", "%", "humidity", 0, SensorStateClass.MEASUREMENT),
            "fan_speed": ("Fan speed", "rpm", "", 0, SensorStateClass.MEASUREMENT),
            "filter_life": ("Filter life", "%", "", 0, SensorStateClass.MEASUREMENT),
            "filter_hours": ("Filter used", "h", "duration", 0, SensorStateClass.TOTAL_INCREASING),
            "wifi_signal": ("WiFi signal", "dBm", "signal_strength", 0, SensorStateClass.MEASUREMENT),
            "uptime": ("Uptime", "s", "duration", 0, SensorStateClass.TOTAL_INCREASING),
            "free_heap": ("Free heap", "B", "data_size", 0, SensorStateClass.MEASUREMENT),
            "min_free_heap": ("Min free heap", "B", "data_size", 0, SensorStateClass.MEASUREMENT),
        }
        for oid, (name, unit, dc, acc, sc) in expect_sensors.items():
            e = by_id[oid]
            assert isinstance(e, SensorInfo), oid
            assert (e.name, e.unit_of_measurement, e.device_class, e.accuracy_decimals, e.state_class) == \
                (name, unit, dc, acc, sc), (oid, e)
        assert by_id["wifi_signal"].entity_category == EntityCategory.DIAGNOSTIC
        for oid in ("display_led", "buzzer", "child_lock"):
            assert isinstance(by_id[oid], SwitchInfo)
            assert by_id[oid].entity_category == EntityCategory.CONFIG
        assert isinstance(by_id["reset_filter"], ButtonInfo)
        door = by_id["filter_door"]
        assert isinstance(door, BinarySensorInfo) and door.device_class == "door"
        assert door.entity_category == EntityCategory.NONE
        for oid, name in (("motor_fault", "Motor fault"), ("pm_sensor_fault", "PM sensor fault")):
            e = by_id[oid]
            assert isinstance(e, BinarySensorInfo) and e.device_class == "problem", e
            assert e.name == name and e.entity_category == EntityCategory.DIAGNOSTIC, e
        lvl = by_id["fan_level"]
        assert isinstance(lvl, NumberInfo), lvl
        assert (lvl.name, lvl.icon, lvl.min_value, lvl.max_value, lvl.step, lvl.mode) == \
            ("Fan level", "mdi:fan", 1.0, 15.0, 1.0, NumberMode.SLIDER), lvl
        assert lvl.entity_category == EntityCategory.NONE and not lvl.unit_of_measurement
        sel = by_id["mode"]
        assert isinstance(sel, SelectInfo), sel
        assert (sel.name, sel.icon, sel.options) == ("Mode", "mdi:fan-auto",
                                                     ["Auto", "Silent", "Favorite"]), sel
        assert sel.entity_category == EntityCategory.NONE
        bias = by_id["auto_bias"]
        assert isinstance(bias, NumberInfo), bias
        assert (bias.name, bias.icon, bias.min_value, bias.max_value, bias.step, bias.mode,
                bias.unit_of_measurement, bias.entity_category) == \
            ("Auto speed bias", "mdi:tune-variant", -50.0, 50.0, 10.0, NumberMode.SLIDER,
             "%", EntityCategory.CONFIG), bias
        for oid in ("wifi_signal", "uptime", "free_heap", "min_free_heap"):
            assert by_id[oid].entity_category == EntityCategory.DIAGNOSTIC, oid
        for oid, name, icon in (("firmware", "Firmware", "mdi:chip"),
                                ("reset_reason", "Reset reason", "mdi:restart-alert"),
                                ("ota_slot", "OTA slot", "mdi:swap-horizontal"),
                                ("ip_address", "IP address", "mdi:ip-network")):
            e = by_id[oid]
            assert isinstance(e, TextSensorInfo), e
            assert (e.name, e.icon, e.entity_category) == (name, icon, EntityCategory.DIAGNOSTIC), e
        rst = by_id["restart"]
        assert isinstance(rst, ButtonInfo) and rst.name == "Restart", rst
        assert rst.device_class == "restart" and rst.entity_category == EntityCategory.DIAGNOSTIC
        # nothing secret in any entity (MAC, DID, token, keys)
        assert not [e for e in ents if "mac" in e.object_id or "token" in e.object_id]
        ok("list_entities: fan + 2 numbers + select + 11 sensors + 4 text_sensors + 3 switches + "
           "2 buttons + 3 binary_sensors, fields match")

        # ESPHome key = fnv1(object_id)
        def fnv1(s):
            h = 2166136261
            for c in s.encode():
                h = (h * 16777619) & 0xFFFFFFFF
                h ^= c
            return h
        assert all(e.key == fnv1(e.object_id) for e in ents)
        ok("entity keys = FNV-1(object_id) as in ESPHome")

        k = {oid: e.key for oid, e in by_id.items()}
        await wait_for(lambda: len(tr.latest) == 25, what="initial states")
        s = tr.latest
        assert s[k["purifier"]].state is True and s[k["purifier"]].preset_mode == "Auto"
        assert s[k["purifier"]].speed_level == 0
        assert s[k["pm25"]].state == 12 and abs(s[k["temperature"]].state - 21.5) < 1e-4
        assert s[k["humidity"]].state == 45 and s[k["filter_life"]].state == 87
        assert s[k["filter_hours"]].state == 450 and s[k["wifi_signal"]].state == -55
        assert s[k["display_led"]].state is True and s[k["buzzer"]].state is True
        assert s[k["child_lock"]].state is False and s[k["filter_door"]].state is False
        assert not s[k["pm25"]].missing_state
        assert s[k["motor_fault"]].state is False and s[k["pm_sensor_fault"]].state is False
        assert isinstance(s[k["fan_level"]], NumberState) and s[k["fan_level"]].state == 8
        assert not s[k["fan_level"]].missing_state
        assert isinstance(s[k["mode"]], SelectState) and s[k["mode"]].state == "Auto"
        assert not s[k["mode"]].missing_state
        assert isinstance(s[k["auto_bias"]], NumberState) and s[k["auto_bias"]].state == 0
        assert not s[k["auto_bias"]].missing_state
        assert isinstance(s[k["firmware"]], TextSensorState)
        assert s[k["firmware"]].state == "0.1.0-host" and not s[k["firmware"]].missing_state
        assert s[k["reset_reason"]].state == "power_on" and s[k["ota_slot"]].state == "A"
        ip = s[k["ip_address"]]
        assert ip.missing_state or len(ip.state.split(".")) == 4, ip
        assert 0 <= s[k["uptime"]].state < 60, s[k["uptime"]]
        assert s[k["free_heap"]].state == 100000 and s[k["min_free_heap"]].state == 100000
        ok("subscribe_states: 25 initial states with stub values (fan level 8 in Auto, bias 0, "
           "firmware 0.1.0-host, power_on, slot A)")

        # ---- commands ----
        srv.stub_log.clear()
        cli.fan_command(k["purifier"], state=True, speed_level=4)
        await wait_for(lambda: tr.latest[k["purifier"]].speed_level == 4, what="fan speed push")
        st = tr.latest[k["purifier"]]
        assert st.preset_mode == "Favorite" and st.state
        stub = srv.state()
        assert stub["mode"] == "6" and stub["fav"] == "3", stub
        assert "STUB set_favorite_level 3 -> 0" in srv.stub_log
        await wait_for(lambda: tr.latest[k["fan_level"]].state == 4 and
                       tr.latest[k["mode"]].state == "Favorite", what="number/select follow fan")
        ok("fan_command speed_level=4 -> stub Favorite level 3, fan + number + select pushed")

        cli.fan_command(k["purifier"], preset_mode="Silent")
        await wait_for(lambda: tr.latest[k["purifier"]].preset_mode == "Silent", what="preset push")
        assert tr.latest[k["purifier"]].speed_level == 0 and srv.state()["mode"] == "1"
        ok("fan_command preset_mode=Silent -> stub mode 1")

        cli.fan_command(k["purifier"], state=False)
        await wait_for(lambda: tr.latest[k["purifier"]].state is False, what="fan off push")
        assert srv.state()["power"] == "0"
        cli.fan_command(k["purifier"], state=True, preset_mode="Auto")
        await wait_for(lambda: tr.latest[k["purifier"]].state is True and
                       tr.latest[k["purifier"]].preset_mode == "Auto", what="fan on+auto")
        ok("fan_command off / on+Auto")

        # ---- fan level number / mode select ----
        cli.number_command(k["fan_level"], 9)
        await wait_for(lambda: tr.latest[k["fan_level"]].state == 9 and
                       tr.latest[k["mode"]].state == "Favorite" and
                       tr.latest[k["purifier"]].speed_level == 9, what="number push")
        stub = srv.state()
        assert stub["fav"] == "8" and stub["mode"] == "6" and stub["power"] == "1", stub
        ok("number_command fan_level=9 -> stub Favorite level 8; number, select, fan pushed")

        cli.select_command(k["mode"], "Silent")
        await wait_for(lambda: tr.latest[k["mode"]].state == "Silent" and
                       tr.latest[k["purifier"]].preset_mode == "Silent", what="select push")
        assert tr.latest[k["fan_level"]].state == 9 and srv.state()["mode"] == "1"
        ok("select_command mode=Silent -> stub mode 1; fan level 9 kept on the slider")

        cli.fan_command(k["purifier"], state=False)
        await wait_for(lambda: tr.latest[k["purifier"]].state is False, what="fan off push")
        cli.number_command(k["fan_level"], 2.6)
        await wait_for(lambda: tr.latest[k["purifier"]].state is True and
                       tr.latest[k["fan_level"]].state == 3 and
                       tr.latest[k["mode"]].state == "Favorite", what="number powers on")
        assert srv.state()["fav"] == "2"
        cli.fan_command(k["purifier"], state=False)
        await wait_for(lambda: tr.latest[k["purifier"]].state is False, what="fan off push")
        cli.select_command(k["mode"], "Auto")
        await wait_for(lambda: tr.latest[k["purifier"]].state is True and
                       tr.latest[k["mode"]].state == "Auto", what="select powers on")
        ok("number / select while off -> powered on (as the fan's speed and stock's mode command)")

        # same value again: HA still gets its state back (no stale slider)
        cli.number_command(k["fan_level"], 3)
        await wait_for(lambda: tr.latest[k["mode"]].state == "Favorite", what="number -> Favorite")
        await asyncio.sleep(0.3)
        n_lvl = sum(1 for h in tr.history if h.key == k["fan_level"])
        n_sel = sum(1 for h in tr.history if h.key == k["mode"])
        cli.number_command(k["fan_level"], 3)
        cli.select_command(k["mode"], "Bogus")
        await wait_for(lambda: sum(1 for h in tr.history if h.key == k["fan_level"]) > n_lvl and
                       sum(1 for h in tr.history if h.key == k["mode"]) > n_sel,
                       what="echo of unchanged / invalid values")
        assert tr.latest[k["mode"]].state == "Favorite" and tr.latest[k["fan_level"]].state == 3
        cli.select_command(k["mode"], "Auto")
        await wait_for(lambda: tr.latest[k["mode"]].state == "Auto", what="back to Auto")
        ok("unchanged number / unknown option -> state echoed anyway")

        # ---- auto speed bias number ----
        cli.number_command(k["auto_bias"], 20)
        await wait_for(lambda: tr.latest[k["auto_bias"]].state == 20, what="bias push")
        stub = srv.state()
        assert stub["bias"] == "20" and stub["mode"] == "0", stub
        assert "STUB set_auto_bias 20 -> 0" in srv.stub_log
        assert tr.latest[k["mode"]].state == "Auto"
        cli.number_command(k["auto_bias"], -26)
        await wait_for(lambda: tr.latest[k["auto_bias"]].state == -30, what="bias rounded")
        cli.number_command(k["auto_bias"], 99)
        await wait_for(lambda: tr.latest[k["auto_bias"]].state == 50, what="bias clamped")
        assert srv.state()["bias"] == "50"
        n_b = sum(1 for h in tr.history if h.key == k["auto_bias"])
        cli.number_command(k["auto_bias"], 50)
        await wait_for(lambda: sum(1 for h in tr.history if h.key == k["auto_bias"]) > n_b,
                       what="bias echo of an unchanged value")
        cli.number_command(k["auto_bias"], 0)
        await wait_for(lambda: tr.latest[k["auto_bias"]].state == 0, what="bias 0")
        assert srv.state()["bias"] == "0"
        ok("number_command auto_bias 20 / -26 -> -30 / 99 -> 50 / 0: rounded, clamped, echoed")

        for oid, field in (("display_led", "led"), ("buzzer", "buzzer"), ("child_lock", "lock")):
            want = not tr.latest[k[oid]].state
            cli.switch_command(k[oid], want)
            await wait_for(lambda: tr.latest[k[oid]].state is want, what=f"{oid} push")
            assert srv.state()[field] == str(int(want))
        ok("switch_command display_led/buzzer/child_lock -> stub + pushes")

        cli.button_command(k["reset_filter"])
        await wait_for(lambda: tr.latest[k["filter_life"]].state == 100 and
                       tr.latest[k["filter_hours"]].state == 0, what="filter reset push")
        assert srv.state()["filter_life"] == "100"
        ok("button_command reset_filter -> filter_life 100 / hours 0 pushed")

        # ---- changes from the device side ----
        srv.cmd("door 1")
        await wait_for(lambda: tr.latest[k["filter_door"]].state is True, what="door push")
        n_before = sum(1 for h in tr.history if h.key == k["filter_door"])
        ok("filter door opened on device -> binary_sensor push")
        # command rejected (door open): HA must still get the true state back
        n_fan = sum(1 for h in tr.history if h.key == k["purifier"])
        cli.fan_command(k["purifier"], state=False)
        await wait_for(lambda: sum(1 for h in tr.history if h.key == k["purifier"]) > n_fan,
                       what="state re-send after rejected command")
        assert tr.latest[k["purifier"]].state is True
        assert "STUB set_power 0 -> -6003" in srv.stub_log
        ok("rejected command (door open, -6003) -> real state re-sent")
        srv.cmd("door 0")
        await wait_for(lambda: tr.latest[k["filter_door"]].state is False, what="door close")
        assert sum(1 for h in tr.history if h.key == k["filter_door"]) == n_before + 1

        t0 = time.monotonic()
        srv.cmd("pm 42")
        srv.cmd("temp 233")
        await wait_for(lambda: tr.latest[k["pm25"]].state == 42 and
                       abs(tr.latest[k["temperature"]].state - 23.3) < 1e-4, what="sensor push")
        ok(f"sensor change pushed after {1000 * (time.monotonic() - t0):.0f} ms (1/s throttle)")
        # only changed states: no unrelated pushes
        mark = len(tr.history)
        srv.cmd("hum 50")
        await wait_for(lambda: tr.latest[k["humidity"]].state == 50, what="humidity push")
        await asyncio.sleep(1.5)
        extra = {h.key for h in tr.history[mark:]} - {k["humidity"], k["fan_speed"]}
        assert not extra, f"unexpected pushes: {extra}"
        ok("only changed entities are pushed")

        # ---- faults (B2/B4) ----
        n_pm = sum(1 for h in tr.history if h.key == k["pm25"])
        srv.cmd("pmfault 1")
        await wait_for(lambda: tr.latest[k["pm_sensor_fault"]].state is True and
                       tr.latest[k["pm25"]].missing_state, what="pm fault push")
        assert sum(1 for h in tr.history if h.key == k["pm25"]) == n_pm + 1
        ok("PM sensor fault -> problem binary_sensor on, PM2.5 missing_state (HA: unknown)")
        srv.cmd("pmfault 0")
        await wait_for(lambda: tr.latest[k["pm_sensor_fault"]].state is False and
                       not tr.latest[k["pm25"]].missing_state and
                       tr.latest[k["pm25"]].state == 42, what="pm recovery push")
        ok("PM sensor recovery -> fault off, live PM2.5 pushed")
        srv.cmd("motorfault 1")
        await wait_for(lambda: tr.latest[k["motor_fault"]].state is True, what="motor fault push")
        srv.cmd("motorfault 0")
        await wait_for(lambda: tr.latest[k["motor_fault"]].state is False, what="motor fault clear")
        ok("motor fault -> problem binary_sensor on / off")

        # ---- logs ----
        logs = []
        unsub = cli.subscribe_logs(lambda m: logs.append(m), log_level=LogLevel.LOG_LEVEL_DEBUG,
                                   dump_config=True)
        await wait_for(lambda: len(logs) >= 3, what="dump_config logs")
        cli.button_command(k["reset_filter"])
        await wait_for(lambda: any(b"filter reset" in m.message for m in logs), what="log line")
        unsub()
        ok(f"subscribe_logs: dump_config + runtime log lines ({len(logs)} lines)")

        # ---- Restart button (U2): deferred, refused during upload / trial ----
        logs = []
        unsub = cli.subscribe_logs(lambda m: logs.append(m), log_level=LogLevel.LOG_LEVEL_DEBUG)
        for what in ("upload", "trial"):
            srv.cmd(f"{what} 1")
            srv.state()        # stdin command processed
            cli.button_command(k["restart"])
            await wait_for(lambda: any(b"restart refused" in m.message for m in logs),
                           what=f"refusal log ({what})")
            await asyncio.sleep(0.8)
            assert not [x for x in srv.stub_log if x.startswith("STUB reboot")], srv.stub_log
            srv.cmd(f"{what} 0")
            logs.clear()
        ok("button_command restart during an OTA upload / unconfirmed trial -> refused, logged")
        srv.stub_log.clear()
        t0 = time.monotonic()
        cli.button_command(k["restart"])
        await wait_for(lambda: any(b"restart requested" in m.message for m in logs),
                       what="restart log")
        await wait_for(lambda: "STUB reboot 1 -> 0" in srv.stub_log, what="deferred reboot")
        dt = time.monotonic() - t0
        assert 0.45 <= dt < 2.0, dt
        unsub()
        ok(f"button_command restart -> reboot (tagged) {1000 * dt:.0f} ms later, from the poll")

        # ---- second concurrent client ----
        cli2, *_rest, tr2 = await ha_connect(client_info="second client")
        await wait_for(lambda: len(tr2.latest) == 25, what="client2 initial states")
        cli2.switch_command(k["buzzer"], not tr.latest[k["buzzer"]].state)
        want = not tr.latest[k["buzzer"]].state
        await wait_for(lambda: tr.latest[k["buzzer"]].state is want and
                       tr2.latest[k["buzzer"]].state is want, what="push to both clients")
        ok("2 concurrent clients: command from #2 pushed to #1 and #2")
        await cli2.disconnect()

        # ---- ping / disconnect / reconnect ----
        await cli.disconnect()
        ok("disconnect (DisconnectRequest/Response)")
        cli, info, caps, ents, services, tr = await ha_connect()
        await wait_for(lambda: len(tr.latest) == 25, what="states after reconnect")
        assert tr.latest[k["pm25"]].state == 42
        await cli.disconnect()
        ok("reconnect: device_info, entities, states again")
    finally:
        srv.stop()
    NOISE_PSK = None
    if key:
        return

    # ---- password ----
    srv = Server(binary, "-p", "s3cret")
    try:
        cli, info, *_ = await ha_connect(password="s3cret")
        assert info.uses_password
        await cli.disconnect()
        ok("legacy password: correct password accepted")
        bad = APIClient(HOST, PORT, "wrong")
        try:
            await bad.connect(login=True)
            await bad.device_info()
            await bad.list_entities_services()
            raise AssertionError("wrong password accepted")
        except InvalidAuthAPIError:
            ok("legacy password: wrong password -> InvalidAuthAPIError")
        except Exception as e:  # connection closed also acceptable
            ok(f"legacy password: wrong password rejected ({type(e).__name__})")
        finally:
            await bad.disconnect(force=True)
    finally:
        srv.stop()


# ---------------------------------------------------------------- raw framing
def varint(v):
    out = bytearray()
    while v >= 0x80:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    out.append(v)
    return bytes(out)


def frame(t, payload=b""):
    return b"\x00" + varint(len(payload)) + varint(t) + payload


class Raw:
    """Raw API connection with a persistent receive buffer: frames that
    arrive together in one TCP read are kept for the next expect(), so the
    test doesn't depend on how the kernel coalesces segments."""

    def __init__(self):
        self.s = socket.create_connection((HOST, PORT))
        self.buf = b""

    def send(self, data):
        self.s.sendall(data)

    def _parse(self):
        b = self.buf
        if not b:
            return None
        assert b[0] == 0, f"bad frame start {b[:8]!r}"
        p, vals = 1, []
        for _ in range(2):
            v = sh = 0
            while True:
                if p >= len(b):
                    return None
                c = b[p]
                p += 1
                v |= (c & 0x7F) << sh
                sh += 7
                if not c & 0x80:
                    break
            vals.append(v)
        if len(b) < p + vals[0]:
            return None
        self.buf = b[p + vals[0]:]
        return vals[1], b[p:p + vals[0]]

    def _fill(self, end):
        self.s.settimeout(max(0.01, end - time.monotonic()))
        try:
            d = self.s.recv(4096)
        except socket.timeout:
            return
        if not d:
            raise ConnectionError("closed by server")
        self.buf += d

    def next(self, timeout=3.0):
        """Next frame (type, body), in order."""
        end = time.monotonic() + timeout
        while True:
            f = self._parse()
            if f:
                return f
            if time.monotonic() >= end:
                raise AssertionError("timeout waiting for a frame")
            self._fill(end)

    def expect(self, t, timeout=3.0, skip=()):
        """Next frame must be type t; types in skip may come before it."""
        while True:
            mt, body = self.next(timeout)
            if mt == t:
                return body
            assert mt in skip, f"expected type {t}, got {mt}"

    def closed(self, timeout=2.0):
        """True if the server closes the connection (buffered frames are
        drained first)."""
        end = time.monotonic() + timeout
        try:
            while time.monotonic() < end:
                self.buf = b""
                self._fill(end)
        except (ConnectionError, ConnectionResetError, BrokenPipeError):
            return True
        return False

    def hello(self):
        self.send(frame(1, b"\x0a\x03raw\x10\x01\x18\x13"))
        body = self.expect(2)
        assert body.startswith(b"\x08\x01\x10\x13"), body  # API 1.19

    def close(self):
        self.s.close()


def raw_hello_retry(timeout=3.0):
    """Connect + hello; retry while the server still holds a just-closed
    connection's slot (it notices the close on its next poll)."""
    end = time.monotonic() + timeout
    while True:
        r = Raw()
        try:
            r.hello()
            return r
        except (ConnectionError, ConnectionResetError, BrokenPipeError):
            r.close()
            if time.monotonic() > end:
                raise
            time.sleep(0.05)


def part2(binary):
    print("part 2: raw framing edge cases")
    srv = Server(binary)
    try:
        # hello + ping split into single bytes (partial recv handling); the
        # replies must be exactly HelloResponse then PingResponse
        r = Raw()
        for b in frame(1, b"\x0a\x05bytes\x10\x01\x18\x13") + frame(7):
            r.send(bytes([b]))
        assert r.next()[0] == 2 and r.next()[0] == 8
        ok("hello + ping sent one byte at a time -> HelloResponse, PingResponse")

        # same, but everything in one send: both replies, in order
        r2 = Raw()
        r2.send(frame(1, b"\x0a\x03one\x10\x01\x18\x13") + frame(7) + frame(7))
        assert [r2.next()[0] for _ in range(3)] == [2, 8, 8]
        r2.close()
        ok("hello + 2 pings in one send -> 3 replies in order")

        # unknown message type keeps the connection
        r.send(frame(9999, b"\x08\x01") + frame(7))
        r.expect(8)
        ok("unknown message type 9999 ignored, connection kept")

        # oversized message (bigger than RX buffer) skipped, connection kept
        big = b"\x0a" + varint(3000) + b"x" * 3000
        r.send(frame(40, big) + frame(7))
        r.expect(8)
        ok("3 KB message skipped without dropping the connection")

        # pipelined burst: list entities + 20 pings in one send; every ping
        # answered, all 27 entities listed, ListEntitiesDone
        r.send(frame(11) + frame(7) * 20)
        pongs = listed = 0
        done = False
        while pongs < 20 or not done:
            mt, _ = r.next()
            if mt == 8:
                pongs += 1
            elif mt == 19:
                done = True
            else:
                assert mt in (12, 14, 16, 17, 18, 49, 52, 61), mt
                listed += 1
        assert listed == 27
        ok("pipelined ListEntities + 20 pings -> 27 entities, Done, 20 pongs")
        r.close()

        # message before hello -> closed (on_no_setup_connection)
        r = Raw()
        r.send(frame(11))
        assert r.closed()
        r.close()
        ok("ListEntities before Hello -> connection closed")

        # bad indicator (e.g. Noise client) -> "\0Bad indicator byte", closed
        r = Raw()
        r.send(b"\x01\x00\x00")
        got = b""
        r.s.settimeout(2)
        try:
            while True:
                d = r.s.recv(100)
                if not d:
                    break
                got += d
        except (ConnectionResetError, socket.timeout):
            pass
        assert got == b"\x00Bad indicator byte", got
        r.close()
        ok("bad indicator byte -> '\\x00Bad indicator byte' + close (like ESPHome)")

        # client limit: 3 slots, 4th refused
        conns = [raw_hello_retry() for _ in range(3)]
        extra = Raw()
        assert extra.closed()
        extra.close()
        for c in conns:
            c.send(frame(7))
            c.expect(8)
        ok("3 concurrent raw clients served, 4th refused")
        for c in conns:
            c.send(frame(5))
            c.expect(6)
            assert c.closed()
            c.close()
        ok("DisconnectRequest -> DisconnectResponse + close")

        # slot freed after disconnect
        raw_hello_retry().close()
        ok("slots freed after disconnect")
    finally:
        srv.stop()


def part3_slow(binary):
    print("part 3: timers (slow, ~70 s)")
    srv = Server(binary)
    try:
        idle = Raw()                                    # never says hello
        s = Raw()
        s.hello()
        t0 = time.monotonic()
        s.expect(7, timeout=75)                         # server PingRequest
        dt = time.monotonic() - t0
        assert 59 < dt < 70, dt
        ok(f"server sent keepalive PingRequest after {dt:.0f} s idle")
        assert idle.closed(timeout=3), "no-hello client not dropped"
        ok("client without Hello dropped after 60 s (ESPHome HANDSHAKE_TIMEOUT_MS)")
        s.send(frame(8) + frame(7))                     # PingResponse, Ping
        s.expect(8)
        ok("connection alive after answering the ping")
        s.close()
    finally:
        srv.stop()


async def expect_api_error(cli, exc, what):
    try:
        await cli.connect(login=True)
        await cli.device_info()
        raise AssertionError(f"{what}: connected")
    except exc as e:
        ok(f"{what} -> {type(e).__name__}")
    finally:
        await cli.disconnect(force=True)


async def part4_noise_rejects(binary):
    print("part 4: encryption rejects")
    srv = Server(binary, "-k", KEY)
    try:
        await expect_api_error(APIClient(HOST, PORT, None, noise_psk=WRONG_KEY),
                               InvalidEncryptionKeyAPIError, "wrong key")
        await expect_api_error(APIClient(HOST, PORT, None),
                               RequiresEncryptionAPIError, "plaintext client vs encrypted server")
        # the same, byte for byte (ESPHome: frame 01 00 13, 01 "Bad indicator byte", close)
        r = Raw()
        r.send(frame(1, b"\x0a\x01x\x10\x01\x18\x13"))
        end = time.monotonic() + 3
        while len(r.buf) < 22 and time.monotonic() < end:
            r._fill(end)
        assert bytes(r.buf[:22]) == b"\x01\x00\x13\x01Bad indicator byte", bytes(r.buf)
        assert r.closed()
        r.close()
        ok("raw plaintext hello -> 01 00 13 01 'Bad indicator byte' + close")
        # the server still works for a correct client afterwards
        cli = APIClient(HOST, PORT, None, noise_psk=KEY)
        await cli.connect(login=True)
        assert (await cli.device_info()).api_encryption_supported
        await cli.disconnect()
        ok("correct key still accepted after the rejects")
    finally:
        srv.stop()
    srv = Server(binary)
    try:
        await expect_api_error(APIClient(HOST, PORT, None, noise_psk=KEY),
                               EncryptionPlaintextAPIError, "Noise client vs plaintext server")
    finally:
        srv.stop()
    # an invalid key: the server refuses to start (no plaintext fallback)
    p = subprocess.run([binary, "-n", NAME, "-k", "not-a-key"], capture_output=True, text=True,
                       timeout=5)
    assert p.returncode == 1 and "invalid encryption key" in p.stderr, (p.returncode, p.stderr)
    ok("invalid key -> server exits, never serves plaintext")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--slow", action="store_true", help="also test keepalive timers")
    a = ap.parse_args()
    asyncio.run(part1(a.binary))
    asyncio.run(part1(a.binary, KEY))
    part2(a.binary)
    asyncio.run(part4_noise_rejects(a.binary))
    if a.slow:
        part3_slow(a.binary)
    print(f"ALL PASSED ({len(PASSED)} checks)")


if __name__ == "__main__":
    sys.exit(main())
