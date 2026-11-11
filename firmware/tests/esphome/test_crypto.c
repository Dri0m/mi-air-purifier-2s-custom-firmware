/* test_crypto.c - standard test vectors for the Noise crypto:
 *   SHA-256 (FIPS 180-2 / NIST examples), HMAC-SHA256 (RFC 4231 1, 2, 6),
 *   X25519 (RFC 7748 5.2, 6.1 and the 1 / 1000 iteration test),
 *   ChaCha20-Poly1305 AEAD (RFC 8439 2.8.2),
 *   Noise_NNpsk0_25519_ChaChaPoly_SHA256 responder against a vector made
 *   with the Python "noise" package (noiseprotocol, used by aioesphomeapi),
 *   base64 PSK decoding. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "noise.h"
#include "crypto/monocypher/monocypher.h"

static int fails;

static void hex(uint8_t *out, const char *h)
{
	for (size_t i = 0; h[2 * i]; i++) {
		unsigned v;
		sscanf(h + 2 * i, "%2x", &v);
		out[i] = (uint8_t)v;
	}
}

static void check(const char *what, const uint8_t *got, const char *want_hex)
{
	uint8_t want[512];
	size_t n = strlen(want_hex) / 2;
	hex(want, want_hex);
	if (memcmp(got, want, n)) {
		printf("  FAIL %s\n", what);
		fails++;
	} else {
		printf("  ok   %s\n", what);
	}
}

/* api_platform_entropy is needed by rng.c; deterministic here */
#include "esphome_api.h"
int api_platform_entropy(void *buf, size_t len)
{
	static uint8_t x;
	uint8_t *p = buf;
	for (size_t i = 0; i < len; i++)
		p[i] = x++;
	return 0;
}

