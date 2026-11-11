#!/usr/bin/env python3
"""Guarded flash writer for the 88MW300 (Mi Air Purifier 2S), over UART boot.

This installs our custom firmware into the PASSIVE appfw slot (B, 0x8A000) and
switches the partition table so boot2 picks it, keeping stock in slot A as an
untouched recovery image.  It drives the RAM stub mw300_flashwr (built from
flashwr_core.c + mw300_flashwr.c); every destructive guard is ALSO enforced
inside that stub, so this host tool can only ever talk to a stub that will
refuse a forbidden range by itself.

How it works
  Each chunk is one ROM session: the loader (uartboot_load.Link, which can
  only send 0x55 and 0x53) loads [stub | params@0xFC0 | payload@0x1000] into
  SRAM at 0x00110000, jumps, and reads the stub's bit-banged report.  The
  stub erases/programs/verifies the one range named in its parameter block,
  then AIRCR-resets back into the ROM, so the next chunk needs NO power cycle.

  write-slot-b:  split the MRVL image into sector-aligned chunks, write each
                 to slot B, then CRC-verify the whole slot from flash.
  activate b|a:  write a new WMPT table (gen above the current max, chosen
                 appfw gen above the other) into the INACTIVE table sector.
                 'activate a' is the recovery command (puts stock back).
  status:        read both table sectors (never the PSM) and print them.

Safety
  * The only bytes sent to the ROM are 0x55 and 0x53 (Link enforces it).
  * The stub refuses any range outside slot B or a single table sector, any
    unaligned erase, and has no chip/block erase or status-write opcode.
  * write-slot-b is refused while slot B is the active boot slot; activate
    into the active table sector, or with a non-increasing gen, or selecting
    a slot without a valid image, is refused.
  * The default chunk is 4 KB so the whole load stays at 0x2000 bytes, the
    size proven on hardware.  --chunk can raise it once a
    larger ROM load is confirmed on the bench.
"""
import argparse
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import uartboot_load as ub  # noqa: E402  (Link, crc32_mw, SerialTransport)

crc32_mw = ub.crc32_mw

# ---- constants, mirror of flashwr_core.h -------------------------------
LOAD_BASE = 0x00110000
FW_PARAM_OFF = 0x0FC0
FW_PAYLOAD_OFF = 0x1000
FW_PARAM_MAGIC = 0x52574C46
FW_PARAM_VERSION = 1
FW_OP_WRITE, FW_OP_CRC, FW_OP_READ = 1, 2, 3
FW_FLAG_DRYRUN = 1
FW_MAX_PAYLOAD = 0x10000
DEFAULT_CHUNK = 0x1000            # keeps the ROM load at the proven 0x2000

FLASH_SIZE = 0x200000
SECTOR = 0x1000
TABLE0, TABLE1 = 0x4000, 0x5000
TABLES_END = 0x6000
PSM_START, PSM_END = 0x6000, 0xA000
SLOTA, SLOTB = 0x0A000, 0x8A000
SLOT_SIZE = 0x80000
SLOTB_END = 0x10A000
WMPT_MAGIC = 0x54504D57
WMPT_N = 8
WMPT_LEN = 16 + 24 * WMPT_N + 4   # 0xD4
XIP_BASE = 0x1F000000
MRVL_MAGIC = 0x4C56524D
MRVL_SIG = 0x2E9CF17B

# Error codes, same order as the FW_E_* enum in flashwr_core.h.
FW_ERRORS = [
    "ok", "params", "op", "length", "unaligned", "range",
    "table-format", "active-table", "table-gen", "image",
    "slot-b-active", "layout", "payload-crc", "jedec",
    "protected", "wel", "timeout", "verify", "cmd",
]

# Stock layout of this unit (fw-boot-ota-build.md 2.1; matches both on-flash
# tables and flashwr_core.c's `layout[]`).
STOCK_LAYOUT = [
    (0, "boot2", 0x000000, 0x06000),
    (4, "psm", 0x006000, 0x04000),
    (1, "appfw", 0x00A000, 0x80000),
    (1, "appfw", 0x08A000, 0x80000),
    (2, "wififw", 0x10A000, 0x30000),
    (2, "wififw", 0x13A000, 0x30000),
    (5, "mcufw", 0x16A000, 0x60000),
    (6, "userdata", 0x1CA000, 0x36000),
]
APPFW_A, APPFW_B = 2, 3            # indices of the two appfw entries


class FlashError(RuntimeError):
    pass


