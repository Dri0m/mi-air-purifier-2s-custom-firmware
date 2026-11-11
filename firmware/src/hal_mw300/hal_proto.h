/* hal_proto.h - pure-C protocol helpers used by the MW300 HAL.
 *
 * No SDK or RTOS dependencies, so these can be unit-tested on the host:
 *   - Plantower-style 32-byte PM frame parser ('BM' / 'CM' / 'DM' headers)
 *   - PM sensor command frame builder (stock cmd 0x59)
 *   - Sensirion CRC-8 and SHT3x raw -> stock units conversion
 *   - 1 bpp -> 4 bpp (SSD1327) row conversion
 * Facts: evidence/fw-hardware-map.md sections 3, 4.3, 4.5, 6.2.
 */
#ifndef HAL_PROTO_H
#define HAL_PROTO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- PM sensor (UART2, 9600 8N1) ---- */
#define PM_FRAME_LEN 32

typedef struct {
	uint8_t buf[PM_FRAME_LEN];
	uint8_t len;
} pm_parser;

typedef struct {
	uint8_t header;        /* 'B' (0x42), 'C' (0x43) or 'D' (0x44) */
	uint16_t pm25_raw;     /* bytes 6-7 big-endian (Plantower PM2.5 CF=1) */
} pm_frame;

void pm_parser_reset(pm_parser *p);
/* Feed one byte.  Returns true when a complete frame with a valid
 * header, length field 0x001C and 16-bit sum checksum was assembled;
 * *out is then filled.  Resynchronises on garbage automatically. */
bool pm_parser_feed(pm_parser *p, uint8_t byte, pm_frame *out);

/* 7-byte command frame `hdr 4D 59 00 arg 00 cs` (cs = 16-bit sum of
 * bytes 0..4, big-endian in 5..6).  Stock sends arg 1 (auto/favorite)
 * or 0 (silent) on mode change.  Returns the frame length (7). */
size_t pm_build_cmd(uint8_t header, uint8_t arg, uint8_t out[7]);

/* ---- SHT3x ---- */
uint8_t sht_crc8(const uint8_t *data, size_t len); /* poly 0x31, init 0xFF */
/* Decode `T_hi T_lo CRC RH_hi RH_lo CRC`.  Checks both CRCs and rejects
 * raw 0xFFFF like stock.  temp in 0.1 C (stock formula
 * T*1750/65535 - 450), rh in integer %RH (RH*100/65535, clamped 100). */
bool sht_decode(const uint8_t raw[6], int16_t *temp_dC, uint8_t *rh);

/* ---- OLED ---- */
/* Convert one row of a row-major, MSB-first 1 bpp framebuffer (width
 * pixels, width/8 bytes) to SSD1327 4 bpp (width/2 bytes).  on_level is
 * the 4-bit grey for set pixels.  high_nibble_first: pixel x (even) goes
 * to bits 7:4 of the output byte.  mirror_x: reverse pixel order. */
void oled_row_1to4(const uint8_t *src, uint8_t *dst, uint16_t width,
		   uint8_t on_level, bool high_nibble_first, bool mirror_x);

/* ---- Fan ---- */
/* Half-period (ton = toff) in timer counts for the frequency-coded fan
 * command: f = (rpm >> 2) Hz, 50 % duty.  Returns 0 when the command
 * would be 0 Hz (stop). */
uint32_t fan_half_period(uint32_t timer_hz, uint16_t rpm);
/* rpm from FG edges counted over elapsed_ms (pulses_per_rev edges/rev). */
uint16_t fan_rpm_from_count(uint32_t edges, uint32_t elapsed_ms,
			    uint32_t pulses_per_rev);
#endif
