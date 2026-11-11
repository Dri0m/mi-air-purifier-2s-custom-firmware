/*
 * flashwr_core.c - portable core of the guarded 88MW300 flash writer.
 * See flashwr_core.h for the image layout and the guard rules.
 *
 * Freestanding: no libc calls (the stub is built with -ffreestanding
 * -fno-builtin), no struct copies, no large initialised locals.
 */
#include "flashwr_core.h"

/* ------------------------------------------------------------ helpers */

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* CRC-32/IEEE reflected 0xEDB88320, init 0, no final xor (boot ROM,
 * MRVL segments and WMPT all use this one; it is not zlib.crc32). */
uint32_t fw_crc32(uint32_t crc, const uint8_t *p, uint32_t n)
{
	while (n--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
	}
	return crc;
}

uint32_t fw_params_crc(const struct fw_params *p)
{
	return fw_crc32(0, (const uint8_t *)p, 28u);
}

/* [a, b) and [c, d) overlap */
static int overlaps(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	return a < d && c < b;
}

/* ------------------------------------------------------------ guards */

int fw_check_params(const struct fw_params *p)
{
	if (p->magic != FW_PARAM_MAGIC || p->version != FW_PARAM_VERSION ||
	    p->pcrc != fw_params_crc(p))
		return FW_E_PARAMS;
	if (p->flags & ~FW_FLAG_DRYRUN)
		return FW_E_PARAMS;
	return FW_OK;
}

int fw_is_table_write(uint32_t op, uint32_t offset)
{
	return op == FW_OP_WRITE && (offset == FW_TABLE0 || offset == FW_TABLE1);
}

int fw_check_range(uint32_t op, uint32_t offset, uint32_t length)
{
	uint32_t end, span_end;

	if (op != FW_OP_WRITE && op != FW_OP_CRC && op != FW_OP_READ)
		return FW_E_OP;
	if (length == 0)
		return FW_E_LEN;
	if (offset >= FW_FLASH_SIZE || length > FW_FLASH_SIZE - offset)
		return FW_E_RANGE;
	end = offset + length;               /* no overflow: both <= 2 MiB */

	if (op == FW_OP_READ) {
		/* Table sectors only: never boot2 code, never the PSM. */
		if (offset < FW_TABLE0 || end > FW_TABLES_END)
			return FW_E_RANGE;
		return FW_OK;
	}
	if (op == FW_OP_CRC) {
		int in_tables = offset >= FW_TABLE0 && end <= FW_TABLES_END;
		int in_slots = offset >= FW_SLOTA && end <= FW_SLOTB_END;
		if (!in_tables && !in_slots)
			return FW_E_RANGE;
		if (overlaps(offset, end, FW_PSM_START, FW_PSM_END))
			return FW_E_RANGE;
		return FW_OK;
	}

	/* WRITE */
	if (length > FW_MAX_PAYLOAD)
		return FW_E_LEN;
	if (offset & (FW_SECTOR - 1u))
		return FW_E_ALIGN;
	span_end = offset + ((length + FW_SECTOR - 1u) & ~(FW_SECTOR - 1u));
	if (offset == FW_TABLE0 || offset == FW_TABLE1) {
		if (span_end != offset + FW_SECTOR)
			return FW_E_RANGE;
	} else if (offset < FW_SLOTB || span_end > FW_SLOTB_END) {
		return FW_E_RANGE;
	}
	/* Belt and braces: the explicit deny list, whatever the code above
	 * says.  boot2, PSM, slot A, wififw and everything above. */
	if (overlaps(offset, span_end, 0u, FW_BOOT2_END) ||
	    overlaps(offset, span_end, FW_PSM_START, FW_PSM_END) ||
	    overlaps(offset, span_end, FW_SLOTA, FW_SLOTB) ||
	    overlaps(offset, span_end, FW_SLOTB_END, FW_FLASH_SIZE))
		return FW_E_RANGE;
	return FW_OK;
}

/* Stock layout of this unit, verified against both tables read from the
 * flash (fw-boot-ota-build.md 2.1; boot2.bin @0x4000 / @0x5000). */
struct layout_ent {
	uint8_t type;
	char name[8];
	uint32_t start, size;
};

