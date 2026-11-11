/* noise.c - Noise_NNpsk0_25519_ChaChaPoly_SHA256 responder for the ESPHome
 * API, see noise.h.  Follows the Noise spec rev 34 (sections 5, 9, 12.3)
 * exactly as noise-c behaves for ESPHome's NoiseResponderHandshake. */
#include <string.h>
#include "noise.h"
#include "crypto/monocypher/monocypher.h"
#define MBEDTLS_CONFIG_FILE "esphome_mbedtls_config.h"
#include "crypto/mbedtls/sha256.h"

void noise_wipe(void *p, size_t n) { crypto_wipe(p, n); }

/* ---------------- SHA-256 / HMAC / HKDF ---------------- */

static void sha_begin(mbedtls_sha256_context *c)
{
	mbedtls_sha256_init(c);
	mbedtls_sha256_starts_ret(c, 0);
}

static void sha_add(mbedtls_sha256_context *c, const void *d, size_t n)
{
	if (n)
		mbedtls_sha256_update_ret(c, d, n);
}

static void sha_end(mbedtls_sha256_context *c, uint8_t out[32])
{
	mbedtls_sha256_finish_ret(c, out);
	mbedtls_sha256_free(c);
}

void noise_sha256(uint8_t out[32], const void *a, size_t alen, const void *b, size_t blen)
{
	mbedtls_sha256_context c;
	sha_begin(&c);
	sha_add(&c, a, alen);
	sha_add(&c, b, blen);
	sha_end(&c, out);
}

void noise_hmac_sha256(uint8_t out[32], const uint8_t *key, size_t keylen,
		       const void *a, size_t alen, const void *b, size_t blen)
{
	mbedtls_sha256_context c;
	uint8_t pad[64], k[32], inner[32];
	if (keylen > 64) {
		noise_sha256(k, key, keylen, NULL, 0);
		key = k;
		keylen = 32;
	}
	memset(pad, 0x36, sizeof(pad));
	for (size_t i = 0; i < keylen; i++)
		pad[i] ^= key[i];
	sha_begin(&c);
	sha_add(&c, pad, 64);
	sha_add(&c, a, alen);
	sha_add(&c, b, blen);
	sha_end(&c, inner);
	for (size_t i = 0; i < 64; i++)
		pad[i] ^= 0x36 ^ 0x5c;
	sha_begin(&c);
	sha_add(&c, pad, 64);
	sha_add(&c, inner, 32);
	sha_end(&c, out);
	crypto_wipe(pad, sizeof(pad));
	crypto_wipe(inner, sizeof(inner));
	crypto_wipe(k, sizeof(k));
}

/* Noise HKDF(chaining_key, ikm) -> 2 or 3 outputs (outputs may alias ck). */
static void hkdf(const uint8_t ck[32], const uint8_t *ikm, size_t ikm_len,
		 uint8_t o1[32], uint8_t o2[32], uint8_t *o3)
{
	uint8_t temp[32], t1[32], t2[32];
	static const uint8_t one = 1, two = 2, three = 3;
	noise_hmac_sha256(temp, ck, 32, ikm, ikm_len, NULL, 0);
	noise_hmac_sha256(t1, temp, 32, &one, 1, NULL, 0);
	noise_hmac_sha256(t2, temp, 32, t1, 32, &two, 1);
	if (o3)
		noise_hmac_sha256(o3, temp, 32, t2, 32, &three, 1);
	memcpy(o1, t1, 32);
	memcpy(o2, t2, 32);
	crypto_wipe(temp, sizeof(temp));
	crypto_wipe(t1, sizeof(t1));
	crypto_wipe(t2, sizeof(t2));
}

/* ---------------- ChaCha20-Poly1305 (RFC 8439 2.8) ---------------- */

