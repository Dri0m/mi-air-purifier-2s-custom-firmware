/* pb.c - minimal protobuf codec, see pb.h */
#include "pb.h"
#include <string.h>

void pb_writer_init(pb_writer *w, uint8_t *buf, size_t cap)
{
	w->buf = buf;
	w->cap = cap;
	w->len = 0;
	w->overflow = false;
}

size_t pb_encode_varint(uint8_t *out, uint64_t v)
{
	size_t n = 0;
	while (v >= 0x80) {
		out[n++] = (uint8_t)(v | 0x80);
		v >>= 7;
	}
	out[n++] = (uint8_t)v;
	return n;
}

size_t pb_varint_size(uint64_t v)
{
	size_t n = 1;
	while (v >= 0x80) {
		v >>= 7;
		n++;
	}
	return n;
}

static void put_raw(pb_writer *w, const void *d, size_t n)
{
	if (w->overflow || w->cap - w->len < n) {
		w->overflow = true;
		return;
	}
	memcpy(w->buf + w->len, d, n);
	w->len += n;
}

void pb_put_varint(pb_writer *w, uint64_t v)
{
	uint8_t tmp[10];
	put_raw(w, tmp, pb_encode_varint(tmp, v));
}

static void put_tag(pb_writer *w, uint32_t field, uint8_t wt)
{
	pb_put_varint(w, ((uint64_t)field << 3) | wt);
}

void pb_w_uint32(pb_writer *w, uint32_t field, uint32_t v)
{
	if (!v)
		return;
	put_tag(w, field, PB_WT_VARINT);
	pb_put_varint(w, v);
}

void pb_w_int32(pb_writer *w, uint32_t field, int32_t v)
{
	if (!v)
		return;
	put_tag(w, field, PB_WT_VARINT);
	/* negative int32 is sign-extended to 64 bits (10-byte varint) */
	pb_put_varint(w, (uint64_t)(int64_t)v);
}

void pb_w_bool(pb_writer *w, uint32_t field, bool v)
{
	if (!v)
		return;
	put_tag(w, field, PB_WT_VARINT);
	pb_put_varint(w, 1);
}

static void put_le32(pb_writer *w, uint32_t v)
{
	uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
			 (uint8_t)(v >> 24) };
	put_raw(w, b, 4);
}

void pb_w_fixed32_force(pb_writer *w, uint32_t field, uint32_t v)
{
	put_tag(w, field, PB_WT_I32);
	put_le32(w, v);
}

void pb_w_float(pb_writer *w, uint32_t field, float v)
{
	uint32_t u;
	if (v == 0.0f)
		return;
	memcpy(&u, &v, 4);
	put_tag(w, field, PB_WT_I32);
	put_le32(w, u);
}

void pb_w_bytes(pb_writer *w, uint32_t field, const void *data, size_t len)
{
	if (!len)
		return;
	put_tag(w, field, PB_WT_LEN);
	pb_put_varint(w, len);
	put_raw(w, data, len);
}

void pb_w_string(pb_writer *w, uint32_t field, const char *s)
{
	if (s)
		pb_w_bytes(w, field, s, strlen(s));
}

void pb_reader_init(pb_reader *r, const uint8_t *buf, size_t len)
{
	r->p = buf;
	r->end = buf + len;
}

int pb_read_varint(const uint8_t **pp, const uint8_t *end, uint64_t *out)
{
	const uint8_t *p = *pp;
	uint64_t v = 0;
	unsigned shift = 0;
	while (p < end && shift < 64) {
		uint8_t b = *p++;
		v |= (uint64_t)(b & 0x7f) << shift;
		if (!(b & 0x80)) {
			*out = v;
			*pp = p;
			return 0;
		}
		shift += 7;
	}
	return -1;
}

int pb_next(pb_reader *r, pb_field *f)
{
	uint64_t tag;
	if (r->p >= r->end)
		return 0;
	if (pb_read_varint(&r->p, r->end, &tag) < 0)
		return -1;
	f->field = (uint32_t)(tag >> 3);
	f->wt = (uint8_t)(tag & 7);
	f->u64 = 0;
	f->data = NULL;
	f->len = 0;
	if (f->field == 0)
		return -1;
	switch (f->wt) {
	case PB_WT_VARINT:
		return pb_read_varint(&r->p, r->end, &f->u64) < 0 ? -1 : 1;
	case PB_WT_I64:
		if (r->end - r->p < 8)
			return -1;
		f->u64 = (uint64_t)pb_load_le32(r->p) |
			 ((uint64_t)pb_load_le32(r->p + 4) << 32);
		r->p += 8;
		return 1;
	case PB_WT_I32:
		if (r->end - r->p < 4)
			return -1;
		f->u64 = pb_load_le32(r->p);
		r->p += 4;
		return 1;
	case PB_WT_LEN: {
		uint64_t n;
		if (pb_read_varint(&r->p, r->end, &n) < 0)
			return -1;
		if (n > (uint64_t)(r->end - r->p))
			return -1;
		f->data = r->p;
		f->len = (size_t)n;
		r->p += n;
		return 1;
	}
	default:
		return -1;              /* groups are not used by api.proto */
	}
}

void pb_field_str(const pb_field *f, char *out, size_t outsz)
{
	size_t n = f->len;
	if (!outsz)
		return;
	if (n >= outsz)
		n = outsz - 1;
	if (n)
		memcpy(out, f->data, n);
	out[n] = 0;
}

bool pb_field_eq(const pb_field *f, const char *s)
{
	size_t n = strlen(s);
	return f->len == n && (n == 0 || memcmp(f->data, s, n) == 0);
}