static const struct layout_ent layout[FW_WMPT_N] = {
	{0, {'b', 'o', 'o', 't', '2'},                   0x000000u, 0x06000u},
	{4, {'p', 's', 'm'},                             0x006000u, 0x04000u},
	{1, {'a', 'p', 'p', 'f', 'w'},                   0x00A000u, 0x80000u},
	{1, {'a', 'p', 'p', 'f', 'w'},                   0x08A000u, 0x80000u},
	{2, {'w', 'i', 'f', 'i', 'f', 'w'},              0x10A000u, 0x30000u},
	{2, {'w', 'i', 'f', 'i', 'f', 'w'},              0x13A000u, 0x30000u},
	{5, {'m', 'c', 'u', 'f', 'w'},                   0x16A000u, 0x60000u},
	{6, {'u', 's', 'e', 'r', 'd', 'a', 't', 'a'},    0x1CA000u, 0x36000u},
};

/* A WMPT table with exactly the stock layout, both CRCs right, distinct
 * appfw gens, and only the boot-confirm flags ('T', 'B') in pad[0] of the
 * appfw entries (firmware/src/ota/ota.c). */
int fw_check_table(const uint8_t *t, uint32_t len)
{
	uint32_t gen;

	if (len != FW_WMPT_LEN)
		return FW_E_TABLE;
	if (rd32(t) != FW_WMPT_MAGIC || t[4] != 1u || t[5] != 0u ||
	    t[6] != FW_WMPT_N || t[7] != 0u)
		return FW_E_TABLE;
	gen = rd32(t + 8);
	if (gen == 0u || gen >= 0x80000000u)
		return FW_E_TABLE;
	if (rd32(t + 12) != fw_crc32(0, t, 12u))
		return FW_E_TABLE;
	if (rd32(t + 16u + 24u * FW_WMPT_N) != fw_crc32(0, t + 16, 24u * FW_WMPT_N))
		return FW_E_TABLE;
	for (uint32_t i = 0; i < FW_WMPT_N; i++) {
		const uint8_t *e = t + 16u + 24u * i;
		const struct layout_ent *l = &layout[i];
		uint32_t egen = rd32(e + 20);

		if (e[0] != l->type || e[1] != 0u)
			return FW_E_TABLE;
		for (int k = 0; k < 8; k++)
			if ((char)e[2 + k] != l->name[k])
				return FW_E_TABLE;
		if (e[11] != 0u)
			return FW_E_TABLE;
		if (l->type == 1u) {
			if (e[10] != 0u && e[10] != 'T' && e[10] != 'B')
				return FW_E_TABLE;
		} else if (e[10] != 0u) {
			return FW_E_TABLE;
		}
		if (rd32(e + 12) != l->start || rd32(e + 16) != l->size)
			return FW_E_TABLE;
		if (egen == 0u || egen >= 0x80000000u)
			return FW_E_TABLE;
	}
	if (rd32(t + 16 + 24 * 2 + 20) == rd32(t + 16 + 24 * 3 + 20))
		return FW_E_TABLE;
	return FW_OK;
}

/* boot2 0x46e: no magic -> gen 0; the backup wins only if strictly higher. */
int fw_boot2_pick_table(const uint8_t *hdr0, const uint8_t *hdr1)
{
	uint32_t g0 = rd32(hdr0) == FW_WMPT_MAGIC ? rd32(hdr0 + 8) : 0u;
	uint32_t g1 = rd32(hdr1) == FW_WMPT_MAGIC ? rd32(hdr1 + 8) : 0u;

	return g1 > g0 ? 1 : 0;
}

/* boot2 boots the second appfw entry only if its gen is strictly higher. */
int fw_table_selects_b(const uint8_t *t)
{
	return rd32(t + 16 + 24 * 3 + 20) > rd32(t + 16 + 24 * 2 + 20);
}

/* ------------------------------------------------------------ report */

static void puts_(const char *s)
{
	while (*s)
		fwp_putc(*s++);
}

static void puthex(uint32_t v, int digits)
{
	static const char hex[] = "0123456789ABCDEF";

	while (digits--)
		fwp_putc(hex[(v >> (4 * digits)) & 15u]);
}