static void poly_mac(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *ad,
		     size_t adlen, const uint8_t *ct, size_t len, uint8_t mac[16])
{
	static const uint8_t zero[16];
	uint8_t pk[64], lens[16];
	crypto_poly1305_ctx p;

	crypto_chacha20_ietf(pk, NULL, 64, key, nonce, 0);      /* block 0 */
	crypto_poly1305_init(&p, pk);
	crypto_poly1305_update(&p, ad, adlen);
	crypto_poly1305_update(&p, zero, (16 - (adlen & 15)) & 15);
	crypto_poly1305_update(&p, ct, len);
	crypto_poly1305_update(&p, zero, (16 - (len & 15)) & 15);
	for (int i = 0; i < 8; i++) {
		lens[i] = (uint8_t)((uint64_t)adlen >> (8 * i));
		lens[8 + i] = (uint8_t)((uint64_t)len >> (8 * i));
	}
	crypto_poly1305_update(&p, lens, 16);
	crypto_poly1305_final(&p, mac);
	crypto_wipe(pk, sizeof(pk));
}

void noise_aead_encrypt(const uint8_t key[32], const uint8_t nonce[12],
			const uint8_t *ad, size_t adlen, uint8_t *buf, size_t len,
			uint8_t tag[16])
{
	crypto_chacha20_ietf(buf, buf, len, key, nonce, 1);
	poly_mac(key, nonce, ad, adlen, buf, len, tag);
}

int noise_aead_decrypt(const uint8_t key[32], const uint8_t nonce[12],
		       const uint8_t *ad, size_t adlen, uint8_t *buf, size_t len,
		       const uint8_t tag[16])
{
	uint8_t mac[16];
	int bad;
	poly_mac(key, nonce, ad, adlen, buf, len, mac);
	bad = crypto_verify16(mac, tag);
	crypto_wipe(mac, sizeof(mac));
	if (bad)
		return -1;
	crypto_chacha20_ietf(buf, buf, len, key, nonce, 1);
	return 0;
}

static void make_nonce(uint8_t nonce[12], uint64_t n)
{
	memset(nonce, 0, 4);
	for (int i = 0; i < 8; i++)
		nonce[4 + i] = (uint8_t)(n >> (8 * i));
}

size_t noise_encrypt(noise_cipher *c, uint8_t *buf, size_t len)
{
	uint8_t nonce[12];
	make_nonce(nonce, c->n++);
	noise_aead_encrypt(c->k, nonce, NULL, 0, buf, len, buf + len);
	return len + NOISE_MAC_LEN;
}

int noise_decrypt(noise_cipher *c, uint8_t *buf, size_t len)
{
	uint8_t nonce[12];
	if (len < NOISE_MAC_LEN)
		return -1;
	make_nonce(nonce, c->n);
	if (noise_aead_decrypt(c->k, nonce, NULL, 0, buf, len - NOISE_MAC_LEN,
			       buf + len - NOISE_MAC_LEN))
		return -1;
	c->n++;
	return (int)(len - NOISE_MAC_LEN);
}

/* ---------------- handshake ---------------- */

typedef struct {
	uint8_t ck[32], h[32], k[32];
} symstate;

static void mix_hash(symstate *s, const void *d, size_t n)
{
	noise_sha256(s->h, s->h, 32, d, n);
}

static void mix_key(symstate *s, const uint8_t *ikm, size_t n)
{
	hkdf(s->ck, ikm, n, s->ck, s->k, NULL);       /* k's nonce restarts at 0 */
}

static void mix_key_and_hash(symstate *s, const uint8_t *ikm, size_t n)
{
	uint8_t th[32];
	hkdf(s->ck, ikm, n, s->ck, th, s->k);
	mix_hash(s, th, 32);
	crypto_wipe(th, sizeof(th));
}

