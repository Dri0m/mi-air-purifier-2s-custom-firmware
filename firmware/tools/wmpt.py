#!/usr/bin/env python3
"""wmpt.py - Marvell WMSDK "WMPT" partition table tool (88MW300, Mi Air Purifier 2S).

Format (evidence/fw-boot-ota-build.md section 2.1, sdk partition.h):

  header, 16 bytes:  u32 magic 'WMPT' | u16 version=1 | u16 n_entries
                     | u32 gen_level | u32 crc32_ieee(header[0:12])
  n x 24 bytes:      u8 type | u8 device | char name[8] | u8 pad[2]
                     | u32 start | u32 size | u32 gen_level
  trailer, 4 bytes:  u32 crc32_ieee(entries)   (== crc over header+entries)

crc32_ieee = reflected 0xEDB88320, init 0, NO final xor (not zlib.crc32).

Tables live at flash 0x4000 (primary) and 0x5000 (backup), one 4 KiB sector each.
boot2 (disassembly 0x3ee/0x46e): a table whose 16-byte header cannot be read
or whose magic != 'WMPT' is treated as gen 0; the backup wins only if its gen
is strictly higher.  boot2 does NOT check either table CRC.  Inside the chosen
table it takes the first two type==1 entries and boots the second one only if
its gen is strictly higher (GPIO43 low at reset also forces the second one).

Our OTA keeps a boot-confirm flag in pad[0] of the appfw entries (boot2 and
libpart ignore the padding): 0x00 normal, 'T' trial (not booted yet),
'B' trial booted (previous slot re-activated until ota_mark_good()).

usage:
  wmpt.py show FILE              # raw table or 2 MiB flash image
  wmpt.py validate FILE          # exit 1 unless consistent (CI / tests)
  wmpt.py gen OUT [--gen N] [--gen-a 3] [--gen-b 2] [--entry T:NAME:START:SIZE:GEN ...]
  wmpt.py mkflash OUT --appfw-a A.mrvl --appfw-b B.mrvl [--table-gen 2 --gen-a 3 --gen-b 2]
  wmpt.py selftest
"""
import argparse
import struct
import sys

MAGIC = 0x54504D57          # 'WMPT'
VERSION = 1
HDR = 16
ENT = 24
MAX_ENTRIES = 16            # libpart truncates above this
TABLE_ADDRS = (0x4000, 0x5000)
SECTOR = 0x1000
FLASH_SIZE = 0x200000

MRVL_MAGIC = 0x4C56524D
MRVL_SIG = 0x2E9CF17B
XIP_BASE = 0x1F000000

TYPE_NAMES = {0: "boot2", 1: "appfw", 2: "wlanfw", 3: "ftfs", 4: "psm",
              5: "user", 6: "btfw"}
FLAG_NAMES = {0x00: "", 0x54: "TRIAL(not booted)", 0x42: "TRIAL(booted, awaiting confirm)"}

# Stock layout of this unit (fw-boot-ota-build.md 2.1).
STOCK_LAYOUT = [
    (0, "boot2", 0x000000, 0x06000, 1),
    (4, "psm", 0x006000, 0x04000, 1),
    (1, "appfw", 0x00A000, 0x80000, 3),
    (1, "appfw", 0x08A000, 0x80000, 2),
    (2, "wififw", 0x10A000, 0x30000, 1),
    (2, "wififw", 0x13A000, 0x30000, 1),
    (5, "mcufw", 0x16A000, 0x60000, 1),
    (6, "userdata", 0x1CA000, 0x36000, 1),
]

_T = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ 0xEDB88320 if _c & 1 else _c >> 1
    _T.append(_c)


