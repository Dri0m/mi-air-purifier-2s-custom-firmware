/* rng_posix.c - api_platform_entropy() for host builds: the OS CSPRNG
 * (arc4random_buf on macOS/BSD, /dev/urandom elsewhere). */
#include <stdio.h>
#include <stdlib.h>
#include "esphome_api.h"

int api_platform_entropy(void *buf, size_t len)
{
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
	arc4random_buf(buf, len);
	return 0;
#else
	FILE *f = fopen("/dev/urandom", "rb");
	size_t n;
	if (!f)
		return -1;
	n = fread(buf, 1, len, f);
	fclose(f);
	return n == len ? 0 : -1;
#endif
}