# Error-code indices (match the FW_E_* enum order in flashwr_core.h).
(E_OK, E_PARAMS, E_OP, E_LEN, E_ALIGN, E_RANGE, E_TABLE, E_ACTIVE_TABLE,
 E_TABLE_GEN, E_IMAGE, E_SLOT_ACTIVE, E_LAYOUT, E_PAYLOAD_CRC, E_JEDEC,
 E_PROTECTED, E_WEL, E_TIMEOUT, E_VERIFY, E_CMD) = range(19)


def host_check_range(op, offset, length):
    """Port of flashwr_core.c fw_check_range().  Defence in depth: the host
    refuses the same ranges the stub would, before anything is sent.  Returns
    an FW_E_* index (0 = ok)."""
    if op not in (FW_OP_WRITE, FW_OP_CRC, FW_OP_READ):
        return E_OP
    if length == 0:
        return E_LEN
    if offset >= FLASH_SIZE or length > FLASH_SIZE - offset:
        return E_RANGE
    end = offset + length

    if op == FW_OP_READ:
        return E_OK if (offset >= TABLE0 and end <= TABLES_END) else E_RANGE
    if op == FW_OP_CRC:
        in_tables = TABLE0 <= offset and end <= TABLES_END
        in_slots = SLOTA <= offset and end <= SLOTB_END
        if not (in_tables or in_slots):
            return E_RANGE
        if offset < PSM_END and PSM_START < end:
            return E_RANGE
        return E_OK

    # WRITE
    if length > FW_MAX_PAYLOAD:
        return E_LEN
    if offset & (SECTOR - 1):
        return E_ALIGN
    span_end = offset + ((length + SECTOR - 1) & ~(SECTOR - 1))
    if offset in (TABLE0, TABLE1):
        if span_end != offset + SECTOR:
            return E_RANGE
    elif offset < SLOTB or span_end > SLOTB_END:
        return E_RANGE

    def overlaps(a, b, c, d):
        return a < d and c < b
    if (overlaps(offset, span_end, 0, TABLE0) or
            overlaps(offset, span_end, PSM_START, PSM_END) or
            overlaps(offset, span_end, SLOTA, SLOTB) or
            overlaps(offset, span_end, SLOTB_END, FLASH_SIZE)):
        return E_RANGE
    return E_OK


# ---- WMPT tables -------------------------------------------------------
def build_table(gen_level, gen_a, gen_b, flag_a=0, flag_b=0):
    """Build a stock-layout WMPT table.  Byte-for-byte what the stub's
    fw_check_table() accepts and what wmpt.py parses as valid."""
    hdr = struct.pack("<IHHI", WMPT_MAGIC, 1, WMPT_N, gen_level)
    hdr += struct.pack("<I", crc32_mw(hdr))
    body = b""
    for i, (typ, name, start, size) in enumerate(STOCK_LAYOUT):
        gen = {APPFW_A: gen_a, APPFW_B: gen_b}.get(i, 1)
        flag = {APPFW_A: flag_a, APPFW_B: flag_b}.get(i, 0)
        body += struct.pack("<BB8sBBIII", typ, 0,
                            name.encode().ljust(8, b"\0"), flag, 0,
                            start, size, gen)
    return hdr + body + struct.pack("<I", crc32_mw(body))


def parse_table(buf, off=0):
    """Minimal parser for status/validation.  Returns a dict; 'ok' True only
    when both CRCs and the stock layout check out."""
    t = {"ok": False, "problems": [], "entries": []}
    if len(buf) < off + WMPT_LEN:
        t["problems"].append("short")
        return t
    magic, ver, n, gen = struct.unpack_from("<IHHI", buf, off)
    t.update(magic=magic, version=ver, n=n, gen=gen)
    if magic != WMPT_MAGIC:
        t["problems"].append(f"magic {magic:#010x}")
        return t
    (hcrc,) = struct.unpack_from("<I", buf, off + 12)
    if crc32_mw(buf[off:off + 12]) != hcrc:
        t["problems"].append("header crc")
    if n != WMPT_N:
        t["problems"].append(f"n={n}")
        return t
    body = buf[off + 16:off + 16 + 24 * n]
    (tcrc,) = struct.unpack_from("<I", buf, off + 16 + 24 * n)
    if crc32_mw(body) != tcrc:
        t["problems"].append("trailer crc")
    for i in range(n):
        typ, dev, name, f0, f1, start, size, egen = struct.unpack_from(
            "<BB8sBBIII", buf, off + 16 + 24 * i)
        t["entries"].append(dict(type=typ, dev=dev,
                                 name=name.split(b"\0")[0].decode("ascii", "replace"),
                                 flag=f0, pad1=f1, start=start, size=size, gen=egen))
    t["ok"] = not t["problems"]
    return t