void noise_prologue_hash(uint8_t h[32], const uint8_t *hello, size_t hello_len)
{
	static const char name[] = NOISE_PROTOCOL_NAME;    /* 36 > 32: hashed */
	uint8_t h0[32], pro[12 + 2];
	mbedtls_sha256_context c;
	noise_sha256(h0, name, sizeof(name) - 1, NULL, 0);
	memcpy(pro, "NoiseAPIInit", 12);
	pro[12] = (uint8_t)(hello_len >> 8);
	pro[13] = (uint8_t)hello_len;
	sha_begin(&c);
	sha_add(&c, h0, 32);
	sha_add(&c, pro, sizeof(pro));
	sha_add(&c, hello, hello_len);
	sha_end(&c, h);
}

int noise_respond(const uint8_t h_prologue[32], const uint8_t psk[32],
		  const uint8_t *msg1, size_t msg1_len, uint8_t e_priv[32],
		  uint8_t msg2[NOISE_MSG2_LEN], noise_cipher *send, noise_cipher *recv)
{
	static const char name[] = NOISE_PROTOCOL_NAME;
	static const uint8_t zero_nonce[12];
	symstate s;
	uint8_t dh[32], acc = 0;
	const uint8_t *re = msg1;
	int rc = NOISE_ERR;

	if (msg1_len < NOISE_MSG1_MIN)
		goto out;
	noise_sha256(s.ck, name, sizeof(name) - 1, NULL, 0);   /* ck = h0 */
	memcpy(s.h, h_prologue, 32);

	/* -> psk, e  (+ encrypted payload, ignored) */
	mix_key_and_hash(&s, psk, 32);
	mix_hash(&s, re, 32);
	mix_key(&s, re, 32);                     /* PSK modifier: MixKey(e) */
	{
		size_t ct_len = msg1_len - 32 - NOISE_MAC_LEN;
		const uint8_t *ct = msg1 + 32;
		uint8_t mac[16];
		/* verify only; the initiator's payload is unused (and empty) */
		poly_mac(s.k, zero_nonce, s.h, 32, ct, ct_len, mac);
		if (crypto_verify16(mac, ct + ct_len)) {
			rc = NOISE_ERR_MAC;
			goto out;
		}
		mix_hash(&s, ct, ct_len + NOISE_MAC_LEN);
	}

	/* <- e, ee */
	crypto_x25519_public_key(msg2, e_priv);
	mix_hash(&s, msg2, 32);
	mix_key(&s, msg2, 32);
	crypto_x25519(dh, e_priv, re);
	for (int i = 0; i < 32; i++)
		acc |= dh[i];
	if (!acc)                                /* low-order point */
		goto out;
	mix_key(&s, dh, 32);
	noise_aead_encrypt(s.k, zero_nonce, s.h, 32, msg2 + 32, 0, msg2 + 32);
	mix_hash(&s, msg2 + 32, NOISE_MAC_LEN);  /* (h unused after this) */

	/* Split(): c1 = initiator->responder, c2 = responder->initiator */
	hkdf(s.ck, NULL, 0, recv->k, send->k, NULL);
	recv->n = send->n = 0;
	rc = NOISE_OK;
out:
	crypto_wipe(&s, sizeof(s));
	crypto_wipe(dh, sizeof(dh));
	crypto_wipe(e_priv, 32);
	return rc;
}

/* ---------------- base64 ---------------- */

static int b64v(char c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

int noise_psk_from_base64(const char *b64, uint8_t psk[32])
{
	uint32_t acc = 0;
	int bits = 0, n = 0, pad = 0;
	if (!b64)
		return -1;
	for (; *b64; b64++) {
		char ch = *b64;
		int v;
		if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
			continue;
		if (ch == '=') {
			pad++;
			continue;
		}
		if (pad || (v = b64v(ch)) < 0)
			return -1;       /* data after padding / bad char */
		acc = (acc << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (n >= 32)
				return -1;
			psk[n++] = (uint8_t)(acc >> bits);
		}
	}
	/* 32 bytes = 43 chars + "=": 2 leftover bits, which must be zero */
	if (n != 32 || pad != 1 || bits != 2 || (acc & 3))
		return -1;
	return 0;
}
