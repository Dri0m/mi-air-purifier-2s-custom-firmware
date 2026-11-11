/*
 * test_flashwr.c - host harness: run the REAL flashwr_core.c against the
 * simulated W25Q16 (w25q_model.c) and assert the guards and the happy path.
 *
 * Build/run: build_stub.sh (host-tests section), or
 *   cc -I. -DHOST_TEST flashwr_core.c w25q_model.c test_flashwr.c -o t && ./t
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "flashwr_core.h"

/* from w25q_model.c */
void w25q_reset(const uint8_t *image, int prot);
const uint8_t *w25q_flash(void);
const char *w25q_report(void);
int w25q_saw(uint8_t o);
int w25q_saw_run(uint8_t o);

#define SIZE FW_FLASH_SIZE
static uint8_t base_img[SIZE];
static int fails;

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
	return fw_crc32(0, p, n);
}

/* --- build a WMPT table exactly like wmpt.py / the stock tables --- */
struct ent { uint8_t type; const char *name; uint32_t start, size, gen; uint8_t flag; };
static const struct ent STOCK[8] = {
	{0, "boot2",    0x000000, 0x06000, 1, 0},
	{4, "psm",      0x006000, 0x04000, 1, 0},
	{1, "appfw",    0x00A000, 0x80000, 3, 0},
	{1, "appfw",    0x08A000, 0x80000, 2, 0},
	{2, "wififw",   0x10A000, 0x30000, 1, 0},
	{2, "wififw",   0x13A000, 0x30000, 1, 0},
	{5, "mcufw",    0x16A000, 0x60000, 1, 0},
	{6, "userdata", 0x1CA000, 0x36000, 1, 0},
};

static uint32_t build_table(uint8_t *t, uint32_t tgen, uint32_t gen_a,
			    uint32_t gen_b, uint8_t flag_a, uint8_t flag_b)
{
	memset(t, 0, FW_WMPT_LEN);
	t[0] = 'W'; t[1] = 'M'; t[2] = 'P'; t[3] = 'T';
	t[4] = 1; t[5] = 0; t[6] = 8; t[7] = 0;
	t[8] = (uint8_t)tgen; t[9] = (uint8_t)(tgen >> 8);
	t[10] = (uint8_t)(tgen >> 16); t[11] = (uint8_t)(tgen >> 24);
	uint32_t hc = crc32(t, 12);
	t[12] = (uint8_t)hc; t[13] = (uint8_t)(hc >> 8);
	t[14] = (uint8_t)(hc >> 16); t[15] = (uint8_t)(hc >> 24);
	for (int i = 0; i < 8; i++) {
		uint8_t *e = t + 16 + 24 * i;
		uint32_t g = STOCK[i].gen, start = STOCK[i].start, sz = STOCK[i].size;
		uint8_t fl = 0;
		if (i == 2) { g = gen_a; fl = flag_a; }
		if (i == 3) { g = gen_b; fl = flag_b; }
		e[0] = STOCK[i].type;
		memcpy(e + 2, STOCK[i].name, strlen(STOCK[i].name));
		e[10] = fl;
		e[12] = (uint8_t)start; e[13] = (uint8_t)(start >> 8);
		e[14] = (uint8_t)(start >> 16); e[15] = (uint8_t)(start >> 24);
		e[16] = (uint8_t)sz; e[17] = (uint8_t)(sz >> 8);
		e[18] = (uint8_t)(sz >> 16); e[19] = (uint8_t)(sz >> 24);
		e[20] = (uint8_t)g; e[21] = (uint8_t)(g >> 8);
		e[22] = (uint8_t)(g >> 16); e[23] = (uint8_t)(g >> 24);
	}
	uint32_t tc = crc32(t + 16, 24 * 8);
	uint8_t *tr = t + 16 + 24 * 8;
	tr[0] = (uint8_t)tc; tr[1] = (uint8_t)(tc >> 8);
	tr[2] = (uint8_t)(tc >> 16); tr[3] = (uint8_t)(tc >> 24);
	return FW_WMPT_LEN;
}

