#!/usr/bin/env python3
"""mrvl_pack.py - ELF -> Marvell WMSDK "MRVL" appfw image (replaces axf2firmware).

Image format (evidence/fw-boot-ota-build.md section 2.2, firmware_structure.h):

  header, 20 bytes:  'MRVL', 0x2E9CF17B, timestamp, seg_cnt, entry
  seg_cnt x 20 bytes: type (2 = load), file offset, length, load addr, crc
  0xFF padding up to --header-size (0xC8 = room for 9 segment headers,
  matching `_text_offset` in the linker script and the stock image)
  segment data

CRC = CRC-32 reflected poly 0xEDB88320, init 0, NO final xor (not zlib.crc32).

XIP segments (load address in the flash window 0x1F000000..) execute in place,
so their file offset inside the slot must equal (addr - 0x1F000000).  All other
segments (SRAM0 0x001xxxxx, SRAM1 0x200xxxxx) are copied by boot2 and are
placed sequentially.  The packer refuses to produce an image where that
invariant cannot hold.

usage:
  mrvl_pack.py pack  IN.elf OUT.mrvl [--timestamp N] [--header-size 0xc8]
                     [--max-size 0x80000] [--compare STOCK.raw]
  mrvl_pack.py info  IMAGE.mrvl          # parse + verify every segment CRC
  mrvl_pack.py check IN.elf IMAGE.mrvl [--map X.map] [--stock-elf S.elf]
  mrvl_pack.py selftest                  # CRC algorithm known-answer test
"""
import argparse
import os
import struct
import sys
import time

MRVL_MAGIC = b"MRVL"
MRVL_SIG = 0x2E9CF17B
SEG_TYPE_LOAD = 2
XIP_BASE = 0x1F000000
SLOT_SIZE = 0x80000
DEFAULT_HDR = 0xC8

# Memory boot2 is executing from / using while it copies our SRAM segments.
# boot2 is loaded to SRAM0 0x0015C000 (entry 0x0015C001) and keeps buffers up
# to the top of SRAM0 (0x0015D8xx / 0x0015F000).  A load segment overlapping
# this would overwrite boot2 mid-copy.
BOOT2_RESERVED = (0x0015C000, 0x00160000)
# boot2 sets SP = 0x20020000 (code at boot2 0x15C074) and keeps its stack at
# the top of SRAM1 while it copies segments.  Its depth is unknown; the stock
# app's SRAM1 segment ends at 0x20010C6C and boots, so that is the proven
# limit for our SRAM1 load segments.
SRAM1_LOAD_LIMIT = 0x20010C6C
SRAM0 = (0x00100000, 0x00160000)
SRAM1 = (0x20000000, 0x20020000)

_T = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ 0xEDB88320 if _c & 1 else _c >> 1
    _T.append(_c)


