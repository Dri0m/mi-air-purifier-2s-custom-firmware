/* hal_proto.c - pure-C protocol helpers (see hal_proto.h). */
#include "hal_proto.h"

/* ---------------------------------------------------------------- PM */

void pm_parser_reset(pm_parser *p)
{
	p->len = 0;
}

static bool pm_is_header0(uint8_t b)
{
	return b == 0x42 || b == 0x43 || b == 0x44; /* 'B' 'C' 'D' */
}

static bool pm_frame_valid(const uint8_t *f)
{
	uint16_t sum = 0;
	int i;

	if (!pm_is_header0(f[0]) || f[1] != 0x4D)
		return false;
	if (f[2] != 0x00 || f[3] != 0x1C) /* length field = 28 */
		return false;
	for (i = 0; i < 30; i++)
		sum = (uint16_t)(sum + f[i]);
	return sum == (uint16_t)((f[30] << 8) | f[31]);
}

bool pm_parser_feed(pm_parser *p, uint8_t byte, pm_frame *out)
{
	/* Header sync: byte 0 must be B/C/D, byte 1 must be 'M'. */
	if (p->len == 0) {
		if (pm_is_header0(byte))
			p->buf[p->len++] = byte;
		return false;
	}
	if (p->len == 1) {
		if (byte == 0x4D) {
			p->buf[p->len++] = byte;
		} else {
			p->len = 0;
			if (pm_is_header0(byte))
				p->buf[p->len++] = byte;
		}
		return false;
	}
	p->buf[p->len++] = byte;
	if (p->len < PM_FRAME_LEN)
		return false;

	if (pm_frame_valid(p->buf)) {
		out->header = p->buf[0];
		out->pm25_raw = (uint16_t)((p->buf[6] << 8) | p->buf[7]);
		p->len = 0;
		return true;
	}

	/* Bad frame: rescan the buffered bytes (after byte 0) for the next
	 * header candidate so a misaligned start does not cost a frame. */
	{
		uint8_t i, n = 0;
		uint8_t tmp[PM_FRAME_LEN];
		for (i = 1; i < PM_FRAME_LEN; i++) {
			if (n == 0 && !pm_is_header0(p->buf[i]))
				continue;
			if (n == 1 && p->buf[i] != 0x4D) {
				n = 0;
				if (!pm_is_header0(p->buf[i]))
					continue;
			}
			tmp[n++] = p->buf[i];
		}
		for (i = 0; i < n; i++)
			p->buf[i] = tmp[i];
		p->len = n;
	}
	return false;
}

size_t pm_build_cmd(uint8_t header, uint8_t arg, uint8_t out[7])
{
	uint16_t cs;
	int i;

	out[0] = header;
	out[1] = 0x4D;
	out[2] = 0x59;
	out[3] = 0x00;
	out[4] = arg;
	cs = 0;
	for (i = 0; i < 5; i++)
		cs = (uint16_t)(cs + out[i]);
	out[5] = (uint8_t)(cs >> 8);
	out[6] = (uint8_t)cs;
	return 7;
}

/* --------------------------------------------------------------- SHT */

uint8_t sht_crc8(const uint8_t *data, size_t len)
{
	uint8_t crc = 0xFF;
	size_t i;
	int b;

	for (i = 0; i < len; i++) {
		crc ^= data[i];
		for (b = 0; b < 8; b++)
			crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31)
					   : (uint8_t)(crc << 1);
	}
	return crc;
}

bool sht_decode(const uint8_t raw[6], int16_t *temp_dC, uint8_t *rh)
{
	uint32_t t, h;

	if (sht_crc8(&raw[0], 2) != raw[2] || sht_crc8(&raw[3], 2) != raw[5])
		return false;
	t = (uint32_t)((raw[0] << 8) | raw[1]);
	h = (uint32_t)((raw[3] << 8) | raw[4]);
	if (t == 0xFFFF || h == 0xFFFF)
		return false;
	*temp_dC = (int16_t)((int32_t)(t * 1750u / 65535u) - 450);
	h = h * 100u / 65535u;
	*rh = (uint8_t)(h > 100 ? 100 : h);
	return true;
}

/* -------------------------------------------------------------- OLED */

void oled_row_1to4(const uint8_t *src, uint8_t *dst, uint16_t width,
		   uint8_t on_level, bool high_nibble_first, bool mirror_x)
{
	uint16_t x;
	uint8_t on = on_level & 0x0F;

	for (x = 0; x < width; x += 2) {
		uint16_t xa = mirror_x ? (uint16_t)(width - 1 - x) : x;
		uint16_t xb = mirror_x ? (uint16_t)(width - 2 - x)
				       : (uint16_t)(x + 1);
		uint8_t a = (src[xa >> 3] >> (7 - (xa & 7))) & 1;
		uint8_t b = (src[xb >> 3] >> (7 - (xb & 7))) & 1;
		uint8_t first = a ? on : 0, second = b ? on : 0;
		dst[x >> 1] = high_nibble_first
			? (uint8_t)((first << 4) | second)
			: (uint8_t)((second << 4) | first);
	}
}

/* --------------------------------------------------------------- Fan */

uint32_t fan_half_period(uint32_t timer_hz, uint16_t rpm)
{
	uint32_t f = (uint32_t)(rpm >> 2); /* stock: MOTOR_Set(0, rpm >> 2) */

	if (f == 0)
		return 0;
	return timer_hz / (2u * f);
}

uint16_t fan_rpm_from_count(uint32_t edges, uint32_t elapsed_ms,
			    uint32_t pulses_per_rev)
{
	uint64_t rpm;

	if (elapsed_ms == 0 || pulses_per_rev == 0)
		return 0;
	rpm = (uint64_t)edges * 60000u / ((uint64_t)elapsed_ms * pulses_per_rev);
	return (uint16_t)(rpm > 0xFFFF ? 0xFFFF : rpm);
}
