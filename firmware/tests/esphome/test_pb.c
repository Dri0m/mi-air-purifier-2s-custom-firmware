/* test_pb.c - unit tests for the hand-written protobuf codec. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pb.h"

int main(void)
{
	uint8_t buf[64];
	pb_writer w;
	pb_reader r;
	pb_field f;

	/* varints */
	assert(pb_encode_varint(buf, 300) == 2 && buf[0] == 0xac && buf[1] == 0x02);
	assert(pb_varint_size(0) == 1 && pb_varint_size(0xffffffffffffffffull) == 10);

	pb_writer_init(&w, buf, sizeof(buf));
	pb_w_uint32(&w, 1, 150);                 /* 08 96 01 */
	pb_w_int32(&w, 2, -1);                   /* 10 ff x9 01 (10-byte varint) */
	pb_w_bool(&w, 3, false);                 /* omitted */
	pb_w_fixed32_force(&w, 4, 0);            /* 25 00000000 */
	pb_w_float(&w, 5, 21.5f);                /* 2d 0000ac41 */
	pb_w_string(&w, 6, "hi");                /* 32 02 'h' 'i' */
	pb_w_string(&w, 7, "");                  /* omitted */
	assert(!w.overflow);
	assert(w.len == 3 + 11 + 5 + 5 + 4);
	assert(!memcmp(buf, "\x08\x96\x01\x10\xff\xff\xff\xff\xff\xff\xff\xff\xff\x01", 14));
	assert(!memcmp(buf + 14, "\x25\x00\x00\x00\x00\x2d\x00\x00\xac\x41\x32\x02hi", 14));

	pb_reader_init(&r, buf, w.len);
	assert(pb_next(&r, &f) == 1 && f.field == 1 && f.wt == 0 && f.u64 == 150);
	assert(pb_next(&r, &f) == 1 && f.field == 2 && (int32_t)f.u64 == -1);
	assert(pb_next(&r, &f) == 1 && f.field == 4 && f.wt == 5 && f.u64 == 0);
	assert(pb_next(&r, &f) == 1 && f.field == 5 && f.u64 == 0x41ac0000u);
	assert(pb_next(&r, &f) == 1 && f.field == 6 && pb_field_eq(&f, "hi"));
	assert(pb_next(&r, &f) == 0);

	/* truncated LEN field, field number 0, writer overflow */
	pb_reader_init(&r, (const uint8_t *)"\x0a\x05zz", 4);
	assert(pb_next(&r, &f) == -1);
	pb_reader_init(&r, (const uint8_t *)"\x00\x01", 2);
	assert(pb_next(&r, &f) == -1);
	pb_writer_init(&w, buf, 3);
	pb_w_string(&w, 1, "toolong");
	assert(w.overflow && w.len <= 3);
	printf("test_pb: OK\n");
	return 0;
}