def boot2_pick_table(t0, t1):
    g0 = t0["gen"] if t0.get("magic") == WMPT_MAGIC else 0
    g1 = t1["gen"] if t1.get("magic") == WMPT_MAGIC else 0
    return 1 if g1 > g0 else 0


def table_selects_b(t):
    e = t["entries"]
    return e[APPFW_B]["gen"] > e[APPFW_A]["gen"]


# ---- MRVL image validation --------------------------------------------
def validate_mrvl(img, slot_size=SLOT_SIZE):
    if len(img) < 20:
        raise FlashError("image shorter than an MRVL header")
    magic, sig, ts, n, entry = struct.unpack_from("<5I", img, 0)
    if magic != MRVL_MAGIC:
        raise FlashError(f"not an MRVL image (magic {magic:#010x})")
    if sig != MRVL_SIG:
        raise FlashError(f"bad MRVL signature {sig:#010x}")
    if not 1 <= n <= 9:
        raise FlashError(f"implausible segment count {n}")
    if not (XIP_BASE <= entry < XIP_BASE + slot_size) or not (entry & 1):
        raise FlashError(f"entry {entry:#010x} not an odd address in the XIP window")
    segs = []
    end = 20 + 20 * n
    for i in range(n):
        typ, off, ln, la, crc = struct.unpack_from("<5I", img, 20 + 20 * i)
        if typ != 2:
            raise FlashError(f"seg{i} type {typ} != 2")
        if off < 20 + 20 * n or off + ln > min(len(img), slot_size):
            raise FlashError(f"seg{i} [{off:#x},{off + ln:#x}) outside the image/slot")
        if XIP_BASE <= la < XIP_BASE + slot_size and la - XIP_BASE != off:
            raise FlashError(f"seg{i} XIP laddr {la:#010x} != slot offset {off:#x}")
        if crc32_mw(img[off:off + ln]) != crc:
            raise FlashError(f"seg{i} CRC mismatch (stored {crc:#010x})")
        segs.append((typ, off, ln, la, crc))
        end = max(end, off + ln)
    if end > slot_size:
        raise FlashError(f"image end {end:#x} exceeds slot size {slot_size:#x}")
    return dict(time=ts, segs=segs, entry=entry, end=end)


# ---- chunk plan --------------------------------------------------------
def plan_chunks(image, slot_base=SLOTB, chunk=DEFAULT_CHUNK):
    """Split an image into sector-aligned, in-order chunks for slot writes.
    Returns a list of dicts: offset (flash), payload (the real bytes),
    data_len, crc (of the payload).  The stub erases ceil(len/4K) sectors and
    programs exactly data_len bytes; it leaves the rounded-up tail at 0xFF, so
    no padding is sent and the CRC is over the real bytes (== the stub's)."""
    if chunk % SECTOR or not 0 < chunk <= FW_MAX_PAYLOAD:
        raise FlashError(f"chunk {chunk:#x} must be a multiple of {SECTOR:#x}, <= {FW_MAX_PAYLOAD:#x}")
    chunks = []
    for pos in range(0, len(image), chunk):
        data = image[pos:pos + chunk]
        off = slot_base + pos
        span = (len(data) + SECTOR - 1) & ~(SECTOR - 1)
        if off + span > slot_base + SLOT_SIZE:
            raise FlashError("image does not fit in the slot")
        chunks.append(dict(offset=off, payload=data,
                           data_len=len(data), crc=crc32_mw(data)))
    return chunks


# ---- parameter block + stub image --------------------------------------
def make_params(op, offset, length, crc, flags=0):
    body = struct.pack("<7I", FW_PARAM_MAGIC, FW_PARAM_VERSION, op,
                       offset, length, crc, flags)
    return body + struct.pack("<I", crc32_mw(body))