def crc32_ieee(data, crc=0):
    """Reflected 0xEDB88320, init 0, no final xor (boot2 HW CRC engine)."""
    t = _T
    for b in data:
        crc = t[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc & 0xFFFFFFFF


def read_elf_load_segments(path):
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
        sys.exit(f"{path}: not a little-endian ELF32 file")
    (e_type, e_machine, _ver, e_entry, e_phoff, _shoff, _flags, _ehsize,
     e_phentsize, e_phnum) = struct.unpack_from("<HHIIIIIHHH", d, 16)
    if e_machine != 40:
        sys.exit(f"{path}: not an ARM ELF (e_machine={e_machine})")
    segs = []
    for i in range(e_phnum):
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = \
            struct.unpack_from("<8I", d, e_phoff + i * e_phentsize)
        if p_type != 1 or p_filesz == 0:      # PT_LOAD with file contents only
            continue
        # Use the physical (load) address: boot2 copies to laddr; for XIP it
        # is the run address.  Our linker script has LMA == VMA everywhere.
        segs.append({"laddr": p_paddr, "vaddr": p_vaddr,
                     "data": d[p_offset:p_offset + p_filesz],
                     "memsz": p_memsz})
    return e_entry, segs


def is_xip(addr):
    return XIP_BASE <= addr < XIP_BASE + 0x01000000


def overlaps(a0, a1, b0, b1):
    return a0 < b1 and b0 < a1


def build_image(entry, segs, timestamp, header_size, max_size):
    n = len(segs)
    if n == 0:
        sys.exit("no loadable segments")
    if 20 + 20 * n > header_size:
        sys.exit(f"{n} segments do not fit in a {header_size:#x}-byte header")
    errors = []
    pos = header_size
    layout = []
    for s in segs:
        a, ln = s["laddr"], len(s["data"])
        if s["laddr"] != s["vaddr"]:
            errors.append(f"segment @{a:#010x}: LMA != VMA ({s['vaddr']:#010x}); "
                          "boot2 loads to LMA and nothing relocates")
        if is_xip(a):
            want = a - XIP_BASE
            if want < pos:
                errors.append(f"XIP segment @{a:#010x} needs file offset {want:#x} "
                              f"but the image is already {pos:#x} bytes long")
                want = pos
            off = want
        else:
            off = pos
            if not (overlaps(a, a + ln, *SRAM0) or overlaps(a, a + ln, *SRAM1)):
                errors.append(f"segment @{a:#010x} len {ln:#x} is not in SRAM0/SRAM1/XIP")
            if overlaps(a, a + ln, *SRAM1) and a + ln > SRAM1_LOAD_LIMIT:
                errors.append(f"SRAM1 segment ends at {a + ln:#x}, beyond {SRAM1_LOAD_LIMIT:#x} "
                              "(stock maximum; boot2's stack is at the top of SRAM1)")
            if overlaps(a, a + ln, *BOOT2_RESERVED):
                errors.append(f"segment @{a:#010x}..{a + ln:#010x} overlaps boot2 "
                              f"region {BOOT2_RESERVED[0]:#x}..{BOOT2_RESERVED[1]:#x}")
        layout.append((off, s))
        pos = off + ln
    # Overlap check between segments' load ranges.
    for i, (_, s1) in enumerate(layout):
        for _, s2 in layout[i + 1:]:
            if overlaps(s1["laddr"], s1["laddr"] + len(s1["data"]),
                        s2["laddr"], s2["laddr"] + len(s2["data"])):
                errors.append(f"segments @{s1['laddr']:#x} and @{s2['laddr']:#x} overlap")
    if not (entry & 1):
        errors.append(f"entry {entry:#010x} is not a Thumb address (bit 0 clear)")
    if not is_xip(entry) and not any(
            s["laddr"] <= (entry & ~1) < s["laddr"] + len(s["data"]) for s in segs):
        errors.append(f"entry {entry:#010x} is not inside any segment")
    if pos > max_size:
        errors.append(f"image is {pos:#x} bytes, larger than the slot ({max_size:#x})")
    if errors:
        sys.exit("mrvl_pack: refusing to build:\n  " + "\n  ".join(errors))

    img = bytearray(b"\xff" * pos)
    struct.pack_into("<4sIIII", img, 0, MRVL_MAGIC, MRVL_SIG, timestamp, n, entry)
    for i, (off, s) in enumerate(layout):
        crc = crc32_ieee(s["data"])
        struct.pack_into("<5I", img, 20 + 20 * i, SEG_TYPE_LOAD, off, len(s["data"]),
                         s["laddr"], crc)
        img[off:off + len(s["data"])] = s["data"]
    return bytes(img)


def parse_image(d):
    magic, sig, ts, n, entry = struct.unpack_from("<4sIIII", d, 0)
    if magic != MRVL_MAGIC or sig != MRVL_SIG:
        raise ValueError("not an MRVL image")
    if not 0 < n <= 9:
        raise ValueError(f"implausible segment count {n}")
    segs = []
    for i in range(n):
        typ, off, ln, laddr, crc = struct.unpack_from("<5I", d, 20 + 20 * i)
        segs.append({"type": typ, "off": off, "len": ln, "laddr": laddr, "crc": crc,
                     "crc_ok": off + ln <= len(d) and crc32_ieee(d[off:off + ln]) == crc})
    return {"ts": ts, "entry": entry, "segs": segs}


def describe(d, label):
    info = parse_image(d)
    end = max(s["off"] + s["len"] for s in info["segs"])
    print(f"{label}: entry {info['entry']:#010x}, {len(info['segs'])} segments, "
          f"timestamp {info['ts']:#x} "
          f"({time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime(info['ts']))} UTC), "
          f"image end {end:#x} ({end} bytes, {100.0 * end / SLOT_SIZE:.1f}% of slot)")
    ok = True
    for i, s in enumerate(info["segs"]):
        print(f"  seg{i}: type {s['type']} file+{s['off']:#07x} len {s['len']:#07x} "
              f"-> {s['laddr']:#010x} crc {s['crc']:#010x} "
              f"{'OK' if s['crc_ok'] else 'BAD'}")
        ok &= s["crc_ok"] and s["type"] == SEG_TYPE_LOAD
    return info, ok


def cmd_pack(a):
    entry, segs = read_elf_load_segments(a.elf)
    if a.timestamp is not None:
        ts = a.timestamp
    elif os.environ.get("SOURCE_DATE_EPOCH"):
        ts = int(os.environ["SOURCE_DATE_EPOCH"])
    else:
        ts = int(time.time())
    img = build_image(entry, segs, ts & 0xFFFFFFFF, a.header_size, a.max_size)
    with open(a.out, "wb") as f:
        f.write(img)
    _, ok = describe(img, a.out)
    if not ok:
        sys.exit("self-check failed")
    if a.compare:
        ref = open(a.compare, "rb").read()
        rinfo, rok = describe(ref, a.compare)
        mine = parse_image(img)
        same_hdr = all(
            (s["type"], s["off"], s["len"], s["laddr"], s["crc"]) ==
            (r["type"], r["off"], r["len"], r["laddr"], r["crc"])
            for s, r in zip(mine["segs"], rinfo["segs"])) and \
            len(mine["segs"]) == len(rinfo["segs"]) and mine["entry"] == rinfo["entry"]
        # Byte-compare everything except the 4-byte timestamp at offset 8.
        body_same = img[:8] == ref[:8] and img[12:] == ref[12:len(img)]
        tail = ref[len(img):]
        tail_ff = all(b == 0xFF for b in tail)
        print(f"compare: segment table+CRCs {'IDENTICAL' if same_hdr else 'DIFFER'}; "
              f"image bytes (excluding timestamp) {'IDENTICAL' if body_same else 'DIFFER'}; "
              f"reference tail after image {len(tail):#x} bytes "
              f"{'all 0xFF' if tail_ff else 'NOT all 0xFF'}")
        if not (same_hdr and body_same and rok):
            sys.exit(1)


def cmd_info(a):
    d = open(a.image, "rb").read()
    _, ok = describe(d, a.image)
    sys.exit(0 if ok else 1)


def read_elf_symbols(path):
    """name -> (value, size, section index) for FUNC/OBJECT/NOTYPE symbols."""
    d = open(path, "rb").read()
    e_shoff, = struct.unpack_from("<I", d, 32)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", d, 46)
    shdrs = [struct.unpack_from("<10I", d, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    syms = {}
    for sh in shdrs:
        if sh[1] != 2:                       # SHT_SYMTAB
            continue
        strtab = shdrs[sh[6]]
        for off in range(sh[4], sh[4] + sh[5], 16):
            st_name, st_value, st_size, st_info, _o, st_shndx = struct.unpack_from("<IIIBBH", d, off)
            if st_shndx == 0:
                continue
            s = d[strtab[4] + st_name:d.index(b"\0", strtab[4] + st_name)].decode()
            if s and not s.startswith("$"):
                syms.setdefault(s, (st_value, st_size, st_shndx))
    return syms


def cmd_check(a):
    """Static boot-readiness checks of a built image (no hardware)."""
    ok = True

    def res(cond, msg):
        nonlocal ok
        print(f"  [{'ok' if cond else 'FAIL'}] {msg}")
        ok &= bool(cond)

    img = open(a.image, "rb").read()
    info = parse_image(img)
    segs = info["segs"]
    syms = read_elf_symbols(a.elf)
    entry_elf, _ = read_elf_load_segments(a.elf)
    print(f"check {a.image} against {a.elf}")
    end = max(s["off"] + s["len"] for s in segs)
    res(all(s["crc_ok"] for s in segs), "all segment CRCs verify (crc32 0xEDB88320, init 0, no xor)")
    res(end <= SLOT_SIZE, f"image {end:#x} bytes <= appfw slot {SLOT_SIZE:#x} ({100.0 * end / SLOT_SIZE:.1f}%)")
    kinds = []
    for s in segs:
        a0, a1 = s["laddr"], s["laddr"] + s["len"]
        if is_xip(a0):
            kinds.append("XIP")
            res(s["off"] == a0 - XIP_BASE, f"XIP seg {a0:#x}: file offset {s['off']:#x} == addr-0x1F000000")
        elif overlaps(a0, a1, *SRAM0):
            kinds.append("SRAM0")
            res(not overlaps(a0, a1, *BOOT2_RESERVED),
                f"SRAM0 seg {a0:#x}..{a1:#x} clear of boot2 {BOOT2_RESERVED[0]:#x}..")
        elif overlaps(a0, a1, *SRAM1):
            kinds.append("SRAM1")
            res(a1 <= SRAM1_LOAD_LIMIT,
                f"SRAM1 seg {a0:#x}..{a1:#x} ends below {SRAM1_LOAD_LIMIT:#x} (boot2 stack at SRAM1 top)")
        else:
            kinds.append("?")
    res(kinds == ["SRAM0", "XIP", "SRAM1"], f"segment order {kinds} == stock [SRAM0, XIP, SRAM1]")
    res(segs[0]["laddr"] == 0x00100000 and segs[0]["off"] == DEFAULT_HDR,
        "seg0 = vectors at SRAM0 0x00100000, file offset 0xC8 (as stock)")
    res(info["entry"] == entry_elf, f"header entry {info['entry']:#x} == ELF entry")
    rst = syms.get("Reset_IRQHandler")
    res(rst and (rst[0] | 1) == info["entry"], "entry == Reset_IRQHandler | 1 (Thumb)")
    res(is_xip(info["entry"]) and info["entry"] & 1, "entry is a Thumb address in the XIP window (as stock)")

    # Vector table (seg0 start).
    v = struct.unpack_from("<16I", img, segs[0]["off"])
    res(v[0] == 0x20020000, f"initial SP {v[0]:#x} == 0x20020000 (stock value)")
    res(v[1] == info["entry"], f"reset vector {v[1]:#x} == entry")
    code_ranges = [(s["laddr"], s["laddr"] + s["len"]) for s in segs[:2]]
    vt = syms.get("__vector_table")
    nvec = vt[1] // 4 if vt and vt[1] else 16 + 62
    vecs = struct.unpack_from(f"<{nvec}I", img, segs[0]["off"])
    bad = [i for i, x in enumerate(vecs[1:], 1)
           if x and not (x & 1 and any(r0 <= (x & ~1) < r1 for r0, r1 in code_ranges))]
    res(not bad, f"all {nvec - 1} non-zero vectors are Thumb addresses inside .init/.text"
        + (f" (bad: {bad[:8]})" if bad else ""))
    for name in ("HardFault_IRQHandler", "Default_IRQHandler"):
        s = syms.get(name)
        res(s and SRAM0[0] <= s[0] < SRAM0[1], f"{name} runs from SRAM0 (stock: 0x001002F9 / 0x0010013D)")

    # Code that must not execute from XIP (flash program/erase, QSPI, clock/PMU switching).
    must_ram = ["FLASH_Read", "FLASH_Write", "FLASH_Erase", "FLASH_WriteEnable", "QSPI_Init",
                "QSPI_StartTransfer", "CLK_ModuleClkDivider", "CLK_GPTInternalClkSrc",
                "CLK_SystemClkSrc", "PMU_PowerOnVDDIO"]
    for name in must_ram:
        s = syms.get(name)
        if s is None:
            print(f"  [--] {name} not linked")
            continue
        res(SRAM0[0] <= s[0] < SRAM0[1], f"{name} @{s[0]:#x} in SRAM0")
    # Stock evidence: CLK_ModuleClkDivider / CLK_GPTInternalClkSrc are RAM code in appfwA too
    # (fw-hardware-map.md Appendix A: "->RAM 0010278c", "->RAM 00102024").

    # Linker-provided memory map.
    for nm in ("_heap_start", "_heap_end", "_heap_2_start", "_heap_2_end",
               "_bss_sram0", "_ebss_sram0", "_bss_sram1", "_ebss_sram1", "_estack"):
        if nm not in syms:
            res(False, f"symbol {nm} present")
    if all(n in syms for n in ("_heap_start", "_heap_end", "_heap_2_start", "_heap_2_end")):
        h0 = syms["_heap_end"][0] - syms["_heap_start"][0]
        h1 = syms["_heap_2_end"][0] - syms["_heap_2_start"][0]
        print(f"  [..] heap SRAM0 {syms['_heap_start'][0]:#x}..{syms['_heap_end'][0]:#x} = {h0} B, "
              f"SRAM1 {syms['_heap_2_start'][0]:#x}..{syms['_heap_2_end'][0]:#x} = {h1} B, "
              f"total {h0 + h1} B")
        res(h0 + h1 >= 72 * 1024, "heap >= 72 KiB (SDK minimum)")

    # Radio firmware: either the stock wififw partition (wm_wlan_init) or
    # the blob embedded in our image (wlfw_embedded + wlan_init_embedded).
    res("wm_wlan_init" in syms or "wlfw_embedded" in syms,
        "radio firmware loader linked (partition or embedded)")
    if "wlfw_embedded" in syms:
        addr = syms["wlfw_embedded"][0]
        res(0x1F000000 <= addr < 0x1F080000 and addr % 4 == 0,
            f"embedded WLFW blob in the XIP segment at {addr:#x}")
    for name in ("main", "os_init", "app_start", "wlan_init", "wlan_start",
                 "part_init", "FreeRTOS_IPInit", "xNetworkInterfaceOutput", "handle_data_packet",
                 "vTaskStartScheduler", "PendSV_IRQHandler", "SVC_IRQHandler", "SysTick_IRQHandler",
                 "netio_tcp_listen", "netio_udp_bind", "wmstdio_init", "board_gpio_power_on"):
        res(name in syms, f"symbol {name} linked")
    # FreeRTOS port handlers must be the real ones, not the Default_IRQHandler alias.
    dflt = syms.get("Default_IRQHandler", (None,))[0]
    for i, name in ((11, "SVC"), (14, "PendSV"), (15, "SysTick")):
        res((vecs[i] & ~1) != dflt, f"vector {i} ({name}) is the FreeRTOS port handler, not Default")

    if a.stock_elf:
        _, ssegs = read_elf_load_segments(a.stock_elf)
        print("  stock vs ours (load segments):")
        for i, (st, my) in enumerate(zip(ssegs, segs)):
            print(f"    seg{i}: stock {st['laddr']:#010x} len {len(st['data']):#08x}   "
                  f"ours {my['laddr']:#010x} len {my['len']:#08x}")
        sv = struct.unpack_from(f"<{nvec}I", ssegs[0]["data"], 0)
        sdef = sv[2]
        print(f"    vectors equal to Default_IRQHandler: stock {sum(1 for x in sv[16:] if x == sdef)}"
              f"/{nvec - 16} IRQs, ours {sum(1 for x in vecs[16:] if x == (dflt | 1))}/{nvec - 16}")
    if a.map:
        lines = open(a.map).read().splitlines()
        # Only the memory-map part (after "Linker script and memory map").
        start = next((i for i, l in enumerate(lines) if l.startswith("Linker script and memory map")), 0)
        placed = []                      # (section name, address, size, object)
        i = start
        while i < len(lines):
            t = lines[i].split()
            if lines[i].startswith(" .") and t:
                name = t[0]
                if len(t) == 1 and i + 1 < len(lines):      # long name: rest on next line
                    t = [name] + lines[i + 1].split()
                    i += 1
                if len(t) >= 4 and t[1].startswith("0x") and t[2].startswith("0x"):
                    placed.append((name, int(t[1], 16), int(t[2], 16), t[3]))
            i += 1
        for obj in ("mw300_flash.o", "mw300_qspi.o", "mw300_pmu.o", "mw300_clock.o", "mdev_pm.o"):
            hits = [p for p in placed if p[3].endswith("(" + obj + ")") and p[2] > 0
                    and (p[0].startswith(".text") or p[0].startswith(".rodata"))]
            in_xip = [p for p in hits if is_xip(p[1])]
            res(hits and not in_xip,
                f"map: {len(hits)} code/rodata sections of {obj} all in SRAM0"
                + (f" (XIP: {[p[0] for p in in_xip][:4]})" if in_xip else ""))
    print("check: " + ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)


def cmd_selftest(_a):
    # crc32_ieee with init 0/no xor relates to zlib's CRC-32 by the standard
    # identity; check a fixed vector and zlib consistency.
    import zlib
    v = b"123456789"
    # zlib.crc32(v) == crc32_ieee(v, 0xFFFFFFFF) ^ 0xFFFFFFFF
    assert crc32_ieee(v, 0xFFFFFFFF) ^ 0xFFFFFFFF == zlib.crc32(v) == 0xCBF43926
    assert crc32_ieee(b"") == 0
    print(f"selftest OK; crc32_ieee('123456789') = {crc32_ieee(v):#010x}")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    pk = sub.add_parser("pack")
    pk.add_argument("elf")
    pk.add_argument("out")
    pk.add_argument("--timestamp", type=lambda s: int(s, 0))
    pk.add_argument("--header-size", type=lambda s: int(s, 0), default=DEFAULT_HDR)
    pk.add_argument("--max-size", type=lambda s: int(s, 0), default=SLOT_SIZE)
    pk.add_argument("--compare", help="reference MRVL image (e.g. stock appfwA.raw)")
    pk.set_defaults(fn=cmd_pack)
    inf = sub.add_parser("info")
    inf.add_argument("image")
    inf.set_defaults(fn=cmd_info)
    ck = sub.add_parser("check")
    ck.add_argument("elf")
    ck.add_argument("image")
    ck.add_argument("--map")
    ck.add_argument("--stock-elf")
    ck.set_defaults(fn=cmd_check)
    st = sub.add_parser("selftest")
    st.set_defaults(fn=cmd_selftest)
    a = p.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
