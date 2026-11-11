/* pb.h - minimal hand-written protobuf (proto3) encoder/decoder for the
 * ESPHome native API.  No allocation; writers work on caller buffers and
 * latch an overflow flag instead of writing past the end. */
#ifndef ESPHOME_PB_H
#define ESPHOME_PB_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Wire types */
#define PB_WT_VARINT 0
#define PB_WT_I64    1
#define PB_WT_LEN    2
#define PB_WT_I32    5

typedef struct {
	uint8_t *buf;
	size_t cap;
	size_t len;
	bool overflow;
} pb_writer;

void pb_writer_init(pb_writer *w, uint8_t *buf, size_t cap);
void pb_put_varint(pb_writer *w, uint64_t v);
size_t pb_varint_size(uint64_t v);
size_t pb_encode_varint(uint8_t *out, uint64_t v);   /* out >= 10 bytes */

/* Field writers.  Like proto3 / ESPHome, default values (0, false, "",
 * 0.0f) are omitted, except via the *_force variants. */
void pb_w_uint32(pb_writer *w, uint32_t field, uint32_t v);
void pb_w_int32(pb_writer *w, uint32_t field, int32_t v);
void pb_w_bool(pb_writer *w, uint32_t field, bool v);
void pb_w_fixed32_force(pb_writer *w, uint32_t field, uint32_t v);
void pb_w_float(pb_writer *w, uint32_t field, float v);
void pb_w_string(pb_writer *w, uint32_t field, const char *s);
void pb_w_bytes(pb_writer *w, uint32_t field, const void *data, size_t len);

/* Reader */
typedef struct {
	const uint8_t *p;
	const uint8_t *end;
} pb_reader;

typedef struct {
	uint32_t field;
	uint8_t wt;
	uint64_t u64;             /* VARINT, I64, I32 (zero-extended) */
	const uint8_t *data;      /* LEN */
	size_t len;               /* LEN */
} pb_field;

void pb_reader_init(pb_reader *r, const uint8_t *buf, size_t len);
/* 1 = field read, 0 = end of message, -1 = malformed */
int pb_next(pb_reader *r, pb_field *f);
int pb_read_varint(const uint8_t **p, const uint8_t *end, uint64_t *out);

/* Copy a LEN field into a NUL-terminated buffer (truncating). */
void pb_field_str(const pb_field *f, char *out, size_t outsz);
/* Compare a LEN field with a C string. */
bool pb_field_eq(const pb_field *f, const char *s);

static inline uint32_t pb_load_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}
#endif