def build_stub_image(stub_code, params, payload=b""):
    if len(stub_code) > FW_PARAM_OFF:
        raise FlashError(f"stub is {len(stub_code)} bytes; must be <= {FW_PARAM_OFF:#x}")
    if len(params) != 32:
        raise FlashError("params must be 32 bytes")
    if len(payload) > FW_MAX_PAYLOAD:
        raise FlashError(f"payload {len(payload)} > {FW_MAX_PAYLOAD:#x}")
    img = bytearray(FW_PAYLOAD_OFF + len(payload))
    img[:len(stub_code)] = stub_code
    img[FW_PARAM_OFF:FW_PARAM_OFF + 32] = params
    img[FW_PAYLOAD_OFF:FW_PAYLOAD_OFF + len(payload)] = payload
    return bytes(img)


# ---- report parsing ----------------------------------------------------
class Report:
    def __init__(self, lines):
        self.lines = lines
        self.text = "\n".join(lines)

    def _code(self, kw):
        for ln in self.lines:
            if ln.startswith(kw + " "):
                try:
                    return int(ln.split()[1])
                except (IndexError, ValueError):
                    return -1
        return None

    @property
    def refused(self):
        return self._code("REFUSED")

    @property
    def error(self):
        return self._code("ERROR")

    def value(self, kw):
        for ln in self.lines:
            if ln.startswith(kw + " "):
                return ln[len(kw) + 1:]
        return None


# ---- the serial session for one op -------------------------------------
class Flasher:
    def __init__(self, link, stub_code, base=LOAD_BASE,
                 detect_timeout=180.0, report_timeout=30.0):
        self.link = link
        self.stub = stub_code
        self.base = base
        self.detect_timeout = detect_timeout
        self.report_timeout = report_timeout

    def _one_session(self, params, payload, capture_len=0):
        """Load+jump; return (Report, captured_bytes).  One ROM session."""
        link = self.link
        result = link.detect(self.detect_timeout)
        if result is None:
            raise FlashError("no Detection ACK (wiring / BOOT straps / power)")
        version, ack = result
        if ack == 0xAC:
            raise FlashError("0xAC: OTP security on, ROM wants a password")
        image = build_stub_image(self.stub, params, payload)
        link.load(image, base=self.base, entry=self.base | 1)

        lines, raw = [], bytearray()
        begin = None
        captured = bytearray()
        end = time.monotonic() + self.report_timeout
        while time.monotonic() < end:
            line = link.read_line(max(0.2, end - time.monotonic()))
            if line is None:
                break
            if begin is not None:
                break  # handled below after we know capture_len
            text = line.decode("ascii", "replace").rstrip("\r\n")
            if text:
                lines.append(text)
                print(f"  stub: {text}")
            if text.startswith("BEGIN ") and capture_len:
                begin = int(text.split()[1])
                break
            if text in ("RESET",) or text.startswith(("REFUSED", "ERROR", "VERIFY", "DRYRUN", "CRC ")):
                # keep reading briefly for a trailing RESET
                if text == "RESET":
                    break
        if begin is not None:
            got = 0
            crc = 0
            while got < begin:
                chunk = link.read_exact(min(4096, begin - got), 30.0)
                if not chunk:
                    raise FlashError(f"read stopped after {got}/{begin}")
                captured += chunk
                crc = ub.crc32_mw_update(crc, chunk)
                got += len(chunk)
            # trailing DONE CRC + RESET
            for _ in range(4):
                line = link.read_line(5.0)
                if not line:
                    break
                text = line.decode("ascii", "replace").rstrip("\r\n")
                if text:
                    lines.append(text)
            rep = Report(lines)
            done = rep.value("DONE CRC")
            if done is not None and int(done, 16) != crc:
                raise FlashError(f"read CRC mismatch: stub {done}, host {crc:08x}")
            return rep, bytes(captured)
        return Report(lines), b""

    def op(self, op, offset, length, payload=b"", crc=0, flags=0,
           capture_len=0, retries=0):
        rc = host_check_range(op, offset, length)
        if rc != E_OK:
            raise FlashError(f"host guard refuses this range: {FW_ERRORS[rc]} "
                             f"(op {op}, {offset:#x}+{length:#x})")
        last = None
        for attempt in range(retries + 1):
            if attempt:
                print(f"  retry {attempt}/{retries} ...")
            params = make_params(op, offset, length, crc, flags)
            rep, data = self._one_session(params, payload, capture_len)
            if rep.refused is not None:
                name = FW_ERRORS[rep.refused] if 0 <= rep.refused < len(FW_ERRORS) else "?"
                raise FlashError(f"stub refused: {name} (code {rep.refused})")
            if rep.error is not None:
                name = FW_ERRORS[rep.error] if 0 <= rep.error < len(FW_ERRORS) else "?"
                last = FlashError(f"stub error: {name} (code {rep.error})")
                continue
            return rep, data
        raise last or FlashError("op failed")


