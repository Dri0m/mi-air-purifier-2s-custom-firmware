#!/usr/bin/env python3
"""Offline tests for the guarded flash writer (uartboot_flash.py).

No serial port and no hardware.  It checks:
  * the host-side range guard against every forbidden/allowed region;
  * the host error-code table matches flashwr_core.h exactly;
  * build_table / parse_table round-trip and wmpt.py accepts our tables;
  * the chunk plan for the real firmware image is correct;
  * activate b / activate a produce tables with the gen ordering boot2 needs;
  * a simulated ROM + W25Q16, driven through uartboot_flash.Flasher, writes
    slot B and refuses forbidden ranges (same behaviour as the C stub test).

The simulated stub reuses the compiled host core only indirectly: the pure
guard logic is re-checked here in Python against the same constants, and the
end-to-end C test (test_flashwr.c) exercises the real flashwr_core.c.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.abspath(os.path.join(HERE, "..", "firmware", "tools")))

import uartboot_flash as uf  # noqa: E402
import uartboot_load as ub   # noqa: E402

try:
    import wmpt              # firmware/tools/wmpt.py, the OTA agent's validator
    HAVE_WMPT = True
except Exception:            # noqa: BLE001
    HAVE_WMPT = False

FW_IMAGE = os.path.abspath(os.path.join(HERE, "..", "firmware", "build", "purifier.mrvl"))

fails = 0


def ok(name, cond):
    global fails
    print(f"  {name:55s} {'ok' if cond else 'FAIL'}")
    if not cond:
        fails += 1


# ---------------------------------------------------------------- header sync
def test_header_sync():
    print("== constants stay in sync with flashwr_core.h ==")
    h = open(os.path.join(HERE, "flashwr_core.h")).read()

    def val(name):
        import re
        m = re.search(rf"#define\s+{name}\s+([0-9A-Fa-fxXuU]+)", h)
        return int(m.group(1).rstrip("uU"), 0)

    ok("FW_PARAM_OFF", uf.FW_PARAM_OFF == val("FW_PARAM_OFF"))
    ok("FW_PAYLOAD_OFF", uf.FW_PAYLOAD_OFF == val("FW_PAYLOAD_OFF"))
    ok("FW_PARAM_MAGIC", uf.FW_PARAM_MAGIC == val("FW_PARAM_MAGIC"))
    ok("FW_MAX_PAYLOAD", uf.FW_MAX_PAYLOAD == val("FW_MAX_PAYLOAD"))
    ok("SLOTB", uf.SLOTB == val("FW_SLOTB"))
    ok("SLOTB_END", uf.SLOTB_END == val("FW_SLOTB_END"))
    ok("TABLE0/1", uf.TABLE0 == val("FW_TABLE0") and uf.TABLE1 == val("FW_TABLE1"))
    # The enum order: parse FW_E_* names from the header and compare.
    import re
    enum = h[h.index("enum {"):h.index("FW_E_COUNT")]
    names = re.findall(r"FW_(E_\w+|OK)", enum)
    # map to the host list labels
    mapping = ["ok", "params", "op", "length", "unaligned", "range",
               "table-format", "active-table", "table-gen", "image",
               "slot-b-active", "layout", "payload-crc", "jedec",
               "protected", "wel", "timeout", "verify", "cmd"]
    ok("error table length matches enum", len(uf.FW_ERRORS) == len(mapping) == len(names))
    ok("FW_ERRORS order matches enum", uf.FW_ERRORS == mapping)


# ---------------------------------------------------------------- range guard
def test_range_guard():
    print("== host range guard (uf.host_check_range) ==")
    W, C, R = uf.FW_OP_WRITE, uf.FW_OP_CRC, uf.FW_OP_READ
    cases = [
        ("WRITE boot2",        W, 0x0,      0x1000, uf.E_RANGE),
        ("WRITE boot2 top",    W, 0x3000,   0x1000, uf.E_RANGE),
        ("WRITE PSM lo",       W, 0x6000,   0x1000, uf.E_RANGE),
        ("WRITE PSM hi",       W, 0x9000,   0x1000, uf.E_RANGE),
        ("WRITE slot A start", W, 0xA000,   0x1000, uf.E_RANGE),
        ("WRITE slot A last",  W, 0x89000,  0x1000, uf.E_RANGE),
        ("WRITE straddle A/B", W, 0x89000,  0x2000, uf.E_RANGE),
        ("WRITE wififw",       W, 0x10A000, 0x1000, uf.E_RANGE),
        ("WRITE past slot B",  W, 0x109000, 0x2000, uf.E_RANGE),
        ("WRITE table0",       W, 0x4000,   0xD4,   uf.E_OK),
        ("WRITE table1",       W, 0x5000,   0xD4,   uf.E_OK),
        ("WRITE 2 tbl sectors",W, 0x4000,   0x1001, uf.E_RANGE),
        ("WRITE slot B start", W, 0x8A000,  0x1000, uf.E_OK),
        ("WRITE slot B 64K",   W, 0x8A000,  0x10000,uf.E_OK),
        ("WRITE slot B last",  W, 0x109000, 0x1000, uf.E_OK),
        ("WRITE unaligned",    W, 0x8A800,  0x1000, uf.E_ALIGN),
        ("WRITE over max",     W, 0x8A000,  0x10001,uf.E_LEN),
        ("WRITE zero len",     W, 0x8A000,  0,      uf.E_LEN),
        ("CRC slot range",     C, 0xA000,   0x100000,uf.E_OK),
        ("CRC tables",         C, 0x4000,   0x2000, uf.E_OK),
        ("CRC touching PSM",   C, 0x6000,   0x1000, uf.E_RANGE),
        ("CRC boot2",          C, 0x0,      0x1000, uf.E_RANGE),
        ("READ tables",        R, 0x4000,   0x2000, uf.E_OK),
        ("READ PSM",           R, 0x6000,   0x1000, uf.E_RANGE),
        ("READ slot",          R, 0xA000,   0x1000, uf.E_RANGE),
    ]
    for name, op, off, ln, exp in cases:
        ok(name, uf.host_check_range(op, off, ln) == exp)

    # Exhaustive sweep: every 4K-aligned write sector must be allowed iff it
    # is a table sector or inside slot B.
    bad = 0
    for off in range(0, uf.FLASH_SIZE, 0x1000):
        allowed = uf.host_check_range(W, off, 0x1000) == uf.E_OK
        want = off in (0x4000, 0x5000) or (0x8A000 <= off < 0x10A000)
        if allowed != want:
            bad += 1
    ok("exhaustive 4K sector sweep (512 sectors)", bad == 0)


# ---------------------------------------------------------------- tables
def test_tables():
    print("== WMPT build / parse / wmpt.py acceptance ==")
    t = uf.build_table(3, 3, 4, flag_b=ord("T"))
    ok("build_table length 0xD4", len(t) == 0xD4)
    p = uf.parse_table(t)
    ok("parse_table ok", p["ok"])
    ok("residue zero (crc over whole table)", ub.crc32_mw(t) == 0)
    ok("selects B (gen b>a)", uf.table_selects_b(p))
    ok("trial flag read back", p["entries"][uf.APPFW_B]["flag"] == ord("T"))
    # field-by-field against the extracted stock tables in boot2.bin
    boot2 = os.path.abspath(os.path.join(HERE, "..", "backup", "re", "boot2.bin"))
    if os.path.exists(boot2):
        d = open(boot2, "rb").read()
        ok("build_table reproduces stock table @0x4000 byte-exact",
           d[0x4000:0x4000 + 0xD4] == uf.build_table(2, 3, 2))
        ok("build_table reproduces stock table @0x5000 byte-exact",
           d[0x5000:0x5000 + 0xD4] == uf.build_table(1, 1, 2))
    else:
        print("  (backup/re/boot2.bin absent; skipped stock-table compare)")
    if HAVE_WMPT:
        wp = wmpt.parse_table(t)
        ok("wmpt.py accepts our activate-b table", not wp["problems"])
        wp2 = wmpt.parse_table(uf.build_table(3, 4, 2))  # activate a
        ok("wmpt.py accepts our activate-a table", not wp2["problems"])
        ok("wmpt.py selftest passes", wmpt.cmd_selftest(None) == 0)
    else:
        print("  (wmpt.py not importable; skipped its acceptance checks)")


# ---------------------------------------------------------------- activate plan
def test_activate_plan():
    print("== activate plan: gen ordering boot2 needs ==")
    # Current: stock, table @0x4000 gen2 active, appfw A gen3 / B gen2.
    t0 = uf.parse_table(uf.build_table(2, 3, 2))
    t1 = uf.parse_table(uf.build_table(1, 1, 2))

    off, tbl, ngen, (ga, gb) = uf.plan_activate(t0, t1, "b")
    pt = uf.parse_table(tbl)
    ok("activate b writes inactive sector 0x5000", off == 0x5000)
    ok("activate b table gen above max", ngen == 3)
    ok("activate b: appfw B gen above A", gb > ga)
    # After writing, boot2 must pick the new table and it must select B.
    new1 = pt
    ok("boot2 picks new table", uf.boot2_pick_table(t0, new1) == 1)
    ok("new table selects B", uf.table_selects_b(new1))
    ok("activate b sets trial flag", pt["entries"][uf.APPFW_B]["flag"] == ord("T"))
    if HAVE_WMPT:
        ok("wmpt.py validates activate-b table", not wmpt.parse_table(tbl)["problems"])

    # Recovery: custom running (table @0x5000 gen3, B active), activate a.
    t0 = uf.parse_table(uf.build_table(2, 3, 2))
    t1 = uf.parse_table(uf.build_table(3, 3, 4, flag_b=ord("T")))
    off, tbl, ngen, (ga, gb) = uf.plan_activate(t0, t1, "a")
    pt = uf.parse_table(tbl)
    ok("activate a writes inactive sector 0x4000", off == 0x4000)
    ok("activate a table gen above max", ngen == 4)
    ok("activate a: appfw A gen above B", ga > gb)
    ok("new table selects A", not uf.table_selects_b(pt))
    ok("activate a clears trial flags",
       pt["entries"][uf.APPFW_A]["flag"] == 0 and pt["entries"][uf.APPFW_B]["flag"] == 0)
    ok("boot2 picks the recovery table", uf.boot2_pick_table(t0, pt) == 1)
    if HAVE_WMPT:
        ok("wmpt.py validates activate-a table", not wmpt.parse_table(tbl)["problems"])


# ---------------------------------------------------------------- chunk plan
def test_chunk_plan():
    print("== chunk plan for the real firmware image ==")
    if not os.path.exists(FW_IMAGE):
        print(f"  (no {FW_IMAGE}; build with 'make -C firmware' - skipped)")
        return
    image = open(FW_IMAGE, "rb").read()
    info = uf.validate_mrvl(image)
    ok("purifier.mrvl is a valid MRVL image", True)
    ok("entry in XIP window", 0x1F000000 <= info["entry"] < 0x1F080000)
    ok("fits the slot", info["end"] <= 0x80000)

    for chunk in (0x1000, 0x4000, 0x10000):
        chunks = uf.plan_chunks(image, uf.SLOTB, chunk)
        # reconstruct the image from the (trimmed) chunk payloads
        rebuilt = b"".join(c["payload"][:c["data_len"]] for c in chunks)
        aligned = all(c["offset"] % 0x1000 == 0 for c in chunks)
        inslot = all(uf.SLOTB <= c["offset"] and
                     c["offset"] + len(c["payload"]) <= uf.SLOTB + 0x80000 for c in chunks)
        crcok = all(c["crc"] == ub.crc32_mw(c["payload"]) for c in chunks)
        ok(f"chunk {chunk:#06x}: rebuilds the image", rebuilt == image)
        ok(f"chunk {chunk:#06x}: all sector-aligned, in slot B", aligned and inslot)
        ok(f"chunk {chunk:#06x}: per-chunk CRCs correct", crcok)
        exp = (len(image) + chunk - 1) // chunk
        ok(f"chunk {chunk:#06x}: {exp} chunks", len(chunks) == exp)

    # default chunk keeps the ROM load at the proven 0x2000 bytes
    chunks = uf.plan_chunks(image, uf.SLOTB, uf.DEFAULT_CHUNK)
    img0 = uf.build_stub_image(b"\x00" * 0x800,
                               uf.make_params(uf.FW_OP_WRITE, chunks[0]["offset"],
                                              chunks[0]["data_len"], chunks[0]["crc"]),
                               chunks[0]["payload"])
    ok("default-chunk ROM load is 0x2000 bytes (proven size)", len(img0) == 0x2000)


# ---------------------------------------------------------------- simulated run
class SimFlash:
    """A W25Q16 model + a Python re-implementation of the stub's op handling,
    enough to drive uartboot_flash.Flasher end to end with no serial port.

    This is the Python mirror of flashwr_core.c; the authoritative dynamic
    test of the C core is test_flashwr.c.  Here we prove the HOST tool's
    session loop, report parsing, retries and verify step."""

    def __init__(self, image):
        self.d = bytearray(image)

    # -- the only interface Flasher needs is a Link; fake it minimally --
    def detect(self, timeout):
        return (b"\x01\x00\x41\x0a", 0xA3)

    def load(self, image, base, entry):
        self.params = image[uf.FW_PARAM_OFF:uf.FW_PARAM_OFF + 32]
        self.payload = image[uf.FW_PAYLOAD_OFF:]
        self._emit = self._run()

    def read_line(self, timeout):
        return self._emit.pop(0) if self._emit else None

    def read_exact(self, n, timeout):
        out = self._raw[:n]
        del self._raw[:n]
        return bytes(out)

    # -- the stub logic --
    def _run(self):
        self._raw = bytearray()
        lines = [b"MWFLASHWR 1\r\n"]
        p = self.params
        magic, ver, op, off, ln, crc, flags, pcrc = struct.unpack("<8I", p)
        if magic != uf.FW_PARAM_MAGIC or pcrc != ub.crc32_mw(p[:28]):
            return lines + [b"REFUSED 1\r\n", b"RESET\r\n"]
        rc = uf.host_check_range(op, off, ln)  # same constants as the stub
        if rc:
            return lines + [f"REFUSED {rc}\r\n".encode(), b"RESET\r\n"]
        if op == uf.FW_OP_READ:
            self._raw = bytearray(self.d[off:off + ln])
            c = 0
            for b in self._raw:
                c = ub.crc32_mw_update(c, bytes([b]))
            return lines + [f"BEGIN {ln}\r\n".encode(),
                            f"DONE CRC {c:08x}\r\n".encode(), b"RESET\r\n"]
        if op == uf.FW_OP_CRC:
            c = ub.crc32_mw(bytes(self.d[off:off + ln]))
            return lines + [f"CRC {off:08x} {ln:08x} {c:08x}\r\n".encode(), b"RESET\r\n"]
        # WRITE
        if ub.crc32_mw(self.payload[:ln]) != crc:
            return lines + [b"REFUSED 12\r\n", b"RESET\r\n"]
        pay = self.payload[:ln]
        table = off in (0x4000, 0x5000)
        t0 = uf.parse_table(self.d, 0x4000)
        t1 = uf.parse_table(self.d, 0x5000)
        active = uf.boot2_pick_table(t0, t1)
        if table:
            if uf.parse_table(pay).get("problems") or len(pay) != 0xD4:
                return lines + [b"REFUSED 6\r\n", b"RESET\r\n"]
            if off == (0x5000 if active else 0x4000):
                return lines + [b"REFUSED 7\r\n", b"RESET\r\n"]
            ng = struct.unpack_from("<I", pay, 8)[0]
            if ng <= max(t0.get("gen", 0) if t0["ok"] else 0,
                         t1.get("gen", 0) if t1["ok"] else 0):
                return lines + [b"REFUSED 8\r\n", b"RESET\r\n"]
        else:
            at = (t1 if active else t0)
            if not at["ok"]:
                return lines + [b"REFUSED 11\r\n", b"RESET\r\n"]
            if uf.table_selects_b(at):
                return lines + [b"REFUSED 10\r\n", b"RESET\r\n"]
        # erase+program the span
        span = (ln + 0xFFF) & ~0xFFF
        self.d[off:off + span] = b"\xff" * span
        self.d[off:off + ln] = pay
        got = ub.crc32_mw(bytes(self.d[off:off + ln]))
        tag = b"VERIFY OK" if got == crc else b"VERIFY FAIL"
        return lines + [f"{tag.decode()} {got:08x}\r\n".encode(), b"RESET\r\n"]

    def close(self):
        pass


def make_base_flash():
    d = bytearray(b"\xff" * 0x200000)
    d[0x6000:0xA000] = b"\x5a" * 0x4000            # PSM marker (no secrets)
    d[0x4000:0x4000 + 0xD4] = uf.build_table(2, 3, 2)
    d[0x5000:0x5000 + 0xD4] = uf.build_table(1, 1, 2)
    # valid MRVL in both slots
    img = make_mini_mrvl()
    d[0xA000:0xA000 + len(img)] = img
    d[0x8A000:0x8A000 + len(img)] = img
    return d


def make_mini_mrvl():
    off = 40
    body = bytes((i * 7 + 1) & 0xFF for i in range(0x80))
    laddr = 0x1F000000 + off
    h = struct.pack("<5I", 0x4C56524D, 0x2E9CF17B, 0x5D1208DD, 1, laddr | 1)
    s = struct.pack("<5I", 2, off, len(body), laddr, ub.crc32_mw(body))
    return h + s + body


def test_sim_session():
    print("== simulated ROM+flash through uartboot_flash.Flasher ==")
    base = make_base_flash()

    # write-slot-b: a small image, default chunk.
    img = make_mini_mrvl()
    sim = SimFlash(bytes(base))
    fl = uf.Flasher(sim, b"\x00" * 0x800)
    chunks = uf.plan_chunks(img, uf.SLOTB, uf.DEFAULT_CHUNK)
    for c in chunks:
        fl.op(uf.FW_OP_WRITE, c["offset"], c["data_len"], c["payload"], c["crc"])
    ok("sim: slot B holds the image", sim.d[0x8A000:0x8A000 + len(img)] == img)
    ok("sim: boot2/PSM/slotA untouched", sim.d[:0x8A000] == base[:0x8A000])
    rep, _ = fl.op(uf.FW_OP_CRC, uf.SLOTB, len(img))
    gotcrc = int(rep.value("CRC").split()[-1], 16)
    ok("sim: full-slot CRC matches", gotcrc == ub.crc32_mw(img))

    # forbidden range refused by the host guard before any session.
    try:
        uf.Flasher(SimFlash(bytes(base)), b"\x00" * 0x10).op(
            uf.FW_OP_WRITE, 0x6000, 0x1000, b"\x00" * 0x1000, ub.crc32_mw(b"\x00" * 0x1000))
        ok("sim: PSM write refused", False)
    except uf.FlashError as e:
        ok("sim: PSM write refused by host guard", "range" in str(e))

    # activate b then status.
    sim = SimFlash(bytes(base))
    fl = uf.Flasher(sim, b"\x00" * 0x10)
    t0, t1, _ = uf.read_both_tables(fl, 0)
    off, tbl, ngen, _ = uf.plan_activate(t0, t1, "b")
    fl.op(uf.FW_OP_WRITE, off, len(tbl), tbl, ub.crc32_mw(tbl))
    t0, t1, _ = uf.read_both_tables(fl, 0)
    ok("sim: after activate b, boot2 selects B",
       uf.table_selects_b((t0, t1)[uf.boot2_pick_table(t0, t1)]))

    # recovery: activate a restores stock selection.
    off, tbl, ngen, _ = uf.plan_activate(t0, t1, "a")
    fl.op(uf.FW_OP_WRITE, off, len(tbl), tbl, ub.crc32_mw(tbl))
    t0, t1, _ = uf.read_both_tables(fl, 0)
    ok("sim: after activate a, boot2 selects A",
       not uf.table_selects_b((t0, t1)[uf.boot2_pick_table(t0, t1)]))

    # activate into the active sector is refused by the stub logic.
    sim = SimFlash(bytes(base))
    fl = uf.Flasher(sim, b"\x00" * 0x10)
    tbl = uf.build_table(3, 3, 4, flag_b=ord("T"))
    try:
        fl.op(uf.FW_OP_WRITE, 0x4000, len(tbl), tbl, ub.crc32_mw(tbl))
        ok("sim: write active table refused", False)
    except uf.FlashError as e:
        ok("sim: write active table refused", "active-table" in str(e))


def main():
    uf.ub.self_test()
    test_header_sync()
    test_range_guard()
    test_tables()
    test_activate_plan()
    test_chunk_plan()
    test_sim_session()
    print()
    print("SOME TESTS FAILED" if fails else "all uartboot_flash offline tests passed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
