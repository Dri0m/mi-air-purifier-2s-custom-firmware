/* store.c - settings EEPROM in the STOCK layout (fw-app-logic.md 6.2,
 * fw-hardware-map.md 8), plus settings.h.
 *
 * Every element is a little-endian int32 at page*32+byte; no element
 * crosses a 32-byte page.  The only integrity marks stock has are the
 * magic "smartmi_air_mc1" (p1) and the layout version 0x5272 (p15 b4), so
 * validation is: magic missing -> write all defaults (as stock does);
 * version mismatch -> rewrite the speed table; per-field range checks ->
 * default in RAM (written back on the next flush).  A CRC of our own is
 * deliberately not added: stock would not maintain it.
 *
 * Writes: a RAM shadow holds what the EEPROM contains; store_flush()
 * writes only elements whose value differs from it.
 *
 * Our own settings (not stock) live outside every stock location, in
 * page 64 (0x800..0x81f), with the same int32 LE encoding:
 *   0x800  auto_bias  int32 -50..50, multiple of 10; default 0
 * Stock's location table (.data 0x20000fac, ids 0x00..0x7c) maps only to
 * pages 2..22 and 255 (0x1fe0); the real stock dump of this unit
 * (evidence/dumps/eeprom-20261104-diag1b.txt) additionally has bytes in
 * pages 0, 1 (magic), 32 (0x406) and 48 (0x600), and page 64 erased.  An
 * own element that is erased (0xffffffff) or invalid reads as its
 * default and is NOT written back (F_OWN) on load: the first write
 * happens when the user changes the setting, or on a data reset (which
 * writes the default 0, a valid value).
 */
#include <string.h>
#include "hal.h"
#include "settings.h"
#include "pur_int.h"

#define MAGIC_ADDR  0x020
#define COLA0_ADDR  0x2a0
#define COLA1_ADDR  0x2c0
static const char magic[16] = "smartmi_air_mc1";

#define F_RANGE   0x01             /* validate against min..max */
#define F_KEEP    0x02             /* kept by a data reset (stock WriteAll
				    * 1f016874 skips elems 0x72 and 0x1e) */
#define F_SPEED   0x04             /* rewritten on layout mismatch */
#define F_OWN     0x08             /* ours: signed min..max, step 10;
				    * invalid -> default, not written back */

static const struct {
	uint16_t addr;
	uint8_t flags;
	uint32_t def, min, max;
} tab[E_COUNT] = {
	[E_MODE]       = { 0x040, F_RANGE, 0, 0, 7 },
	[E_APP_EXTRA]  = { 0x04c, 0, 0, 0, 0 },
	[E_LED]        = { 0x050, F_RANGE, 1, 0, 1 },
	[E_DEBUG]      = { 0x054, F_KEEP, 0, 0, 0 },
	[E_POWER]      = { 0x1e0, F_RANGE, 0, 0, 1 },
	[E_LAYOUT]     = { 0x1e4, 0, STORE_LAYOUT_VERSION, 0, 0 },
	[E_CHILD]      = { 0x1e8, F_RANGE, 0, 0, 1 },
	[E_CHAN]       = { 0x1ec, 0, 0, 0, 0 },
	[E_COUNTRY]    = { 0x1f0, 0, 91, 0, 0 },
	[E_GOODH]      = { 0x1f4, F_RANGE, PUR_GOODH_DEFAULT, 36, 114 },
	[E_RFID]       = { 0x1f8, F_RANGE, 0, 0, 1 },
	[E_SPD_SILENT] = { 0x200, F_RANGE | F_SPEED, 350, 300, 2100 },
	[E_SPD_LOW]    = { 0x204, F_RANGE | F_SPEED, 660, 300, 2100 },
	[E_SPD_MED_L]  = { 0x208, F_RANGE | F_SPEED, 1030, 300, 2100 },
	[E_SPD_MED]    = { 0x20c, F_RANGE | F_SPEED, 1250, 300, 2100 },
	[E_SPD_HIGH]   = { 0x210, F_RANGE | F_SPEED, 1410, 300, 2100 },
	[E_SPD_STRONG] = { 0x214, F_RANGE | F_SPEED, 1700, 300, 2100 },
	[E_SPD_FAV]    = { 0x218, F_RANGE | F_SPEED, 1700, 300, 2100 },
	[E_HWVER]      = { 0x240, 0, 0, 0, 0 },
	[E_FUSED]      = { 0x244, F_RANGE, 0, 0, 0x7fffffff },
	[E_AQI_SUM]    = { 0x24c, 0, 0, 0, 0 },
	[E_AQI_CNT]    = { 0x250, 0, 0, 0, 0 },
	[E_DATA_RESET] = { 0x254, F_RANGE, 0, 0, 1 },
	[E_SLAVE_CHAN] = { 0x278, F_RANGE | F_KEEP, 4, 3, 4 },
	[E_F1_HOUR]    = { 0x298, F_RANGE, 3500, 2000, 6000 },
	[E_BUZZER]     = { 0x1fe0, F_RANGE, 1, 0, 1 },
	[E_AUTO_BIAS]  = { 0x800, F_OWN, 0, (uint32_t)AUTO_BIAS_MIN,
			   AUTO_BIAS_MAX },
};