# ---- subcommand logic (the planning parts are unit-tested offline) ------
def read_tables_plan():
    return dict(op=FW_OP_READ, offset=TABLE0, length=TABLES_END - TABLE0)


def plan_activate(cur_t0, cur_t1, target):
    """Given the two current tables (parsed), return (write_offset, table_bytes).
    Writes into the INACTIVE sector, gen above max, chosen slot gen above the
    other.  Raises if that would select a slot we can't confirm here."""
    if target not in ("a", "b"):
        raise FlashError("target must be 'a' or 'b'")
    g0 = cur_t0["gen"] if cur_t0.get("ok") else 0
    g1 = cur_t1["gen"] if cur_t1.get("ok") else 0
    active = boot2_pick_table(cur_t0, cur_t1)
    write_off = TABLE1 if active == 0 else TABLE0
    new_gen = max(g0, g1) + 1
    # Preserve the current appfw gens as the baseline, then lift the target.
    src = cur_t0 if active == 0 else cur_t1
    if src.get("ok"):
        ga = src["entries"][APPFW_A]["gen"]
        gb = src["entries"][APPFW_B]["gen"]
    else:
        ga, gb = 3, 2
    if target == "b":
        gb = max(ga, gb) + 1
        flag_a, flag_b = 0, ord("T")   # trial flag, matches ota.c boot-confirm
    else:
        ga = max(ga, gb) + 1
        flag_a, flag_b = 0, 0          # recovery: stock, no trial
    tbl = build_table(new_gen, ga, gb, flag_a, flag_b)
    return write_off, tbl, new_gen, (ga, gb)


# ---- CLI ---------------------------------------------------------------
def load_stub(path):
    with open(path, "rb") as f:
        return f.read()


def open_link(args):
    port = ub.pick_port(args.port)
    print(f"port {port} at {args.baud} 8N1")
    transport = ub.SerialTransport(port, args.baud)
    return ub.Link(transport), transport


def read_both_tables(flasher, retries):
    rep, data = flasher.op(FW_OP_READ, TABLE0, TABLES_END - TABLE0,
                           capture_len=TABLES_END - TABLE0, retries=retries)
    t0 = parse_table(data, 0)
    t1 = parse_table(data, TABLE1 - TABLE0)
    return t0, t1, data


def print_tables(t0, t1):
    for addr, t in ((TABLE0, t0), (TABLE1, t1)):
        if t.get("magic") != WMPT_MAGIC:
            print(f"table @{addr:#06x}: no WMPT table")
            continue
        st = "VALID" if t["ok"] else "INVALID: " + ", ".join(t["problems"])
        print(f"table @{addr:#06x}: gen {t['gen']} [{st}]")
        for i, e in enumerate(t["entries"]):
            fl = {0: "", 0x54: " TRIAL", 0x42: " TRIAL-BOOTED"}.get(e["flag"], f" flag={e['flag']:#x}")
            print(f"   [{i}] type {e['type']} {e['name']:9s} "
                  f"{e['start']:#08x} +{e['size']:#07x} gen {e['gen']}{fl}")
    if t0.get("magic") == WMPT_MAGIC or t1.get("magic") == WMPT_MAGIC:
        pick = boot2_pick_table(t0, t1)
        t = (t0, t1)[pick]
        print(f"boot2 picks table @{(TABLE0, TABLE1)[pick]:#06x}", end="")
        if t["ok"]:
            print(f" -> boots slot {'B' if table_selects_b(t) else 'A'}")
        else:
            print()