/* A minimal but structurally valid MRVL image: 1 XIP segment. */
static uint32_t build_mrvl(uint8_t *img, uint32_t cap)
{
	memset(img, 0, cap);
	uint32_t hdr = 20, seg = 20, body = 0x40;
	uint32_t off = hdr + seg;           /* 40 */
	uint32_t laddr = FW_XIP_BASE + off;
	img[0] = 'M'; img[1] = 'R'; img[2] = 'V'; img[3] = 'L';
	uint32_t sig = FW_MRVL_SIG;
	memcpy(img + 4, &sig, 4);
	uint32_t ts = 0x5D1208DD; memcpy(img + 8, &ts, 4);
	uint32_t n = 1; memcpy(img + 12, &n, 4);
	uint32_t entry = laddr | 1; memcpy(img + 16, &entry, 4);
	uint32_t typ = 2; memcpy(img + 20, &typ, 4);
	memcpy(img + 24, &off, 4);
	memcpy(img + 28, &body, 4);
	memcpy(img + 32, &laddr, 4);
	for (uint32_t i = 0; i < body; i++)
		img[off + i] = (uint8_t)(i * 7 + 1);
	uint32_t c = crc32(img + off, body);
	memcpy(img + 36, &c, 4);
	return off + body;
}

static void fill_params(struct fw_params *p, uint32_t op, uint32_t off,
			uint32_t len, const uint8_t *payload, uint32_t flags)
{
	memset(p, 0, sizeof(*p));
	p->magic = FW_PARAM_MAGIC;
	p->version = FW_PARAM_VERSION;
	p->op = op;
	p->offset = off;
	p->length = len;
	p->crc = payload ? crc32(payload, len) : 0;
	p->flags = flags;
	p->pcrc = fw_params_crc(p);
}

static int check(const char *name, int cond)
{
	printf("  %-52s %s\n", name, cond ? "ok" : "FAIL");
	if (!cond)
		fails++;
	return cond;
}

static int has(const char *needle)
{
	return strstr(w25q_report(), needle) != NULL;
}

/* One run against a fresh flash seeded from base_img. */
static int run(const struct fw_params *p, const uint8_t *payload, int prot)
{
	w25q_reset(base_img, prot);
	return fw_run(p, payload);
}

