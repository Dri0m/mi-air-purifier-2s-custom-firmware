/* stubs.c - weak placeholders so the image links before the other modules
 * exist.  A module's real (strong) definition replaces each one at link
 * time; nothing else needs to change.  Do not add logic here. */
#include <stdint.h>
#include "esphome_api.h"
#include "ota.h"
#include "hal.h"
#include "purifier.h"

#define WEAK __attribute__((weak))

WEAK int api_server_init(const api_config *cfg) { (void)cfg; return 0; }
WEAK void api_server_poll(void) { }
WEAK void mdns_init(const api_config *cfg) { (void)cfg; }
WEAK void mdns_poll(void) { }
WEAK int ota_server_init(uint16_t port, const char *password) { (void)port; (void)password; return 0; }
WEAK void ota_server_poll(void) { }
WEAK void hal_init(void) { }
WEAK void purifier_init(void) { }
WEAK void purifier_tick(void) { }

WEAK int ota_boot_check(void) { return 0; }
WEAK int ota_mark_good(void) { return 0; }
WEAK void ota_tick(void) { }
WEAK void purifier_flush(void) { }
WEAK int ota_server_ready(void) { return 0; }