static void putdec(uint32_t v)
{
	char buf[10];
	int i = 0;

	do {
		buf[i++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v);
	while (i)
		fwp_putc(buf[--i]);
}

static void eol(void)
{
	puts_("\r\n");
}

/* Codes go out as numbers (the host has the names, see FW_E_* order);
 * keeps the stub below its parameter block. */
static int refuse(int code)
{
	puts_("REFUSED ");
	putdec((uint32_t)code);
	eol();
	return code;
}

static int fail(int code)
{
	puts_("ERROR ");
	putdec((uint32_t)code);
	eol();
	return code;
}

/* ------------------------------------------------------------ SPI flash */

static int cmd_allowed(uint8_t op)
{
	switch (op) {
	case FW_CMD_WREN: case FW_CMD_PP: case FW_CMD_SE4K:
	case FW_CMD_RDSR1: case FW_CMD_RDSR2: case FW_CMD_READ:
	case FW_CMD_JEDEC: case FW_CMD_EXITCRM: case FW_CMD_RSTEN:
	case FW_CMD_RST: case FW_CMD_RELPD:
		return 1;
	default:
		return 0;
	}
}

static int cmd_trap;

/* Every SPI transaction starts here: CS low, then the opcode. */
static int cmd_begin(uint8_t op)
{
	if (!cmd_allowed(op)) {
		cmd_trap = 1;
		return 0;
	}
	fwp_cs(0);
	fwp_xfer(op);
	return 1;
}

static void cmd_end(void)
{
	fwp_cs(1);
}

static void cmd_addr(uint32_t a)
{
	fwp_xfer((uint8_t)(a >> 16));
	fwp_xfer((uint8_t)(a >> 8));
	fwp_xfer((uint8_t)a);
}

static void simple_cmd(uint8_t op)
{
	if (cmd_begin(op))
		cmd_end();
}

static uint8_t read_sr(uint8_t op)
{
	uint8_t v = 0xFFu;

	if (cmd_begin(op)) {
		v = fwp_xfer(0xFFu);
		cmd_end();
	}
	return v;
}

static void flash_read(uint32_t addr, uint8_t *buf, uint32_t n)
{
	if (!cmd_begin(FW_CMD_READ))
		return;
	cmd_addr(addr);
	while (n--)
		*buf++ = fwp_xfer(0xFFu);
	cmd_end();
}

/* Leave continuous-read / power-down states (same as the dumper). */
static void flash_wake(void)
{
	if (cmd_begin(FW_CMD_EXITCRM)) {
		fwp_xfer(0xFFu);
		cmd_end();
	}
	simple_cmd(FW_CMD_RSTEN);
	simple_cmd(FW_CMD_RST);
	for (int i = 0; i < 4; i++)
		fwp_wait();
	simple_cmd(FW_CMD_RELPD);
	for (int i = 0; i < 4; i++)
		fwp_wait();
}

static uint32_t flash_jedec(void)
{
	uint32_t id = 0;

	if (cmd_begin(FW_CMD_JEDEC)) {
		id = (uint32_t)fwp_xfer(0xFFu) << 16;
		id |= (uint32_t)fwp_xfer(0xFFu) << 8;
		id |= fwp_xfer(0xFFu);
		cmd_end();
	}
	return id;
}

#define POLLS_ERASE 40000u   /* >= 4 s at ~100 us per poll; tSE max 0.4 s */
#define POLLS_PAGE  400u     /* >= 40 ms; tPP max 3 ms */

static int wait_idle(uint32_t polls)
{
	while (polls--) {
		if (!(read_sr(FW_CMD_RDSR1) & 1u))
			return FW_OK;
		fwp_wait();
	}
	return FW_E_TIMEOUT;
}

static int write_enable(void)
{
	simple_cmd(FW_CMD_WREN);
	return (read_sr(FW_CMD_RDSR1) & 3u) == 2u ? FW_OK : FW_E_WEL;
}

static int erase_sector(uint32_t addr)
{
	int rc = write_enable();

	if (rc)
		return rc;
	if (!cmd_begin(FW_CMD_SE4K))
		return FW_E_CMD;
	cmd_addr(addr);
	cmd_end();
	return wait_idle(POLLS_ERASE);
}

/* Program n bytes at addr; the caller keeps them inside one page. */
static int program_page(uint32_t addr, const uint8_t *src, uint32_t n)
{
	int rc = write_enable();

	if (rc)
		return rc;
	if (!cmd_begin(FW_CMD_PP))
		return FW_E_CMD;
	cmd_addr(addr);
	while (n--)
		fwp_xfer(*src++);
	cmd_end();
	return wait_idle(POLLS_PAGE);
}

static int program_range(uint32_t addr, const uint8_t *src, uint32_t n,
			 uint32_t *pages)
{
	while (n) {
		uint32_t room = FW_PAGE - (addr & (FW_PAGE - 1u));
		uint32_t k = n < room ? n : room;
		int rc = program_page(addr, src, k);

		if (rc)
			return rc;
		addr += k;
		src += k;
		n -= k;
		++*pages;
	}
	return FW_OK;
}

static uint8_t buf[256];

static uint32_t flash_crc(uint32_t addr, uint32_t n)
{
	uint32_t crc = 0;

	while (n) {
		uint32_t k = n < sizeof(buf) ? n : (uint32_t)sizeof(buf);

		flash_read(addr, buf, k);
		crc = fw_crc32(crc, buf, k);
		addr += k;
		n -= k;
	}
	return crc;
}

/* boot2's image check, read from flash: MRVL header, every segment inside
 * the slot and CRC-correct, entry in the XIP window, XIP segments placed at
 * their slot offset.  Prints "IMAGE OK <segs>" on success. */
static int verify_slot_image(uint32_t slot)
{
	uint8_t h[20];
	uint32_t n, entry;

	flash_read(slot, h, 20u);
	n = rd32(h + 12);
	entry = rd32(h + 16);
	if (rd32(h) != FW_MRVL_MAGIC || rd32(h + 4) != FW_MRVL_SIG ||
	    n < 1u || n > 9u || entry < FW_XIP_BASE ||
	    entry >= FW_XIP_BASE + FW_SLOT_SIZE || !(entry & 1u))
		return FW_E_IMAGE;
	for (uint32_t i = 0; i < n; i++) {
		uint8_t s[20];
		uint32_t off, len, la;

		flash_read(slot + 20u + 20u * i, s, 20u);
		off = rd32(s + 4);
		len = rd32(s + 8);
		la = rd32(s + 12);
		if (rd32(s) != 2u || off < 20u + 20u * n || off >= FW_SLOT_SIZE ||
		    len > FW_SLOT_SIZE - off)
			return FW_E_IMAGE;
		if (la >= FW_XIP_BASE && la < FW_XIP_BASE + FW_SLOT_SIZE &&
		    la - FW_XIP_BASE != off)
			return FW_E_IMAGE;
		if (flash_crc(slot + off, len) != rd32(s + 16))
			return FW_E_IMAGE;
	}
	puts_("IMAGE OK ");
	putdec(n);
	eol();
	return FW_OK;
}

static uint8_t tbl[FW_WMPT_LEN];

/* Read both table headers and report which one boot2 selects. */
static int active_table(void)
{
	uint8_t h0[16], h1[16];
	int pick;

	flash_read(FW_TABLE0, h0, 16u);
	flash_read(FW_TABLE1, h1, 16u);
	pick = fw_boot2_pick_table(h0, h1);
	puts_("ACTIVE-TABLE ");
	puthex(pick ? FW_TABLE1 : FW_TABLE0, 8);
	puts_(" gen ");
	{
		const uint8_t *h = pick ? h1 : h0;
		putdec(rd32(h) == FW_WMPT_MAGIC ? rd32(h + 8) : 0u);
	}
	eol();
	return pick;
}

static uint32_t table_gen_at(uint32_t addr)
{
	uint8_t h[16];

	flash_read(addr, h, 16u);
	return rd32(h) == FW_WMPT_MAGIC ? rd32(h + 8) : 0u;
}

/* Erase the span, program (table: header last), verify the span:
 * payload bytes must match, the rest of the span must read 0xFF. */
static int do_write(uint32_t off, uint32_t len, const uint8_t *src,
		    uint32_t want_crc, int table)
{
	uint32_t span = (len + FW_SECTOR - 1u) & ~(FW_SECTOR - 1u);
	uint32_t pages = 0, crc = 0, bad = 0xFFFFFFFFu;
	int rc;

	puts_("ERASE ");
	puthex(off, 8);
	fwp_putc(' ');
	putdec(span / FW_SECTOR);
	eol();
	for (uint32_t a = off; a < off + span; a += FW_SECTOR) {
		rc = erase_sector(a);
		if (rc)
			return fail(rc);
	}
	puts_("ERASE OK\r\n");

	puts_("PROG ");
	puthex(off, 8);
	fwp_putc(' ');
	putdec(len);
	eol();
	if (table) {
		/* Header (with the magic) last: until it lands the sector has
		 * no magic, boot2 sees gen 0 and keeps the other table. */
		rc = program_range(off + 16u, src + 16, len - 16u, &pages);
		if (!rc)
			rc = program_range(off, src, 16u, &pages);
	} else {
		rc = program_range(off, src, len, &pages);
	}
	if (rc)
		return fail(rc);
	puts_("PROG OK ");
	putdec(pages);
	eol();

	for (uint32_t i = 0; i < span; i += sizeof(buf)) {
		flash_read(off + i, buf, sizeof(buf));
		for (uint32_t k = 0; k < sizeof(buf); k++) {
			uint32_t pos = i + k;
			uint8_t want = pos < len ? src[pos] : 0xFFu;

			if (buf[k] != want && bad == 0xFFFFFFFFu)
				bad = off + pos;
		}
		if (i < len)
			crc = fw_crc32(crc, buf, len - i < sizeof(buf) ? len - i
							     : (uint32_t)sizeof(buf));
	}
	if (bad != 0xFFFFFFFFu || crc != want_crc || cmd_trap) {
		puts_("VERIFY FAIL ");
		puthex(crc, 8);
		puts_(" at ");
		puthex(bad, 8);
		eol();
		return FW_E_VERIFY;
	}
	puts_("VERIFY OK ");
	puthex(crc, 8);
	eol();
	return FW_OK;
}

static int run_write(const struct fw_params *p, const uint8_t *payload)
{
	int table = fw_is_table_write(p->op, p->offset);
	uint32_t crc = fw_crc32(0, payload, p->length);
	int rc;

	if (crc != p->crc) {
		return fail(FW_E_PAYLOAD_CRC);
	}
	puts_("PAYLOAD OK\r\n");

	if (table) {
		int pick;
		uint32_t newgen = rd32(payload + 8);

		rc = fw_check_table(payload, p->length);
		if (rc)
			return refuse(rc);
		pick = active_table();
		if (p->offset == (pick ? FW_TABLE1 : FW_TABLE0))
			return refuse(FW_E_ACTIVE_TABLE);
		if (newgen <= table_gen_at(FW_TABLE0) ||
		    newgen <= table_gen_at(FW_TABLE1))
			return refuse(FW_E_TABLE_GEN);
		rc = verify_slot_image(fw_table_selects_b(payload) ? FW_SLOTB
								    : FW_SLOTA);
		if (rc)
			return refuse(rc);
	} else {
		int pick = active_table();

		flash_read(pick ? FW_TABLE1 : FW_TABLE0, tbl, FW_WMPT_LEN);
		if (fw_check_table(tbl, FW_WMPT_LEN))
			return refuse(FW_E_LAYOUT);
		if (fw_table_selects_b(tbl))
			return refuse(FW_E_SLOT_ACTIVE);
	}

	if (p->flags & FW_FLAG_DRYRUN) {
		puts_("DRYRUN OK ");
		puthex(flash_crc(p->offset, p->length), 8);
		eol();
		return FW_OK;
	}
	return do_write(p->offset, p->length, payload, p->crc, table);
}

int fw_run(const struct fw_params *p, const uint8_t *payload)
{
	uint32_t id;
	uint8_t sr1, sr2;
	int rc;

	cmd_trap = 0;
	puts_("MWFLASHWR 1\r\n");
	rc = fw_check_params(p);
	if (rc)
		return refuse(rc);
	puts_("OP ");
	putdec(p->op);
	fwp_putc(' ');
	puthex(p->offset, 8);
	fwp_putc(' ');
	puthex(p->length, 8);
	fwp_putc(' ');
	puthex(p->crc, 8);
	if (p->flags & FW_FLAG_DRYRUN)
		puts_(" DRYRUN");
	eol();
	rc = fw_check_range(p->op, p->offset, p->length);
	if (rc)
		return refuse(rc);

	flash_wake();
	id = flash_jedec();
	puts_("JEDEC ");
	puthex(id, 6);
	eol();
	if (id != FW_JEDEC_W25Q16)
		return refuse(FW_E_JEDEC);
	sr1 = read_sr(FW_CMD_RDSR1);
	sr2 = read_sr(FW_CMD_RDSR2);
	puts_("SR ");
	puthex(sr1, 2);
	fwp_putc(' ');
	puthex(sr2, 2);
	eol();

	if (p->op == FW_OP_READ) {
		uint32_t crc = 0;

		puts_("BEGIN ");
		putdec(p->length);
		eol();
		for (uint32_t a = p->offset; a < p->offset + p->length;) {
			uint32_t k = p->offset + p->length - a;

			if (k > sizeof(buf))
				k = sizeof(buf);
			flash_read(a, buf, k);
			for (uint32_t i = 0; i < k; i++)
				fwp_putc((char)buf[i]);
			crc = fw_crc32(crc, buf, k);
			a += k;
		}
		puts_("\r\nDONE CRC ");
		puthex(crc, 8);
		eol();
		return FW_OK;
	}
	if (p->op == FW_OP_CRC) {
		puts_("CRC ");
		puthex(p->offset, 8);
		fwp_putc(' ');
		puthex(p->length, 8);
		fwp_putc(' ');
		puthex(flash_crc(p->offset, p->length), 8);
		eol();
		return FW_OK;
	}

	/* WRITE: block protection would make erase/program silently fail.
	 * Status registers are never written; refuse and let a human look. */
	if ((sr1 & 0x7Cu) || (sr2 & 0x40u))
		return refuse(FW_E_PROTECTED);
	rc = run_write(p, payload);
	if (cmd_trap)
		return fail(FW_E_CMD);
	return rc;
}