def crc32_ieee(data, crc=0):
    t = _T
    for b in data:
        crc = t[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc & 0xFFFFFFFF


# ---------------------------------------------------------------- tables

def build_table(entries, gen, version=VERSION):
    """entries: list of (type, name, start, size, gen[, flag]) -> bytes."""
    hdr = struct.pack("<IHHI", MAGIC, version, len(entries), gen)
    hdr += struct.pack("<I", crc32_ieee(hdr))
    body = b""
    for e in entries:
        typ, name, start, size, egen = e[:5]
        flag = e[5] if len(e) > 5 else 0
        nb = name.encode()[:8].ljust(8, b"\0")
        body += struct.pack("<BB8sBBIII", typ, 0, nb, flag, 0, start, size, egen)
    return hdr + body + struct.pack("<I", crc32_ieee(body))


def parse_table(buf, off=0):
    """Return dict with fields + list of problems (empty = fully valid)."""
    t = {"problems": [], "entries": [], "magic_ok": False}
    if len(buf) < off + HDR:
        t["problems"].append("short read")
        return t
    magic, ver, n, gen, hcrc = struct.unpack_from("<IHHII", buf, off)
    t.update(magic=magic, version=ver, n=n, gen=gen, hcrc=hcrc)
    if magic != MAGIC:
        t["problems"].append(f"bad magic {magic:#010x}")
        return t
    t["magic_ok"] = True
    if ver != VERSION:
        t["problems"].append(f"version {ver} != 1")
    calc = crc32_ieee(buf[off:off + 12])
    t["hcrc_calc"] = calc
    if calc != hcrc:
        t["problems"].append(f"header crc {hcrc:#010x} != computed {calc:#010x}")
    if n == 0 or n > MAX_ENTRIES:
        t["problems"].append(f"entry count {n} out of range 1..{MAX_ENTRIES}")
        return t
    end = off + HDR + ENT * n
    if len(buf) < end + 4:
        t["problems"].append("table truncated")
        return t
    body = buf[off + HDR:end]
    (tcrc,) = struct.unpack_from("<I", buf, end)
    t["tcrc"] = tcrc
    t["tcrc_calc"] = crc32_ieee(body)
    if t["tcrc_calc"] != tcrc:
        t["problems"].append(f"trailer crc {tcrc:#010x} != computed {t['tcrc_calc']:#010x}")
    if crc32_ieee(buf[off:end + 4]) != 0 and not t["problems"]:
        t["problems"].append("residue check failed")
    for i in range(n):
        typ, dev, name, p0, p1, start, size, egen = struct.unpack_from(
            "<BB8sBBIII", buf, off + HDR + ENT * i)
        t["entries"].append({"type": typ, "dev": dev,
                             "name": name.split(b"\0")[0].decode("ascii", "replace"),
                             "pad": (p0, p1), "start": start, "size": size, "gen": egen})
    t["size"] = HDR + ENT * n + 4
    return t


def boot2_pick_table(t0, t1):
    """boot2 0x46e: invalid header -> gen 0; backup only if strictly higher."""
    g0 = t0["gen"] if t0["magic_ok"] else 0
    g1 = t1["gen"] if t1["magic_ok"] else 0
    return 1 if g1 > g0 else 0


def appfw_pair(t):
    """First two type==1 entries (boot2 does not look at the name)."""
    idx = [i for i, e in enumerate(t["entries"]) if e["type"] == 1][:2]
    return idx


def boot2_pick_appfw(t):
    idx = appfw_pair(t)
    if len(idx) < 2:
        return idx[0] if idx else None, idx
    a, b = (t["entries"][i] for i in idx)
    return (idx[1] if b["gen"] > a["gen"] else idx[0]), idx


# ---------------------------------------------------------------- MRVL images

def verify_mrvl(img, slot_size=0x80000):
    """Return (ok, text).  img = bytes starting at the slot start."""
    if len(img) < 20:
        return False, "short"
    magic, sig, ts, n, entry = struct.unpack_from("<5I", img, 0)
    if magic != MRVL_MAGIC:
        return False, f"no MRVL magic ({magic:#010x})"
    if sig != MRVL_SIG:
        return False, f"bad sig {sig:#010x}"
    if not 1 <= n <= 9:
        return False, f"seg_cnt {n}"
    out = [f"time={ts} segs={n} entry={entry:#010x}"]
    ok = True
    end = 0
    for i in range(n):
        typ, off, ln, la, crc = struct.unpack_from("<5I", img, 20 + 20 * i)
        if off + ln > min(len(img), slot_size):
            return False, f"seg{i} beyond slot"
        c = crc32_ieee(img[off:off + ln])
        good = c == crc
        ok &= good and typ == 2
        out.append(f"seg{i} type={typ} off={off:#x} len={ln:#x} laddr={la:#010x} "
                   f"crc={crc:#010x} {'OK' if good else f'BAD (calc {c:#010x})'}")
        end = max(end, off + ln)
    out.append(f"image end {end:#x}")
    return ok, "\n      ".join(out)


# ---------------------------------------------------------------- reporting

def describe_table(t, label):
    lines = []
    if not t["magic_ok"]:
        lines.append(f"{label}: no valid table ({'; '.join(t['problems'])}) -> boot2 gen 0")
        return lines
    st = "VALID" if not t["problems"] else "INVALID: " + "; ".join(t["problems"])
    lines.append(f"{label}: gen {t['gen']}, {t['n']} entries, hcrc {t['hcrc']:#010x}, "
                 f"tcrc {t.get('tcrc', 0):#010x}  [{st}]")
    for i, e in enumerate(t["entries"]):
        flag = e["pad"][0]
        fl = FLAG_NAMES.get(flag, f"pad={flag:#04x}") if flag or e["pad"][1] else ""
        if e["pad"][1]:
            fl += " confirmed" if e["pad"][1] == 0x43 else f" pad1={e['pad'][1]:#04x}"
        lines.append(f"   [{i}] type {e['type']} ({TYPE_NAMES.get(e['type'], '?'):6s}) "
                     f"{e['name']:9s} start {e['start']:#08x} size {e['size']:#07x} "
                     f"gen {e['gen']} {fl}")
    return lines


def analyse_flash(d, verbose=True):
    """Return (ok, lines) for a full flash image."""
    lines = []
    ok = True
    ts = [parse_table(d, a) for a in TABLE_ADDRS]
    for a, t in zip(TABLE_ADDRS, ts):
        lines += describe_table(t, f"table @{a:#06x}")
        if t["magic_ok"] and t["problems"]:
            ok = False      # a table boot2 might pick must be fully valid
        if t["magic_ok"]:
            n = t.get("size", HDR)
            tail = d[a + n:a + SECTOR]
            if tail.strip(b"\xff"):
                lines.append(f"   note: non-0xFF bytes after the table in sector @{a:#06x}")
    if not any(t["magic_ok"] for t in ts):
        return False, lines + ["no valid partition table -> boot2 cannot boot"]
    pick = boot2_pick_table(*ts)
    t = ts[pick]
    lines.append(f"boot2 picks table @{TABLE_ADDRS[pick]:#06x} (gen {t['gen']})")
    if t["problems"]:
        return False, lines
    act, idx = boot2_pick_appfw(t)
    if len(idx) < 2:
        return False, lines + ["fewer than two appfw (type 1) entries"]
    ea, eb = (t["entries"][i] for i in idx)
    for e in (ea, eb):
        if e["start"] % SECTOR or e["size"] % SECTOR or e["start"] + e["size"] > len(d):
            ok = False
            lines.append(f"appfw entry start/size invalid: {e}")
    if ea["start"] < eb["start"] + eb["size"] and eb["start"] < ea["start"] + ea["size"]:
        ok = False
        lines.append("appfw slots overlap")
    lines.append(f"boot2 boots appfw entry [{act}] @{t['entries'][act]['start']:#x} "
                 f"(gens {ea['gen']} / {eb['gen']}); GPIO43 low would force [{idx[1]}]")
    for i in idx:
        e = t["entries"][i]
        good, txt = verify_mrvl(d[e["start"]:e["start"] + e["size"]], e["size"])
        role = "ACTIVE " if i == act else "passive"
        lines.append(f"   {role} @{e['start']:#08x}: {'image OK' if good else 'image BAD'}: {txt}")
        if i == act and not good:
            lines.append("   WARNING: active slot image invalid; boot2 would fall back to the other slot")
            ok = False
    return ok, lines


def load(path):
    with open(path, "rb") as f:
        return f.read()


def cmd_show(a, validate=False):
    d = load(a.file)
    if len(d) >= 0x6000 and d[:4] != b"WMPT":
        ok, lines = analyse_flash(d)
    else:
        t = parse_table(d, 0)
        lines = describe_table(t, "table")
        ok = t["magic_ok"] and not t["problems"]
        if ok:
            act, idx = boot2_pick_appfw(t)
            if act is not None:
                lines.append(f"active appfw entry: [{act}]")
    if not validate or not ok:
        print("\n".join(lines))
    print("RESULT:", "OK" if ok else "INVALID")
    return 0 if ok else 1


def parse_int(s):
    return int(s, 0)


def layout_from_args(a):
    if a.entry:
        ents = []
        for s in a.entry:
            p = s.split(":")
            if len(p) not in (5, 6):
                sys.exit(f"bad --entry {s!r}, want TYPE:NAME:START:SIZE:GEN[:FLAG]")
            e = [parse_int(p[0]), p[1], parse_int(p[2]), parse_int(p[3]), parse_int(p[4])]
            if len(p) == 6:
                e.append(parse_int(p[5]))
            ents.append(tuple(e))
        return ents
    ents = [list(e) for e in STOCK_LAYOUT]
    ents[2][4] = a.gen_a
    ents[3][4] = a.gen_b
    return [tuple(e) for e in ents]


def cmd_gen(a):
    tbl = build_table(layout_from_args(a), a.gen)
    with open(a.out, "wb") as f:
        f.write(tbl)
    print(f"wrote {len(tbl):#x} bytes to {a.out}")
    return 0


def cmd_mkflash(a):
    d = bytearray(b"\xff" * FLASH_SIZE)
    # Placeholder patterns for regions we must never touch (tests hash them).
    d[0x0000:0x4000] = bytes([0xB2]) * 0x4000           # boot2 stand-in
    d[0x6000:0xA000] = bytes([0x5A]) * 0x4000           # PSM stand-in (no secrets)
    d[0x10A000:0x16A000] = bytes([0x3C]) * 0x60000      # wififw stand-ins
    ents = layout_from_args(a)
    t_new = build_table(ents, a.table_gen)
    t_old = build_table(ents, max(a.table_gen - 1, 0))
    d[0x4000:0x4000 + len(t_new)] = t_new
    if a.table_gen > 1:
        d[0x5000:0x5000 + len(t_old)] = t_old
    for path, addr in ((a.appfw_a, 0xA000), (a.appfw_b, 0x8A000)):
        if path:
            img = load(path)[:0x80000]
            d[addr:addr + len(img)] = img
    with open(a.out, "wb") as f:
        f.write(d)
    print(f"wrote {a.out}")
    return 0


def cmd_selftest(_a):
    assert crc32_ieee(b"123456789") == 0x2DFD2D88, hex(crc32_ieee(b"123456789"))
    t = build_table(STOCK_LAYOUT, 2)
    assert len(t) == 0xD4
    assert crc32_ieee(t) == 0, "residue over header+entries+trailer must be 0"
    p = parse_table(t)
    assert not p["problems"], p["problems"]
    assert boot2_pick_appfw(p)[0] == 2
    bad = bytearray(t)
    bad[0x30] ^= 1
    assert parse_table(bytes(bad))["problems"]
    print("selftest OK")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sp = ap.add_subparsers(dest="cmd", required=True)
    p = sp.add_parser("show")
    p.add_argument("file")
    p = sp.add_parser("validate")
    p.add_argument("file")
    for name in ("gen", "mkflash"):
        p = sp.add_parser(name)
        p.add_argument("out")
        p.add_argument("--gen-a", type=parse_int, default=3)
        p.add_argument("--gen-b", type=parse_int, default=2)
        p.add_argument("--entry", action="append")
        if name == "gen":
            p.add_argument("--gen", type=parse_int, default=2)
        else:
            p.add_argument("--table-gen", type=parse_int, default=2)
            p.add_argument("--appfw-a")
            p.add_argument("--appfw-b")
    sp.add_parser("selftest")
    a = ap.parse_args()
    if a.cmd == "show":
        return cmd_show(a)
    if a.cmd == "validate":
        return cmd_show(a, validate=True)
    if a.cmd == "gen":
        return cmd_gen(a)
    if a.cmd == "mkflash":
        return cmd_mkflash(a)
    return cmd_selftest(a)


if __name__ == "__main__":
    sys.exit(main())