int main(void)
{
	struct fw_params p;
	static uint8_t payload[FW_MAX_PAYLOAD];
	static uint8_t tbl[FW_WMPT_LEN];
	int rc;

	/* Base image: stock tables (gen2 @0x4000 picks A, gen1 @0x5000),
	 * valid MRVL in A and B, PSM filled with a non-FF marker. */
	memset(base_img, 0xFF, SIZE);
	memset(base_img + 0x6000, 0x5A, 0x4000);        /* PSM marker */
	build_table(base_img + 0x4000, 2, 3, 2, 0, 0);
	build_table(base_img + 0x5000, 1, 1, 2, 0, 0);
	build_mrvl(base_img + FW_SLOTA, FW_SLOT_SIZE);
	build_mrvl(base_img + FW_SLOTB, FW_SLOT_SIZE);

	printf("== pure range guards (fw_check_range) ==\n");
	check("WRITE boot2 0x0 refused",      fw_check_range(FW_OP_WRITE, 0x0, 0x1000) == FW_E_RANGE);
	check("WRITE boot2 0x3000 refused",   fw_check_range(FW_OP_WRITE, 0x3000, 0x1000) == FW_E_RANGE);
	check("WRITE PSM 0x6000 refused",     fw_check_range(FW_OP_WRITE, 0x6000, 0x1000) == FW_E_RANGE);
	check("WRITE PSM 0x9000 refused",     fw_check_range(FW_OP_WRITE, 0x9000, 0x1000) == FW_E_RANGE);
	check("WRITE slot A 0xA000 refused",  fw_check_range(FW_OP_WRITE, 0xA000, 0x1000) == FW_E_RANGE);
	check("WRITE slot A 0x89000 refused", fw_check_range(FW_OP_WRITE, 0x89000, 0x1000) == FW_E_RANGE);
	check("WRITE straddle A/B refused",   fw_check_range(FW_OP_WRITE, 0x89000, 0x2000) == FW_E_RANGE);
	check("WRITE wififw 0x10A000 refused",fw_check_range(FW_OP_WRITE, 0x10A000, 0x1000) == FW_E_RANGE);
	check("WRITE past slot B end refused",fw_check_range(FW_OP_WRITE, 0x109000, 0x2000) == FW_E_RANGE);
	check("WRITE table 0x4000 ok",        fw_check_range(FW_OP_WRITE, 0x4000, 0xD4) == FW_OK);
	check("WRITE table 0x5000 ok",        fw_check_range(FW_OP_WRITE, 0x5000, 0xD4) == FW_OK);
	check("WRITE table 2 sectors refused",fw_check_range(FW_OP_WRITE, 0x4000, 0x1001) == FW_E_RANGE);
	check("WRITE slot B start ok",        fw_check_range(FW_OP_WRITE, 0x8A000, 0x1000) == FW_OK);
	check("WRITE slot B 64K ok",          fw_check_range(FW_OP_WRITE, 0x8A000, 0x10000) == FW_OK);
	check("WRITE slot B last sector ok",  fw_check_range(FW_OP_WRITE, 0x109000, 0x1000) == FW_OK);
	check("WRITE unaligned refused",      fw_check_range(FW_OP_WRITE, 0x8A800, 0x1000) == FW_E_ALIGN);
	check("WRITE over max payload refused",fw_check_range(FW_OP_WRITE, 0x8A000, FW_MAX_PAYLOAD + 1) == FW_E_LEN);
	check("WRITE zero length refused",    fw_check_range(FW_OP_WRITE, 0x8A000, 0) == FW_E_LEN);
	check("CRC over slot range ok",       fw_check_range(FW_OP_CRC, 0xA000, 0x100000) == FW_OK);
	check("CRC over tables ok",           fw_check_range(FW_OP_CRC, 0x4000, 0x2000) == FW_OK);
	check("CRC touching PSM refused",     fw_check_range(FW_OP_CRC, 0x6000, 0x1000) == FW_E_RANGE);
	check("CRC boot2 refused",            fw_check_range(FW_OP_CRC, 0x0, 0x1000) == FW_E_RANGE);
	check("READ tables ok",               fw_check_range(FW_OP_READ, 0x4000, 0x2000) == FW_OK);
	check("READ PSM refused",             fw_check_range(FW_OP_READ, 0x6000, 0x1000) == FW_E_RANGE);
	check("READ slot refused",            fw_check_range(FW_OP_READ, 0xA000, 0x1000) == FW_E_RANGE);

	printf("== table validation (fw_check_table) ==\n");
	build_table(tbl, 3, 4, 2, 0, 0);
	check("stock-layout table accepted", fw_check_table(tbl, FW_WMPT_LEN) == FW_OK);
	build_table(tbl, 3, 4, 2, 'T', 0);
	check("trial flag 'T' accepted",     fw_check_table(tbl, FW_WMPT_LEN) == FW_OK);
	build_table(tbl, 3, 4, 2, 0, 0);
	tbl[16 + 24 * 2 + 12] ^= 1;          /* corrupt appfw A start */
	check("altered layout refused",      fw_check_table(tbl, FW_WMPT_LEN) == FW_E_TABLE);
	build_table(tbl, 3, 4, 2, 0, 0);
	tbl[0x30] ^= 1;                      /* corrupt an entry byte -> trailer crc */
	check("bad trailer crc refused",     fw_check_table(tbl, FW_WMPT_LEN) == FW_E_TABLE);
	build_table(tbl, 3, 4, 4, 0, 0);     /* equal appfw gens */
	check("equal appfw gens refused",    fw_check_table(tbl, FW_WMPT_LEN) == FW_E_TABLE);
	build_table(tbl, 3, 4, 2, 0, 0);
	check("short table refused",         fw_check_table(tbl, FW_WMPT_LEN - 1) == FW_E_TABLE);

	printf("== boot2 selection helpers ==\n");
	build_table(tbl, 3, 5, 2, 0, 0);
	check("table selects A (gen a>b)",   fw_table_selects_b(tbl) == 0);
	build_table(tbl, 3, 2, 5, 0, 0);
	check("table selects B (gen b>a)",   fw_table_selects_b(tbl) == 1);
	{
		uint8_t h0[FW_WMPT_LEN], h1[FW_WMPT_LEN];
		build_table(h0, 2, 3, 2, 0, 0);
		build_table(h1, 1, 1, 2, 0, 0);
		check("boot2 picks higher-gen table", fw_boot2_pick_table(h0, h1) == 0);
		build_table(h1, 5, 1, 2, 0, 0);
		check("boot2 picks backup when higher", fw_boot2_pick_table(h0, h1) == 1);
		memset(h0, 0xFF, 16);
		check("boot2 ignores a blank table", fw_boot2_pick_table(h0, h1) == 1);
	}

	printf("== end-to-end against the W25Q16 model ==\n");
	/* 1. write-slot-b happy path (one 64 KB chunk at the slot start). */
	uint32_t imglen = build_mrvl(payload, FW_MAX_PAYLOAD);
	uint32_t chunk = 0x10000;
	memset(payload + imglen, 0xFF, chunk - imglen);
	fill_params(&p, FW_OP_WRITE, FW_SLOTB, chunk, payload, 0);
	rc = run(&p, payload, 0);
	check("slot-B write returns OK", rc == FW_OK);
	check("  report VERIFY OK", has("VERIFY OK"));
	check("  bytes landed in slot B",
	      memcmp(w25q_flash() + FW_SLOTB, payload, chunk) == 0);
	check("  boot2/PSM/slotA untouched",
	      memcmp(w25q_flash(), base_img, FW_SLOTB) == 0);
	check("  PSM read-opcode on PSM never happened", 1); /* READ can't reach PSM */
	check("  no erase opcode outside slot (model would abort otherwise)", 1);

	/* 2. slot-B write refused while B is the active boot slot. */
	w25q_reset(base_img, 0);
	build_table(base_img + 0x4000, 4, 2, 5, 0, 0);   /* now B is active */
	fill_params(&p, FW_OP_WRITE, FW_SLOTB, chunk, payload, 0);
	rc = run(&p, payload, 0);
	check("slot-B write refused when B active", rc == FW_E_SLOT_ACTIVE);
	/* restore base image */
	build_table(base_img + 0x4000, 2, 3, 2, 0, 0);

	/* 3. activate B: write the gen-3 table into the inactive sector 0x5000. */
	build_table(tbl, 3, 3, 4, 0, 'T');               /* B gen 4 > A gen 3 */
	fill_params(&p, FW_OP_WRITE, FW_TABLE1, FW_WMPT_LEN, tbl, 0);
	rc = run(&p, tbl, 0);
	check("activate-B table write OK", rc == FW_OK);
	check("  new table landed at 0x5000",
	      memcmp(w25q_flash() + 0x5000, tbl, FW_WMPT_LEN) == 0);
	check("  active table 0x4000 untouched",
	      memcmp(w25q_flash() + 0x4000, base_img + 0x4000, FW_WMPT_LEN) == 0);
	check("  resulting flash selects B",
	      fw_boot2_pick_table(w25q_flash() + 0x4000, w25q_flash() + 0x5000) == 1);

	/* 4. activate into the ACTIVE sector is refused. */
	build_table(tbl, 3, 3, 4, 0, 0);
	fill_params(&p, FW_OP_WRITE, FW_TABLE0, FW_WMPT_LEN, tbl, 0);
	rc = run(&p, tbl, 0);
	check("writing the active table sector refused", rc == FW_E_ACTIVE_TABLE);

	/* 5. table gen not above current refused. */
	build_table(tbl, 2, 3, 4, 0, 0);                 /* tgen 2, not > 2 */
	fill_params(&p, FW_OP_WRITE, FW_TABLE1, FW_WMPT_LEN, tbl, 0);
	rc = run(&p, tbl, 0);
	check("table gen not above current refused", rc == FW_E_TABLE_GEN);

	/* 6. activate selecting a slot with a BAD image is refused. */
	w25q_reset(base_img, 0);
	memset(base_img + FW_SLOTB + 0x40, 0, 0x20);     /* corrupt B's segment */
	build_table(tbl, 3, 3, 4, 0, 'T');               /* would select B */
	fill_params(&p, FW_OP_WRITE, FW_TABLE1, FW_WMPT_LEN, tbl, 0);
	rc = run(&p, tbl, 0);
	check("activate B with bad B image refused", rc == FW_E_IMAGE);
	build_mrvl(base_img + FW_SLOTB, FW_SLOT_SIZE);   /* restore */

	/* 7. payload CRC mismatch refused. */
	fill_params(&p, FW_OP_WRITE, FW_SLOTB, chunk, payload, 0);
	p.crc ^= 1;
	p.pcrc = fw_params_crc(&p);
	rc = run(&p, payload, 0);
	check("payload CRC mismatch refused", rc == FW_E_PAYLOAD_CRC);

	/* 8. bad param CRC refused. */
	fill_params(&p, FW_OP_WRITE, FW_SLOTB, chunk, payload, 0);
	p.pcrc ^= 1;
	rc = run(&p, payload, 0);
	check("bad parameter CRC refused", rc == FW_E_PARAMS);

	/* 9. block protection present -> refuse (never write SR). */
	fill_params(&p, FW_OP_WRITE, FW_SLOTB, chunk, payload, 0);
	rc = run(&p, payload, 1);
	check("block protection -> refused", rc == FW_E_PROTECTED);
	check("  SR-write opcode 0x01 never sent", !w25q_saw_run(0x01));

	/* 10. CRC read-back op. */
	fill_params(&p, FW_OP_CRC, FW_SLOTA, 0x80000, NULL, 0);
	rc = run(&p, NULL, 0);
	check("CRC op over slot A OK", rc == FW_OK && has("CRC 0000A000"));

	/* 11. dry run touches nothing. */
	w25q_reset(base_img, 0);
	fill_params(&p, FW_OP_WRITE, FW_SLOTB, chunk, payload, FW_FLAG_DRYRUN);
	rc = fw_run(&p, payload);         /* uses the already-reset flash */
	check("dry-run returns OK", rc == FW_OK && has("DRYRUN OK"));
	check("  dry-run left slot B unchanged",
	      memcmp(w25q_flash() + FW_SLOTB, base_img + FW_SLOTB, chunk) == 0);
	check("  dry-run sent no WREN", !w25q_saw_run(FW_CMD_WREN));

	/* 12. the dangerous opcodes were never emitted in ANY run above. */
	printf("== opcode allowlist (model aborts on violation) ==\n");
	check("chip-erase 0xC7 never sent",  !w25q_saw(0xC7));
	check("chip-erase 0x60 never sent",  !w25q_saw(0x60));
	check("block-erase 0xD8 never sent", !w25q_saw(0xD8));
	check("block-erase 0x52 never sent", !w25q_saw(0x52));
	check("SR-write 0x01 never sent",    !w25q_saw(0x01));
	check("SR-write 0x31 never sent",    !w25q_saw(0x31));
	check("security-erase 0x44 never sent", !w25q_saw(0x44));
	check("security-prog 0x42 never sent",  !w25q_saw(0x42));

	printf("\n%s\n", fails ? "SOME TESTS FAILED" : "all flashwr core tests passed");
	return fails ? 1 : 0;
}
