/* ota.h - over-the-air update into the passive appfw slot.
 * Sequence: ota_begin(size) -> ota_write()* -> ota_end() which verifies the
 * MRVL image (header + segment CRCs read back from flash) and only then
 * makes the slot active (gen = other + 1).  Never touches the running slot,
 * boot2, partition layout, PSM or wififw. */
#ifndef OTA_H
#define OTA_H
#include <stddef.h>
#include <stdint.h>

int ota_begin(uint32_t image_size);
int ota_write(const void *data, size_t len);
int ota_end(void);                         /* 0 = verified + switched */
void ota_abort(void);

/* HTTP receiver: POST /update with header "X-OTA-Password", raw MRVL body;
 * POST /confirm (same password) confirms a trial image; GET /version. */
int ota_server_init(uint16_t port, const char *password);
void ota_server_poll(void);
int ota_server_ready(void);                /* initialised + listening */

/* ---- boot confirm, status, errors (was src/ota/ota_ext.h) ----
 * Boot confirm:
 *   app start:  ota_boot_check();              // early, before Wi-Fi
 *   loop:       ota_trial_poll(ip, ota_ready, api_clients);  // fallback
 *               ota_server_poll();             // POST /confirm + timer
 * A trial image is confirmed by
 *   1. an authenticated POST /confirm (tools/ota_upload.py sends it after
 *      checking /version), accepted once the trial has run
 *      OTA_CONFIRM_MIN_UP_MS, or
 *   2. the unattended fallback in ota_trial_poll(): trial running
 *      >= OTA_SELF_CONFIRM_MS with IP up, the OTA server listening and an
 *      ESPHome API client session (authenticated + subscribed) that has
 *      lasted >= OTA_SELF_CONFIRM_API_MS without a break.
 * An updated image that is not confirmed within OTA_CONFIRM_TIMEOUT_MS, or
 * that resets/hangs and is power-cycled first, boots the previous slot. */
/* Error codes (negative).  ota_strerror() gives a short token. */
#define OTA_OK            0
#define OTA_E_BUSY      (-1)   /* update already in progress */
#define OTA_E_STATE     (-2)   /* call out of sequence */
#define OTA_E_SIZE      (-3)   /* declared size out of range */
#define OTA_E_TABLE     (-4)   /* partition table unreadable/invalid */
#define OTA_E_LAYOUT    (-5)   /* appfw slots not as expected */
#define OTA_E_RUNNING   (-6)   /* cannot determine/avoid the running slot */
#define OTA_E_FLASH     (-7)   /* flash read/write/erase failed */
#define OTA_E_GUARD     (-8)   /* write outside the armed window (bug) */
#define OTA_E_LENGTH    (-9)   /* more/less data than declared */
#define OTA_E_MD5       (-10)  /* MD5 differs from the client's */
#define OTA_E_IMAGE     (-11)  /* not a valid MRVL image for this slot */
#define OTA_E_CRC       (-12)  /* segment CRC mismatch (read back) */
#define OTA_E_VERIFY    (-13)  /* flash content != data received */
#define OTA_E_COMMIT    (-14)  /* table write/verify failed (old slot kept) */
#define OTA_E_TRIAL     (-15)  /* running image not yet confirmed */
#define OTA_E_AUTH      (-16)  /* server: wrong password */
#define OTA_E_EARLY     (-17)  /* POST /confirm before OTA_CONFIRM_MIN_UP_MS */

const char *ota_strerror(int err);

/* Boot confirm.  ota_boot_check() returns 0 = normal boot, 1 = this is an
 * unconfirmed trial boot (call ota_mark_good() when healthy), <0 = error
 * (logged; firmware keeps running).  Idempotent.  ota_server_init() calls
 * it too, in case the app forgot. */
int ota_boot_check(void);
int ota_mark_good(void);
void ota_tick(void);                      /* confirm timeout; called by ota_server_poll() */

/* POST /confirm: OTA_OK = trial confirmed now, 1 = not a trial (nothing
 * done, harmless), OTA_E_EARLY = trial younger than OTA_CONFIRM_MIN_UP_MS
 * (retry later), other <0 = confirm failed (trial and revert stay armed). */
int ota_confirm_request(void);

/* Unattended fallback confirm; call every app-loop pass.  ip_up: DHCP
 * address held; ota_ready: ota_server_ready(); api_clients: ESPHome API
 * sessions that completed the handshake and subscribed to states, or <0 if
 * the API server cannot tell (then the fallback never fires and only
 * POST /confirm confirms).  Cheap when not in a trial. */
void ota_trial_poll(int ip_up, int ota_ready, int api_clients);

/* 1 = unconfirmed trial whose previous image is valid (the table already
 * boots it, or the next boot check re-arms it): any reset reverts.  Only
 * reads RAM flags; safe to call from another task (trial hang guard). */
int ota_trial_revert_armed(void);

/* ota_end() variant that also checks the client's MD5 (NULL = none). */
int ota_end_md5(const uint8_t *md5_or_null);

typedef struct {
    char running;           /* 'A', 'B' or '?' (table order of the appfw entries) */
    char next_boot;         /* slot boot2 will pick from the table now */
    uint32_t running_addr;
    uint32_t image_time;    /* MRVL header timestamp of the running image */
    uint32_t table_gen;
    uint32_t gen[2];        /* appfw gens A, B */
    uint8_t trial;          /* 1 = unconfirmed trial boot */
    uint8_t can_revert;
    uint8_t update_pending; /* an update was committed; reboot to apply */
    uint8_t in_progress;
    uint32_t confirm_left_ms; /* until the timeout revert (trial + can_revert) */
    uint32_t trial_up_ms;     /* trial: time since the boot check */
    uint32_t confirm_wait_ms; /* trial: until POST /confirm is accepted */
    uint8_t api_seen;         /* trial: an API client session was seen */
    uint8_t api_hook;         /* trial: API session count known (fallback possible) */
    uint32_t received, declared;
    int last_error;
} ota_info_t;

int ota_get_info(ota_info_t *info);

#ifndef OTA_CONFIRM_TIMEOUT_MS
#define OTA_CONFIRM_TIMEOUT_MS (10u * 60u * 1000u)
#endif
/* Earliest POST /confirm, measured from the boot check (~ power-on): long
 * enough for Wi-Fi, DHCP, the API server, the first HA connect and the
 * first sensor rounds, i.e. a crash or reset in bring-up shows first. */
#ifndef OTA_CONFIRM_MIN_UP_MS
#define OTA_CONFIRM_MIN_UP_MS (150u * 1000u)  /* past the 120 s Wi-Fi re-kick window */
#endif
/* Fallback self-confirm (no POST /confirm, e.g. ota_upload.py interrupted):
 * trial up >= 5 min, and an API session unbroken for >= 60 s (several
 * ESPHome keepalives and state pushes).  HA reconnects within ~1 min of the
 * reboot, leaving ~5 min of the 10 min window for slow reconnects. */
#ifndef OTA_SELF_CONFIRM_MS
#define OTA_SELF_CONFIRM_MS (5u * 60u * 1000u)
#endif
#ifndef OTA_SELF_CONFIRM_API_MS
#define OTA_SELF_CONFIRM_API_MS (60u * 1000u)
#endif
#ifndef OTA_CONFIRM_RETRY_MS
#define OTA_CONFIRM_RETRY_MS (10u * 1000u)   /* fallback: retry a failed write */
#endif
#ifndef OTA_BOOT_CONFIRM
#define OTA_BOOT_CONFIRM 1      /* 0 = commit images as confirmed directly */
#endif
#endif
