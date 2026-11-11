/*
 * w25q_model.c - host-side simulated W25Q16 + port for flashwr_core.c.
 *
 * Compiled on macOS/Linux and driven by test_flashwr (via the thin C shim
 * flashwr_hosttest.c).  It provides fwp_cs/fwp_xfer/fwp_putc/fwp_wait so the
 * REAL flashwr_core.c runs unchanged against a byte-accurate flash model:
 * WREN/WEL latch, 4 KB sector erase, 256-byte page-program wrap, status WIP,
 * and - crucially - it ABORTS if the core ever clocks an opcode outside the
 * allowlist, so the "no dangerous opcode is even reachable" guard is tested
 * dynamically, not just by code review.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "flashwr_core.h"

#define SIZE FW_FLASH_SIZE

static uint8_t flash[SIZE];
static int wel;
static int cs_high = 1;
static int phase;          /* bytes since CS fell */
static uint8_t op;
static uint32_t addr;
static uint32_t pp_base;   /* page-program start, for wrap */
static int protect;        /* emulate block protection (erase/pp ignored) */

/* Captured report line and the set of opcodes seen this session. */
static char report[8192];
static int report_len;
static uint8_t seen[256];      /* cumulative over the whole test program */
static uint8_t seen_run[256];  /* cleared each w25q_reset */

void w25q_reset(const uint8_t *image, int prot)
{
	if (image)
		memcpy(flash, image, SIZE);
	else
		memset(flash, 0xFF, SIZE);
	wel = 0;
	cs_high = 1;
	protect = prot;
	report_len = 0;
	report[0] = 0;
	memset(seen_run, 0, sizeof(seen_run));
}

const uint8_t *w25q_flash(void) { return flash; }
const char *w25q_report(void) { return report; }
int w25q_saw(uint8_t o) { return seen[o]; }
int w25q_saw_run(uint8_t o) { return seen_run[o]; }

void fwp_putc(char c)
{
	if (report_len < (int)sizeof(report) - 1) {
		report[report_len++] = c;
		report[report_len] = 0;
	}
}

void fwp_wait(void) { }

void fwp_cs(int high)
{
	if (!high && cs_high) {        /* falling edge: new transaction */
		phase = 0;
		op = 0;
	}
	cs_high = high;
}

/* A W25Q16 would simply ignore an unknown opcode, which would let a buggy
 * core think it had erased when it had not.  For the test we want the
 * opposite: any opcode the core should never emit is a hard failure. */
static void forbid(uint8_t o)
{
	fprintf(stderr,
		"FATAL: flashwr_core clocked disallowed SPI opcode 0x%02X\n", o);
	abort();
}

static int is_allowed(uint8_t o)
{
	switch (o) {
	case FW_CMD_WREN: case FW_CMD_PP: case FW_CMD_SE4K:
	case FW_CMD_RDSR1: case FW_CMD_RDSR2: case FW_CMD_READ:
	case FW_CMD_JEDEC: case FW_CMD_EXITCRM: case FW_CMD_RSTEN:
	case FW_CMD_RST: case FW_CMD_RELPD:
		return 1;
	default:
		return 0;
	}
}

uint8_t fwp_xfer(uint8_t out)
{
	uint8_t in = 0xFF;

	if (cs_high)                   /* clocking with CS high: meaningless */
		return 0xFF;

	if (phase == 0) {
		op = out;
		seen[op] = 1;
		seen_run[op] = 1;
		if (!is_allowed(op))
			forbid(op);
		phase = 1;
		switch (op) {
		case FW_CMD_WREN:
			wel = 1;
			break;
		case FW_CMD_RST:
			/* 0x99 after 0x66 clears WEL on real parts */
			wel = 0;
			break;
		}
		return 0xFF;
	}

	switch (op) {
	case FW_CMD_RDSR1:
		return (uint8_t)((wel ? 2 : 0) | (protect ? 0x1C : 0));
	case FW_CMD_RDSR2:
		return 0x00;
	case FW_CMD_JEDEC:
		in = (uint8_t)(FW_JEDEC_W25Q16 >> (8 * (3 - phase)));
		phase++;
		return in;
	case FW_CMD_READ:
		if (phase <= 3) {
			addr = (addr << 8) | out;
			phase++;
			return 0xFF;
		}
		in = flash[addr % SIZE];
		addr++;
		return in;
	case FW_CMD_SE4K:
		if (phase <= 3) {
			addr = (addr << 8) | out;
			phase++;
			if (phase == 4 && wel && !protect) {
				uint32_t base = addr & ~(FW_SECTOR - 1u);
				memset(flash + (base % SIZE), 0xFF, FW_SECTOR);
				wel = 0;
			}
		}
		return 0xFF;
	case FW_CMD_PP:
		if (phase <= 3) {
			addr = (addr << 8) | out;
			phase++;
			if (phase == 4)
				pp_base = addr & ~(FW_PAGE - 1u);
			return 0xFF;
		}
		if (wel && !protect) {
			/* program = AND; address wraps within the page */
			uint32_t a = pp_base | (addr & (FW_PAGE - 1u));
			flash[a % SIZE] &= out;
			addr++;
		}
		return 0xFF;
	default:
		/* EXITCRM/RSTEN/RST/RELPD: no data phase of interest */
		return 0xFF;
	}
}