int main(void)
{
	uint8_t out[512], k[200], buf[512];

	printf("test_crypto:\n");
	/* ---- SHA-256 ---- */
	noise_sha256(out, "abc", 3, NULL, 0);
	check("SHA-256 \"abc\"", out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	noise_sha256(out, "", 0, NULL, 0);
	check("SHA-256 \"\"", out, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	noise_sha256(out, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmn", 44,
		     "lmnomnopnopq", 12);   /* split across two update calls */
	check("SHA-256 448-bit message", out, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	{
		char *m = malloc(1000000);
		memset(m, 'a', 1000000);
		noise_sha256(out, m, 1000000, NULL, 0);
		free(m);
		check("SHA-256 one million 'a'", out, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
	}

	/* ---- HMAC-SHA256, RFC 4231 ---- */
	memset(k, 0x0b, 20);
	noise_hmac_sha256(out, k, 20, "Hi There", 8, NULL, 0);
	check("HMAC RFC 4231 #1", out, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
	noise_hmac_sha256(out, (const uint8_t *)"Jefe", 4, "what do ya want for nothing?", 28, NULL, 0);
	check("HMAC RFC 4231 #2", out, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
	memset(k, 0xaa, 131);
	noise_hmac_sha256(out, k, 131, "Test Using Larger Than Block-Size Key - Hash Key First", 54, NULL, 0);
	check("HMAC RFC 4231 #6 (key > block)", out, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");

	/* ---- X25519, RFC 7748 ---- */
	{
		uint8_t s[32], u[32], a[32], b[32], pa[32], pb[32];
		hex(s, "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4");
		hex(u, "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c");
		crypto_x25519(out, s, u);
		check("X25519 RFC 7748 5.2 #1", out, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
		hex(s, "4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d");
		hex(u, "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493");
		crypto_x25519(out, s, u);
		check("X25519 RFC 7748 5.2 #2", out, "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957");
		hex(a, "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
		hex(b, "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
		crypto_x25519_public_key(pa, a);
		crypto_x25519_public_key(pb, b);
		check("X25519 RFC 7748 6.1 Alice public", pa, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
		check("X25519 RFC 7748 6.1 Bob public", pb, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
		crypto_x25519(out, a, pb);
		check("X25519 RFC 7748 6.1 shared (A)", out, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
		crypto_x25519(out, b, pa);
		check("X25519 RFC 7748 6.1 shared (B)", out, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
		/* iterated: k = u = 9; k, u = X25519(k, u), k */
		uint8_t kk[32] = { 9 }, uu[32] = { 9 }, t[32];
		for (int i = 1; i <= 1000; i++) {
			crypto_x25519(t, kk, uu);
			memcpy(uu, kk, 32);
			memcpy(kk, t, 32);
			if (i == 1)
				check("X25519 RFC 7748 5.2 iterated x1", kk, "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079");
		}
		check("X25519 RFC 7748 5.2 iterated x1000", kk, "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51");
	}

	/* ---- ChaCha20-Poly1305, RFC 8439 2.8.2 ---- */
	{
		static const char pt[] = "Ladies and Gentlemen of the class of '99: If I could offer you "
					 "only one tip for the future, sunscreen would be it.";
		uint8_t key[32], nonce[12], aad[12], tag[16];
		size_t n = sizeof(pt) - 1;
		for (int i = 0; i < 32; i++)
			key[i] = (uint8_t)(0x80 + i);
		hex(nonce, "070000004041424344454647");
		hex(aad, "50515253c0c1c2c3c4c5c6c7");
		memcpy(buf, pt, n);
		noise_aead_encrypt(key, nonce, aad, 12, buf, n, tag);
		check("AEAD RFC 8439 2.8.2 ciphertext", buf,
		      "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116");
		check("AEAD RFC 8439 2.8.2 tag", tag, "1ae10b594f09e26a7e902ecbd0600691");
		assert(noise_aead_decrypt(key, nonce, aad, 12, buf, n, tag) == 0);
		assert(!memcmp(buf, pt, n));
		noise_aead_encrypt(key, nonce, aad, 12, buf, n, tag);
		buf[5] ^= 1;
		assert(noise_aead_decrypt(key, nonce, aad, 12, buf, n, tag) == -1);
		buf[5] ^= 1;
		aad[0] ^= 1;
		assert(noise_aead_decrypt(key, nonce, aad, 12, buf, n, tag) == -1);
		printf("  ok   AEAD decrypt round trip, tampered ciphertext/AD rejected\n");
	}

	/* ---- Noise NNpsk0 responder vs the Python noise package ---- */
	{
		uint8_t psk[32], h[32], msg1[48], e[32], msg2[48];
		noise_cipher send, recv;
		for (int i = 0; i < 32; i++)
			psk[i] = (uint8_t)i;
		memset(e, 0x22, 32);
		hex(msg1, "7b4e909bbe7ffe44c465a220037d608ee35897d31ef972f07f74892cb0f73f13"
			  "d1bc1a38b6793685dc7e2ffdf8ea0933");
		noise_prologue_hash(h, NULL, 0);       /* "NoiseAPIInit\0\0" */
		assert(noise_respond(h, psk, msg1, 48, e, msg2, &send, &recv) == NOISE_OK);
		check("Noise NNpsk0 message 2", msg2,
		      "0faa684ed28867b97f4a6a2dee5df8ce974e76b7018e3f22a1c4cf2678570f20"
		      "7336b95933b321b1ae86673f689f5c9f");
		memcpy(buf, "\x00\x08\x00\x00", 4);
		assert(noise_encrypt(&send, buf, 4) == 20);
		check("Noise transport responder->initiator #0", buf, "d9423bf48bdea62bb10bbd87a7fb5ecc26721050");
		assert(noise_encrypt(&send, buf, 0) == 16);
		check("Noise transport responder->initiator #1 (nonce 1)", buf, "455cefb80c49ccfb47b759043b665238");
		hex(buf, "082a88d4457b72646ca1ef304be34f8ffac1f659");
		assert(noise_decrypt(&recv, buf, 20) == 4 && !memcmp(buf, "\x00\x07\x00\x00", 4));
		printf("  ok   Noise transport initiator->responder decrypts\n");
		/* wrong PSK -> MAC failure */
		psk[0] ^= 1;
		memset(e, 0x22, 32);
		assert(noise_respond(h, psk, msg1, 48, e, msg2, &send, &recv) == NOISE_ERR_MAC);
		psk[0] ^= 1;
		memset(e, 0x22, 32);
		assert(noise_respond(h, psk, msg1, 47, e, msg2, &send, &recv) == NOISE_ERR);
		printf("  ok   Noise wrong PSK -> MAC failure, short message -> error\n");
	}

	/* ---- base64 PSK ---- */
	{
		uint8_t psk[32];
		assert(noise_psk_from_base64("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=", psk) == 0);
		for (int i = 0; i < 32; i++)
			assert(psk[i] == i);
		assert(noise_psk_from_base64(" AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=\n", psk) == 0);
		assert(noise_psk_from_base64("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8", psk) != 0);
		assert(noise_psk_from_base64("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHg==", psk) != 0);
		assert(noise_psk_from_base64("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh!=", psk) != 0);
		assert(noise_psk_from_base64("", psk) != 0);
		printf("  ok   base64 PSK decode (valid, whitespace, short, 31 bytes, bad char)\n");
	}

	/* ---- RNG: distinct outputs ---- */
	{
		uint8_t a[32], b[32];
		assert(noise_rng_bytes(a, 32) != 0);   /* unseeded: refuses */
		assert(noise_rng_init("x", 1) == 0);
		assert(noise_rng_bytes(a, 32) == 0 && noise_rng_bytes(b, 32) == 0);
		assert(memcmp(a, b, 32) != 0);
		printf("  ok   RNG refuses before seeding, outputs differ\n");
	}
	printf(fails ? "test_crypto: FAILED (%d)\n" : "test_crypto: OK\n", fails);
	return fails != 0;
}
