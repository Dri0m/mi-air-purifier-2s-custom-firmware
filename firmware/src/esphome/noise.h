/* noise.h - Noise_NNpsk0_25519_ChaChaPoly_SHA256 responder, as used by the
 * ESPHome native API encryption (esphome/components/noise + api_frame_helper_
 * noise.cpp, 2026.10.2; client: aioesphomeapi _frame_helper/noise.py).
 *
 * Primitives: X25519 + ChaCha20 + Poly1305 from Monocypher 4.0.2, SHA-256
 * from mbed TLS 2.28.1 (both vendored under crypto/).  HMAC, the Noise
 * HKDF, the RFC 8439 AEAD construction and the handshake are ours. */
#ifndef ESPHOME_NOISE_H
#define ESPHOME_NOISE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NOISE_PROTOCOL_NAME "Noise_NNpsk0_25519_ChaChaPoly_SHA256"
#define NOISE_MAC_LEN 16
#define NOISE_MSG1_MIN (32 + NOISE_MAC_LEN)  /* e + encrypted empty payload */
#define NOISE_MSG2_LEN (32 + NOISE_MAC_LEN)

/* return codes of noise_respond() */
#define NOISE_OK 0
#define NOISE_ERR_MAC (-1)      /* wrong PSK: "Handshake MAC failure" */
#define NOISE_ERR (-2)          /* anything else: "Handshake error" */

typedef struct {
	uint8_t k[32];
	uint64_t n;
} noise_cipher;

/* SHA-256 / HMAC-SHA256 (exposed for the test vectors) */
void noise_sha256(uint8_t out[32], const void *a, size_t alen, const void *b, size_t blen);
void noise_hmac_sha256(uint8_t out[32], const uint8_t *key, size_t keylen,
		       const void *a, size_t alen, const void *b, size_t blen);

/* RFC 8439 ChaCha20-Poly1305 AEAD, 96-bit nonce, in place.
 * encrypt: buf[len] -> ciphertext, tag written to tag[16].
 * decrypt: returns 0 and decrypts in place if the tag verifies, else -1
 * (buffer untouched). */
void noise_aead_encrypt(const uint8_t key[32], const uint8_t nonce[12],
			const uint8_t *ad, size_t adlen, uint8_t *buf, size_t len,
			uint8_t tag[16]);
int noise_aead_decrypt(const uint8_t key[32], const uint8_t nonce[12],
		       const uint8_t *ad, size_t adlen, uint8_t *buf, size_t len,
		       const uint8_t tag[16]);

/* Noise CipherState with nonce n (32 zero bits + 64-bit little endian n),
 * empty associated data, as the transport phase uses it.  buf has room for
 * len + 16; returns len + 16. */
size_t noise_encrypt(noise_cipher *c, uint8_t *buf, size_t len);
/* buf holds len bytes of ciphertext+tag; returns plaintext length or -1. */
int noise_decrypt(noise_cipher *c, uint8_t *buf, size_t len);

/* Handshake hash after the prologue: SHA256(SHA256(name) || prologue).
 * ESPHome prologue = "NoiseAPIInit" || be16(len(client hello)) || hello. */
void noise_prologue_hash(uint8_t h[32], const uint8_t *hello, size_t hello_len);

/* Run the responder side: read message 1 (-> psk, e), write message 2
 * (<- e, ee) into msg2[48] and split.  e_priv: 32 random bytes for our
 * ephemeral key (wiped).  On success fills send (responder->initiator)
 * and recv (initiator->responder). */
int noise_respond(const uint8_t h_prologue[32], const uint8_t psk[32],
		  const uint8_t *msg1, size_t msg1_len, uint8_t e_priv[32],
		  uint8_t msg2[NOISE_MSG2_LEN], noise_cipher *send, noise_cipher *recv);

/* Strict base64 (RFC 4648, padded) decode of exactly 32 bytes, as ESPHome's
 * "api: encryption: key:" uses.  Whitespace is ignored.  0 = ok. */
int noise_psk_from_base64(const char *b64, uint8_t psk[32]);

void noise_wipe(void *p, size_t n);

/* ---- CSPRNG (rng.c) for ephemeral keys ---- */
/* Seed from api_platform_entropy() plus extra secret/unique material. */
int noise_rng_init(const void *extra, size_t extra_len);
/* Cheap: fold an event (e.g. a cycle counter at a packet) into the pool. */
void noise_rng_event(void);
/* Mix public per-handshake data (uniqueness, not entropy). */
void noise_rng_mix(const void *d, size_t n);
/* Output; reseeds from api_platform_entropy() each call.  0 = ok. */
int noise_rng_bytes(void *out, size_t n);
#endif