static uint32_t val[E_COUNT];      /* desired values */
static uint32_t shadow[E_COUNT];   /* EEPROM content */
static bool shadow_ok[E_COUNT];    /* shadow known (read or written) */
static bool io_ok;                 /* last init read the EEPROM */
static bool write_magic;           /* magic/cola still to be written */
static bool write_cola;            /* cola only (data reset) */
static bool foreign;               /* magic unreadable/garbled: never write */

static uint32_t le32(const uint8_t *b)
{
	return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 |
	       (uint32_t)b[3] << 24;
}

static bool own_valid(int e, uint32_t v)
{
	int32_t x = (int32_t)v;
	return x >= (int32_t)tab[e].min && x <= (int32_t)tab[e].max &&
	       x % AUTO_BIAS_STEP == 0;
}

uint16_t store_addr(store_elem e) { return tab[e].addr; }
uint32_t store_get(store_elem e) { return val[e]; }
void store_set(store_elem e, uint32_t v) { val[e] = v; }

void store_defaults(bool data_reset)
{
	for (int e = 0; e < E_COUNT; e++)
		if (!data_reset || !(tab[e].flags & F_KEEP))
			val[e] = tab[e].def;
	if (data_reset)
		write_cola = true;
}

int store_init(void)
{
	char m[16];
	int rc = STORE_OK;

	memset(shadow_ok, 0, sizeof shadow_ok);
	write_magic = false;
	write_cola = false;
	foreign = false;
	store_defaults(false);
	io_ok = hal_eeprom_read(MAGIC_ADDR, m, sizeof m) == 0;
	if (!io_ok)
		return STORE_ERR_IO;
	if (memcmp(m, magic, sizeof m) != 0) {
		/* Deviation from stock (which formats on any mismatch): a single
		 * I2C misread must not wipe the stock filter hours/settings.
		 * Re-read; format only a genuinely blank (all 0xFF) chip.  Any
		 * other content: run on RAM defaults and never write. */
		bool blank = true, good = false;
		for (int tries = 0; tries < 3 && !good; tries++) {
			if (hal_eeprom_read(MAGIC_ADDR, m, sizeof m) != 0)
				continue;
			good = memcmp(m, magic, sizeof m) == 0;
			for (unsigned i = 0; i < sizeof m; i++)
				if ((uint8_t)m[i] != 0xFF)
					blank = false;
		}
		if (!good) {
			if (!blank) {
				foreign = true;
				return STORE_ERR_MAGIC;
			}
			write_magic = true;
			store_flush();
			return STORE_ERR_MAGIC;
		}
	}
	for (int e = 0; e < E_COUNT; e++) {
		uint8_t b[4];
		if (hal_eeprom_read(tab[e].addr, b, 4) != 0) {
			io_ok = false;
			store_defaults(false);
			memset(shadow_ok, 0, sizeof shadow_ok);
			return STORE_ERR_IO;
		}
		shadow[e] = val[e] = le32(b);
		shadow_ok[e] = true;
	}
	/* DataResetSet (elem 0x7c) = 1: stock AIR_Var_Init 1f016a7c calls
	 * AIR_Var_WriteAll 1f016874 at boot, which writes every default
	 * (country 91, channel 0, hw_ver 0, cola '0'x16, filter and AQI
	 * totals 0, flag 0) except slave_channel and the debug flag. */
	bool data_reset = val[E_DATA_RESET] == 1;
	if (data_reset) {
		store_defaults(true);
		rc = STORE_FIXED;
	}
	if (val[E_LAYOUT] != STORE_LAYOUT_VERSION) {
		for (int e = 0; e < E_COUNT; e++)
			if (tab[e].flags & F_SPEED)
				val[e] = tab[e].def;
		val[E_LAYOUT] = STORE_LAYOUT_VERSION;
		rc = STORE_FIXED;
	}
	for (int e = 0; e < E_COUNT; e++) {
		if ((tab[e].flags & F_RANGE) &&
		    (val[e] < tab[e].min || val[e] > tab[e].max)) {
			val[e] = tab[e].def;
			rc = STORE_FIXED;
		}
		if ((tab[e].flags & F_OWN) && !own_valid(e, val[e])) {
			/* erased/garbage: default, and treat the EEPROM as
			 * holding it, so nothing is written until a change */
			val[e] = shadow[e] = tab[e].def;
		}
	}
	if (data_reset)            /* stock writes the defaults right away */
		store_flush();
	return rc;
}

