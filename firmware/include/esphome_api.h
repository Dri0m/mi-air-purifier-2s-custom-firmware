/* esphome_api.h - ESPHome native API server (port 6053; plaintext, or Noise
 * encryption when a key is set) and mDNS advertisement (_esphomelib._tcp).
 * Portable: uses netio.h, hal.h (hal_millis) and purifier.h, plus the
 * entropy hook below. */
#ifndef ESPHOME_API_H
#define ESPHOME_API_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
	const char *name;                  /* hostname, e.g. "air-purifier-2s" */
	const char *friendly_name;
	const char *password;              /* legacy API password, "" = none */
	const char *version;               /* firmware version string */
	const char *mac_str;               /* "AA:BB:CC:DD:EE:FF" */
	/* ESPHome "api: encryption: key:": base64 of 32 bytes (openssl rand
	 * -base64 32).  Set: Noise_NNpsk0_25519_ChaChaPoly_SHA256 only, plaintext
	 * clients are rejected.  "" or NULL: plaintext only.  An invalid key
	 * makes api_server_init() fail (it never falls back to plaintext). */
	const char *encryption_key;
} api_config;

int api_server_init(const api_config *cfg);  /* 0 ok, <0 error (bad key, no listen) */
void api_server_poll(void);                /* call often from one task */
int api_server_client_count(void);         /* authenticated + subscribed sessions */
void mdns_init(const api_config *cfg);
void mdns_poll(void);

/* Platform entropy, implemented per platform (device: src/esphome/
 * rng_mw300.c, host: src/host/rng_posix.c).  Fills buf with len bytes of raw
 * unpredictable material; the caller hashes it, so it need not be uniform.
 * Small requests (<= 8 bytes) must be cheap (called per received packet).
 * Returns 0, or <0 if no entropy is available.  Only used with encryption. */
int api_platform_entropy(void *buf, size_t len);
#endif