def cmd_write_slot_b(args):
    with open(args.image, "rb") as f:
        image = f.read()
    info = validate_mrvl(image)
    print(f"image {args.image}: {len(image)} bytes, {len(info['segs'])} segments, "
          f"entry {info['entry']:#010x}, end {info['end']:#x}")
    chunks = plan_chunks(image, SLOTB, args.chunk)
    print(f"plan: {len(chunks)} chunk(s) of up to {args.chunk:#x} bytes into slot B")
    if args.plan_only:
        for c in chunks:
            print(f"   {c['offset']:#08x}  {c['data_len']:#07x} bytes  crc {c['crc']:08x}")
        return 0

    link, transport = open_link(args)
    flasher = Flasher(link, load_stub(args.stub), int(args.base, 0),
                      args.detect_timeout, args.report_timeout)
    try:
        t0, t1, _ = read_both_tables(flasher, args.retries)
        print_tables(t0, t1)
        if (t0.get("ok") and table_selects_b(t0) and boot2_pick_table(t0, t1) == 0) or \
           (t1.get("ok") and table_selects_b(t1) and boot2_pick_table(t0, t1) == 1):
            raise FlashError("slot B is the active boot slot; refusing to overwrite it")
        for i, c in enumerate(chunks, 1):
            print(f"chunk {i}/{len(chunks)} -> {c['offset']:#08x} ({c['data_len']:#x} bytes)")
            flasher.op(FW_OP_WRITE, c["offset"], c["data_len"], c["payload"],
                       c["crc"], retries=args.retries)
        print("verifying the whole slot by CRC readback ...")
        want = crc32_mw(image)
        rep, _ = flasher.op(FW_OP_CRC, SLOTB, len(image), retries=args.retries)
        got = rep.value("CRC")
        gotcrc = int(got.split()[-1], 16) if got else None
        if gotcrc != want:
            raise FlashError(f"slot CRC {gotcrc:#010x} != image {want:#010x}")
        print(f"slot B verified: crc {want:08x}. Run 'activate b' to boot it.")
        return 0
    finally:
        transport.close()


def cmd_activate(args):
    if not args.port:
        raise FlashError("activate reads the device even with --plan-only: pass --port")
    link, transport = open_link(args)
    flasher = Flasher(link, load_stub(args.stub), int(args.base, 0),
                      args.detect_timeout, args.report_timeout)
    try:
        t0, t1, _ = read_both_tables(flasher, args.retries)
        print_tables(t0, t1)
        write_off, tbl, new_gen, (ga, gb) = plan_activate(t0, t1, args.target)
        print(f"writing gen-{new_gen} table (appfw A gen {ga}, B gen {gb}) "
              f"into the inactive sector @{write_off:#06x}")
        if args.plan_only:
            print(f"   {len(tbl)} bytes, crc {crc32_mw(tbl):08x}")
            return 0
        flasher.op(FW_OP_WRITE, write_off, len(tbl), tbl, crc32_mw(tbl),
                   retries=args.retries)
        print("re-reading tables to confirm the switch ...")
        t0, t1, _ = read_both_tables(flasher, args.retries)
        print_tables(t0, t1)
        pick = boot2_pick_table(t0, t1)
        sel = table_selects_b((t0, t1)[pick])
        want_b = args.target == "b"
        if sel != want_b:
            raise FlashError("table switch did not take; check the report above")
        print(f"activated slot {'B (custom)' if want_b else 'A (stock recovery)'}. "
              f"Power-cycle to boot it.")
        return 0
    finally:
        transport.close()


def cmd_status(args):
    link, transport = open_link(args)
    flasher = Flasher(link, load_stub(args.stub), int(args.base, 0),
                      args.detect_timeout, args.report_timeout)
    try:
        t0, t1, _ = read_both_tables(flasher, args.retries)
        print_tables(t0, t1)
        return 0
    finally:
        transport.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=not ("--plan-only" in sys.argv),
                    help="serial port or glob, e.g. /dev/ttyUSB0, /dev/cu.usbserial-*, COM5")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--stub", default=os.path.join(HERE, "mw300_flashwr_110000.bin"))
    ap.add_argument("--base", default=hex(LOAD_BASE))
    ap.add_argument("--chunk", type=lambda s: int(s, 0), default=DEFAULT_CHUNK,
                    help="bytes per ROM session (multiple of 4K; default 0x1000 = proven load size)")
    ap.add_argument("--retries", type=int, default=2)
    ap.add_argument("--detect-timeout", type=float, default=180.0)
    ap.add_argument("--report-timeout", type=float, default=30.0)
    ap.add_argument("--plan-only", action="store_true",
                    help="print what would be done; no serial port is opened")
    sp = ap.add_subparsers(dest="cmd", required=True)
    p = sp.add_parser("write-slot-b"); p.add_argument("image")
    p = sp.add_parser("activate"); p.add_argument("target", choices=["a", "b"])
    sp.add_parser("status")
    args = ap.parse_args()
    try:
        if args.cmd == "write-slot-b":
            return cmd_write_slot_b(args)
        if args.cmd == "activate":
            return cmd_activate(args)
        if args.cmd == "status":
            if args.plan_only:
                print("status needs the device; nothing to plan offline")
                return 0
            return cmd_status(args)
    except FlashError as exc:
        print(f"\nFAILED: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