int store_flush(void)
{
	int rc = 0;

	if (foreign)               /* unrecognised content: read-only */
		return -1;
	if (!io_ok) {              /* EEPROM was unreadable: retry once */
		uint32_t keep[E_COUNT];
		memcpy(keep, val, sizeof keep);
		if (store_init() == STORE_ERR_IO)
			return -1;
		memcpy(val, keep, sizeof keep);
	}
	for (int e = 0; e < E_COUNT; e++) {
		if (shadow_ok[e] && shadow[e] == val[e])
			continue;
		uint8_t b[4] = { (uint8_t)val[e], (uint8_t)(val[e] >> 8),
				 (uint8_t)(val[e] >> 16), (uint8_t)(val[e] >> 24) };
		if (hal_eeprom_write(tab[e].addr, b, 4) == 0) {
			shadow[e] = val[e];
			shadow_ok[e] = true;
		} else {
			rc = -1;
		}
	}
	if ((write_magic || write_cola) && rc == 0) {
		/* magic last, so a partial default write is redone next boot */
		char cola[16];
		memset(cola, '0', sizeof cola);
		if (hal_eeprom_write(COLA0_ADDR, cola, 16) == 0 &&
		    hal_eeprom_write(COLA1_ADDR, cola, 16) == 0 &&
		    (!write_magic ||
		     hal_eeprom_write(MAGIC_ADDR, magic, 16) == 0))
			write_magic = write_cola = false;
		else
			rc = -1;
	}
	return rc;
}

/* ---------- settings.h ---------- */

int settings_load(settings_t *s)
{
	int rc = store_init();
	s->mode = (uint8_t)store_get(E_MODE);
	s->favorite_rpm = (uint16_t)store_get(E_SPD_FAV);
	s->silent_rpm = (uint16_t)store_get(E_SPD_SILENT);
	s->led = store_get(E_LED) != 0;
	s->buzzer = store_get(E_BUZZER) != 0;
	s->child_lock = store_get(E_CHILD) != 0;
	s->filter_usage = store_get(E_FUSED);
	s->power_restore = store_get(E_POWER) != 0;
	return rc == STORE_OK ? 0 : (rc == STORE_FIXED ? 0 : rc);
}

int settings_save(const settings_t *s)
{
	store_set(E_MODE, s->mode);
	store_set(E_SPD_FAV, s->favorite_rpm);
	store_set(E_SPD_SILENT, s->silent_rpm);
	store_set(E_LED, s->led);
	store_set(E_BUZZER, s->buzzer);
	store_set(E_CHILD, s->child_lock);
	store_set(E_FUSED, s->filter_usage);
	store_set(E_POWER, s->power_restore);
	return store_flush();
}
