/* rng.c - small CSPRNG for Noise ephemeral keys.
 *
 * Pool: 32-byte SHA-256 state.  Inputs: api_platform_entropy() at init and
 * before every output (raw platform material, always hashed, never used
 * directly), secret/unique extra data at init (the PSK, the MAC), cheap
 * per-packet timing events, and public per-handshake data (for uniqueness
 * only).  Output: ChaCha20 keystream under a key derived from the pool,
 * then the pool is ratcheted forward ("fast key erasure"), so a later RAM
 * read does not reveal earlier ephemeral keys. */
#include <string.h>
#include "esphome_api.h"
#include "noise.h"
#include "crypto/monocypher/monocypher.h"

static uint8_t pool[32];
static uint32_t events[8];
static uint32_t nevents;
static uint64_t counter;
static int seeded;

static void absorb(const char *tag, const void *d, size_t n)
{
	uint8_t tmp[32 + sizeof(events) + 4];
	memcpy(tmp, pool, 32);
	memcpy(tmp + 32, events, sizeof(events));
	memcpy(tmp + 32 + sizeof(events), tag, 4);
	noise_sha256(pool, tmp, sizeof(tmp), d, n);
	memset(events, 0, sizeof(events));
	crypto_wipe(tmp, sizeof(tmp));
}

static int absorb_platform(size_t n)
{
	uint8_t raw[64];
	if (n > sizeof(raw))
		n = sizeof(raw);
	if (api_platform_entropy(raw, n) != 0)
		return -1;
	absorb("plat", raw, n);
	crypto_wipe(raw, sizeof(raw));
	return 0;
}

int noise_rng_init(const void *extra, size_t extra_len)
{
	if (absorb_platform(64) != 0)
		return -1;
	absorb("xtra", extra, extra_len);
	seeded = 1;
	return 0;
}

void noise_rng_event(void)
{
	uint32_t v = 0;
	if (api_platform_entropy(&v, sizeof(v)) != 0)
		return;
	/* cheap fold; hashed into the pool on the next absorb() */
	events[nevents & 7] = (events[nevents & 7] << 7 | events[nevents & 7] >> 25) ^ v;
	nevents++;
}

void noise_rng_mix(const void *d, size_t n)
{
	absorb("pub_", d, n);
}

int noise_rng_bytes(void *out, size_t n)
{
	uint8_t key[32], nonce[8];
	if (!seeded || absorb_platform(32) != 0)
		return -1;
	for (int i = 0; i < 8; i++)
		nonce[i] = (uint8_t)(counter >> (8 * i));
	counter++;
	noise_sha256(key, "out_", 4, pool, 32);
	crypto_chacha20_djb(out, NULL, n, key, nonce, 0);
	noise_sha256(pool, "next", 4, pool, 32);      /* ratchet */
	crypto_wipe(key, sizeof(key));
	return 0;
}
