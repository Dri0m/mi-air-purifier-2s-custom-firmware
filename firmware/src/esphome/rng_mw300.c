/* rng_mw300.c - api_platform_entropy() on the 88MW300 (DEVICE ONLY, not
 * portable; listed in module.mk as ESPHOME_DEVICE_SRCS).
 *
 * The MW300 has no TRNG and the WMSDK exposes none: get_random_sequence()
 * is newlib rand() seeded once from sample_initialise_random_seed() (flash
 * ID + four 12-bit ADC0 samples of the DAC noise generator), so it is not a
 * CSPRNG, and its seed routine reconfigures ADC0, which src/hal_mw300 owns
 * for the light sensor (16-bit, IN4).  We therefore don't call either.
 *
 * Raw material returned here (hashed by src/esphome/rng.c, never used
 * directly):
 *   - DWT->CYCCNT (200 MHz core cycle counter) and SysTick->VAL: sampled on
 *     every call; rng.c calls us on every received TCP segment / mDNS
 *     packet and at each handshake, so the low bits capture the jitter of
 *     asynchronous Wi-Fi/network arrivals relative to the CPU clock;
 *   - the FreeRTOS tick count;
 *   - the radio's last data-frame noise floor (wlan_get_current_nf(), a
 *     driver variable updated by the Wi-Fi firmware per received frame) and
 *     the current RSSI;
 *   - the MAC (unique, not secret) in the large requests;
 *   - hal_adc_noise(): 8 fresh 16-bit light-ADC conversions folded together
 *     (analog noise in the low bits), in the large requests.
 * rng.c also mixes the PSK (secret) and the peer's handshake data. */
#include <string.h>
#include <lowlevel_drivers.h>
#include "FreeRTOS.h"
#include "task.h"
#include "esphome_api.h"
#include "netio.h"

int wlan_get_current_nf(void);         /* libwlcmgr.a (wlan.h) */

static void cyc_enable(void)
{
	/* idempotent; src/hal_mw300 hal_init() enables it too */
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/* Optional HAL hook (src/hal_mw300/hal_io.c): fresh light-ADC samples,
 * whose low bits are analog noise.  Weak so builds without the HAL link. */
extern uint32_t hal_adc_noise(void) __attribute__((weak));

int api_platform_entropy(void *buf, size_t len)
{
	uint8_t *out = buf;
	uint32_t s[8] = { 0 };   /* never hash stale stack bytes (MAC fills 6 of 8 in s[5..6]) */
	size_t i, n;

	cyc_enable();
	if (len <= 8) {                 /* per-packet event: cheap */
		s[0] = DWT->CYCCNT ^ (SysTick->VAL << 16);
		s[1] = DWT->CYCCNT;
		memcpy(out, s, len);
		return 0;
	}
	for (i = 0; i < len; i += n) {
		uint8_t mac[6];
		s[0] = DWT->CYCCNT;
		s[1] = SysTick->VAL;
		s[2] = (uint32_t)xTaskGetTickCount();
		s[3] = (uint32_t)wlan_get_current_nf();
		s[4] = (uint32_t)netio_rssi() ^ (hal_adc_noise ? hal_adc_noise() << 8 : 0);
		netio_mac(mac);
		memcpy(&s[5], mac, 6);
		s[7] = DWT->CYCCNT;     /* the calls above take variable time */
		n = len - i < sizeof(s) ? len - i : sizeof(s);
		memcpy(out + i, s, n);
	}
	return 0;
}
