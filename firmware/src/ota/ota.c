/* ota.c - OTA core: stream an MRVL image into the passive appfw slot,
 * verify it from flash, then switch the partition table like stock rfget.
 *
 * Safety invariants
 *  1. Only two flash windows are ever writable: the target slot during an
 *     upload, and the ping-pong table sector during a table write.  Every
 *     write/erase goes through ota_guarded_*(), which also refuses the
 *     running slot and everything below 0xA000 except the two table sectors.
 *     boot2, PSM, wififw and the active slot can not be touched.
 *  2. The target slot is never the one boot2 would boot: the target is the
 *     slot that is not executing (XIP offset / boot flags), and if the table
 *     happens to point at it (update committed but not rebooted yet, or boot2
 *     fell back), ota_begin() first re-points the table at the running slot.
 *  3. The image's 4 magic bytes are held in RAM and programmed only after
 *     the rest verified from flash, so an interrupted upload never leaves
 *     something that looks like an image.
 *  4. The table switch happens only after: length == declared, MD5 of the
 *     received stream == client MD5 (if given), MD5 of the flash content ==
 *     MD5 of the stream, and the MRVL structure + every segment CRC verify
 *     from flash (twice: before and after the magic is programmed).
 *  5. Table writes (wmpt.c) program the table magic last: a power cut leaves
 *     either the old table selected or the complete new one.
 *
 * Boot confirm, done purely with the partition table (no extra flash area):
 *   commit:      target.gen = max+1, target.flag = 'T'         (table write 1)
 *   first boot:  ota_boot_check() sees its own entry flagged 'T':
 *                previous.gen = max+1, own flag = 'B'          (table write 2)
 *                -> from now on any reset/hang+power cycle boots the
 *                   previous image ("revert first").
 *   healthy:     ota_mark_good(): own.gen = max+1, flag = 0,
 *                own pad1 = 'C' (confirmed)                    (table write 3)
 *                called by POST /confirm (ota_confirm_request, trial up
 *                >= OTA_CONFIRM_MIN_UP_MS) or by the unattended fallback
 *                (ota_trial_poll: up >= OTA_SELF_CONFIRM_MS, IP + OTA
 *                listening + an API session for OTA_SELF_CONFIRM_API_MS).
 *   timeout:     no ota_mark_good() within OTA_CONFIRM_TIMEOUT_MS -> reboot
 *                (the table already points at the previous slot).
 * An image whose entry has flag 0 but no 'C' mark in pad1 was installed by
 * something else (stock miIO OTA / rfget, a UART flash) and has never been
 * confirmed: its first boot is treated exactly like a 'T' trial, so a first
 * custom image that cannot reach Wi-Fi or keeps crashing falls back to the
 * stock image instead of needing disassembly.
 * The flag lives in pad[0] of the appfw entry, the mark in pad[1]; boot2
 * reads only type, start, size and gen, and libpart copies the padding
 * through unchanged.  A hang
 * *before* ota_boot_check() runs is the one case this can not catch (boot2
 * only falls back when the header/segment CRCs are bad), so call it first
 * thing in main. */
#include <string.h>
#include "ota_internal.h"
#include "ota_flash.h"
#include "ota_crc.h"
#include "hal.h"

uint8_t ota_scratch[OTA_SCRATCH];

/* The fallback and the earliest POST /confirm must both fit in the window,
 * with room for HA to reconnect before the revert. */
typedef char ota_self_confirm_before_timeout[
    (OTA_SELF_CONFIRM_MS + OTA_SELF_CONFIRM_API_MS < OTA_CONFIRM_TIMEOUT_MS &&
     OTA_CONFIRM_MIN_UP_MS < OTA_CONFIRM_TIMEOUT_MS &&
     OTA_SELF_CONFIRM_API_MS <= OTA_SELF_CONFIRM_MS) ? 1 : -1];

/* ------------------------------------------------------------ guard */

static uint32_t g_lo, g_hi;             /* armed window [lo, hi) */
static uint32_t run_lo, run_hi;         /* running slot, never writable */

void ota_guard_arm(uint32_t lo, uint32_t hi) { g_lo = lo; g_hi = hi; }
void ota_guard_disarm(void) { g_lo = g_hi = 0; }

static int guard_ok(uint32_t addr, uint32_t len)
{
    if (len == 0 || addr < g_lo || addr >= g_hi || len > g_hi - addr)
        return 0;
    if (addr < run_hi && run_lo < addr + len)
        return 0;
    if (addr < OTA_PROTECT_END &&
        !(addr >= WMPT_ADDR0 && addr + len <= WMPT_ADDR1 + OTA_FLASH_SECTOR))
        return 0;
    return addr + len <= OTA_FLASH_SIZE;
}

int ota_guarded_write(uint32_t addr, const void *buf, uint32_t len)
{
    if (len == 0)
        return OTA_OK;
    if (!guard_ok(addr, len)) {
        OTA_LOG("GUARD: refused write %x+%x\r\n", (unsigned)addr, (unsigned)len);
        return OTA_E_GUARD;
    }
    return ota_flash_write(addr, buf, len) == 0 ? OTA_OK : OTA_E_FLASH;
}

int ota_guarded_erase(uint32_t addr)
{
    if (addr % OTA_FLASH_SECTOR || !guard_ok(addr, OTA_FLASH_SECTOR)) {
        OTA_LOG("GUARD: refused erase %x\r\n", (unsigned)addr);
        return OTA_E_GUARD;
    }
    return ota_flash_erase_sector(addr) == 0 ? OTA_OK : OTA_E_FLASH;
}

/* ------------------------------------------------------------ state */

enum { ST_IDLE, ST_RECV, ST_FAILED };

static struct {
    int st;
    int target;                 /* 0/1: appfw index being written */
    int run;                    /* 0/1: running appfw index */
    uint32_t slot, slot_size;
    uint32_t declared, received, erased_to;
    uint8_t magic[4];
    ota_md5_ctx md5;
    int last_error;
    uint8_t update_pending;
} o;

static struct {
    volatile uint8_t checked, trial, can_revert;   /* read by the hang guard */
    volatile uint8_t reverting;  /* timeout revert in progress: keep the guard armed */
    int run;
    uint32_t t0;
    /* fallback confirm (ota_trial_poll) */
    uint8_t api_seen, api_up, api_hook, api_nohook_logged, tried;
    uint32_t api_since, last_try;
} bc;

static wmpt_state_t ws;         /* ~480 bytes, static to keep stacks small */
static wmpt_table_t nt;

void ota_reset_state(void)
{
    memset(&o, 0, sizeof o);
    memset(&bc, 0, sizeof bc);
    ota_guard_disarm();
    run_lo = run_hi = 0;
}

const char *ota_strerror(int err)
{
    switch (err) {
    case OTA_OK: return "ok";
    case OTA_E_BUSY: return "busy";
    case OTA_E_STATE: return "bad_state";
    case OTA_E_SIZE: return "bad_size";
    case OTA_E_TABLE: return "partition_table_invalid";
    case OTA_E_LAYOUT: return "unexpected_layout";
    case OTA_E_RUNNING: return "running_slot_unknown";
    case OTA_E_FLASH: return "flash_error";
    case OTA_E_GUARD: return "write_outside_window";
    case OTA_E_LENGTH: return "length_mismatch";
    case OTA_E_MD5: return "md5_mismatch";
    case OTA_E_IMAGE: return "invalid_image";
    case OTA_E_CRC: return "segment_crc_mismatch";
    case OTA_E_VERIFY: return "flash_readback_mismatch";
    case OTA_E_COMMIT: return "table_write_failed";
    case OTA_E_TRIAL: return "unconfirmed_trial_boot";
    case OTA_E_AUTH: return "unauthorized";
    case OTA_E_EARLY: return "too_early";
    default: return "error";
    }
}

static uint32_t max_gen(const wmpt_state_t *s)
{
    uint32_t a = s->t.e[s->app[0]].gen, b = s->t.e[s->app[1]].gen;
    return a > b ? a : b;
}

/* Index (0/1) of the appfw slot that is executing, or -1. */
static int running_index(const wmpt_state_t *s)
{
    uint32_t off;
    if (ota_flash_running_offset(&off) == 0) {
        for (int j = 0; j < 2; j++)
            if (s->t.e[s->app[j]].start == off)
                return j;
    }
    int32_t f = ota_flash_boot_flags();
    if (f >= 0 && (f & 3) < 2)
        return (int)(f & 3);
    return -1;
}

static void protect_running(const wmpt_state_t *s, int run)
{
    const wmpt_entry_t *e = &s->t.e[s->app[run]];
    run_lo = e->start;
    run_hi = e->start + e->size;
}

enum { CONF_KEEP = -1, CONF_CLEAR = 0, CONF_SET = 1 };

/* Make appfw index `want` the one boot2 boots, set flags (and optionally
 * want's confirmation mark), write + recheck. */
static int table_switch(int want, uint8_t want_flag, uint8_t other_flag, int want_conf)
{
    int rc = wmpt_load(&ws);
    if (rc)
        return rc;
    nt = ws.t;
    wmpt_entry_t *ew = &nt.e[ws.app[want]], *eo = &nt.e[ws.app[1 - want]];
    if (ws.active != want)
        ew->gen = max_gen(&ws) + 1;     /* stock: gen = other + 1 */
    ew->flag = want_flag;
    eo->flag = other_flag;
    if (want_conf != CONF_KEEP)
        ew->pad1 = want_conf == CONF_SET ? WMPT_CONF_MARK : 0;
    uint8_t want_pad1 = ew->pad1;
    uint32_t expect_addr = ws.other_addr;
    rc = wmpt_write_next(&ws, &nt);
    if (rc)
        return rc;
    /* Post-condition, re-read from flash: boot2 now picks the new table and
     * from it the wanted slot, with the flags we wrote. */
    if (wmpt_load(&ws) != OTA_OK || ws.addr != expect_addr || ws.active != want ||
        ws.t.e[ws.app[want]].flag != want_flag || ws.t.e[ws.app[1 - want]].flag != other_flag ||
        ws.t.e[ws.app[want]].pad1 != want_pad1)
        return OTA_E_COMMIT;
    return OTA_OK;
}

static int fail(int rc)
{
    o.st = ST_FAILED;
    o.last_error = rc;
    ota_guard_disarm();
    OTA_LOG("failed: %s\r\n", ota_strerror(rc));
    return rc;
}

/* ------------------------------------------------------------ upload */

int ota_begin(uint32_t image_size)
{
    if (o.st == ST_RECV)
        return OTA_E_BUSY;
    o.st = ST_IDLE;
    if (ota_flash_open() != 0)
        return fail(OTA_E_FLASH);
    if (!bc.checked) {
        int brc = ota_boot_check();
        if (brc < 0)
            return fail(brc);           /* boot not classified: never upload */
    }
    if (bc.trial)
        return fail(OTA_E_TRIAL);       /* would overwrite the fallback image */

    int rc = wmpt_load(&ws);
    if (rc)
        return fail(rc);
    int run = running_index(&ws);
    if (run < 0) {
#ifdef OTA_REQUIRE_RUNNING_SLOT
        return fail(OTA_E_RUNNING);
#else
        run = ws.active;                /* what boot2 normally boots */
        OTA_LOG("running slot unknown, assuming table-active %d\r\n", run);
#endif
    }
    int target = 1 - run;
    const wmpt_entry_t *te = &ws.t.e[ws.app[target]];
    if (image_size < 256 || image_size > te->size)
        return fail(OTA_E_SIZE);
    protect_running(&ws, run);

    if (ws.active == target) {
        /* Table points at the slot we are about to erase (earlier update not
         * yet rebooted into, boot2 fell back, or GPIO43 strap).  Re-point it
         * at the running image first so an interruption cannot leave boot2
         * aiming at a half-written slot. */
        OTA_LOG("re-activating running slot before overwrite\r\n");
        rc = table_switch(run, WMPT_FLAG_NONE, WMPT_FLAG_NONE, CONF_KEEP);
        if (rc)
            return fail(rc);
        te = &ws.t.e[ws.app[target]];
    }
    o.update_pending = 0;
    o.target = target;
    o.run = run;
    o.slot = te->start;
    o.slot_size = te->size;
    o.declared = image_size;
    o.received = 0;
    o.erased_to = o.slot;
    memset(o.magic, 0xFF, sizeof o.magic);
    ota_md5_init(&o.md5);
    ota_guard_arm(o.slot, o.slot + o.slot_size);
    o.st = ST_RECV;
    o.last_error = 0;
    OTA_LOG("begin: %u bytes -> slot %c @%x\r\n", (unsigned)image_size,
            'A' + target, (unsigned)o.slot);
    return OTA_OK;
}

int ota_write(const void *data, size_t len)
{
    if (o.st != ST_RECV)
        return OTA_E_STATE;
    if (len > o.declared - o.received)
        return fail(OTA_E_LENGTH);
    if (len == 0)
        return OTA_OK;
    ota_md5_update(&o.md5, data, len);

    const uint8_t *p = (const uint8_t *)data;
    uint32_t pos = o.received, end = o.received + (uint32_t)len;
    while (o.erased_to < o.slot + end) {
        int rc = ota_guarded_erase(o.erased_to);
        if (rc)
            return fail(rc);
        o.erased_to += OTA_FLASH_SECTOR;
    }
    if (pos < 4) {                      /* hold the image magic back */
        uint32_t n = 4 - pos;
        if (n > len)
            n = (uint32_t)len;
        memcpy(o.magic + pos, p, n);
        p += n;
        pos += n;
    }
    if (end > pos) {
        int rc = ota_guarded_write(o.slot + pos, p, end - pos);
        if (rc)
            return fail(rc);
    }
    o.received = end;
    return OTA_OK;
}

void ota_abort(void)
{
    if (o.st != ST_IDLE)
        OTA_LOG("abort after %u/%u bytes\r\n", (unsigned)o.received, (unsigned)o.declared);
    o.st = ST_IDLE;
    ota_guard_disarm();
    /* Nothing to undo: no magic was written and the table is unchanged. */
}

int ota_end(void)
{
    return ota_end_md5(NULL);
}

int ota_end_md5(const uint8_t *md5_or_null)
{
    if (o.st != ST_RECV)
        return OTA_E_STATE;
    if (o.received != o.declared)
        return fail(OTA_E_LENGTH);

    uint8_t got[16], back[16];
    ota_md5_final(&o.md5, got);
    if (md5_or_null && memcmp(got, md5_or_null, 16) != 0)
        return fail(OTA_E_MD5);

    /* 1. Flash content == what we received (MD5 over a full read-back). */
    ota_md5_ctx c;
    ota_md5_init(&c);
    for (uint32_t off = 0; off < o.declared; off += OTA_SCRATCH) {
        uint32_t n = o.declared - off;
        if (n > OTA_SCRATCH)
            n = OTA_SCRATCH;
        if (ota_flash_read(o.slot + off, ota_scratch, n) != 0)
            return fail(OTA_E_FLASH);
        if (off == 0) {
            static const uint8_t ff[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
            if (memcmp(ota_scratch, ff, 4) != 0)
                return fail(OTA_E_VERIFY);
            memcpy(ota_scratch, o.magic, 4);
        }
        ota_md5_update(&c, ota_scratch, n);
    }
    ota_md5_final(&c, back);
    if (memcmp(got, back, 16) != 0)
        return fail(OTA_E_VERIFY);

    /* 2. MRVL structure + segment CRCs from flash (magic from RAM). */
    mrvl_info_t info;
    int rc = mrvl_verify_flash(o.slot, o.declared, o.slot_size, o.magic, &info);
    if (rc)
        return fail(rc);

    /* 3. Program the magic, verify everything again purely from flash. */
    rc = ota_guarded_write(o.slot, o.magic, 4);
    if (rc == OTA_OK)
        rc = mrvl_verify_flash(o.slot, o.declared, o.slot_size, NULL, &info);
    if (rc) {
        (void)ota_guarded_erase(o.slot);    /* leave no half-valid image */
        return fail(rc);
    }
    ota_guard_disarm();

    /* 4. Switch: target.gen = max + 1, ping-pong table copy. */
    rc = table_switch(o.target, OTA_BOOT_CONFIRM ? WMPT_FLAG_TRIAL : WMPT_FLAG_NONE,
                      WMPT_FLAG_NONE, OTA_BOOT_CONFIRM ? CONF_CLEAR : CONF_SET);
    if (rc)
        return fail(rc == OTA_E_COMMIT ? rc : OTA_E_COMMIT);
    o.st = ST_IDLE;
    o.update_pending = 1;
    o.last_error = 0;
    OTA_LOG("switched to slot %c (gen %u, table gen %u), entry %x\r\n", 'A' + o.target,
            (unsigned)ws.t.e[ws.app[o.target]].gen, (unsigned)ws.t.gen, (unsigned)info.entry);
    return OTA_OK;
}

/* ------------------------------------------------------------ boot confirm */

static int slot_image_ok(const wmpt_state_t *s, int idx)
{
    const wmpt_entry_t *e = &s->t.e[s->app[idx]];
    return mrvl_verify_flash(e->start, e->size, e->size, NULL, NULL) == OTA_OK;
}

int ota_boot_check(void)
{
    if (bc.checked)
        return bc.trial;
    /* Only a successful classification is cached (Sol review #4): on any
     * error bc.checked stays 0, the next call retries, and ota_begin()
     * refuses uploads until the boot has been classified. */
    bc.trial = 0;
    bc.can_revert = 0;
    bc.run = -1;
    OTA_LOG("bc: flash open\r\n");
    if (ota_flash_open() != 0)
        return OTA_E_FLASH;
    OTA_LOG("bc: loading tables\r\n");
    int rc = wmpt_load(&ws);
    if (rc)
        return rc;
    int run = running_index(&ws);
    OTA_LOG("bc: tables ok, running slot %d\r\n", run);
    if (run < 0)
        return OTA_E_RUNNING;
    bc.checked = 1;
    bc.run = run;
    const wmpt_entry_t *re = &ws.t.e[ws.app[run]];
    uint8_t flag = re->flag;
    /* Flag 0 without the 'C' mark: installed by stock OTA / UART, never
     * confirmed by this firmware.  Same handling as a fresh 'T' commit. */
    int never_confirmed = OTA_BOOT_CONFIRM && flag == WMPT_FLAG_NONE &&
                          re->pad1 != WMPT_CONF_MARK;
    if (flag != WMPT_FLAG_TRIAL && flag != WMPT_FLAG_BOOTED && !never_confirmed)
        return 0;

    /* Unconfirmed image. */
    protect_running(&ws, run);
    bc.trial = 1;
    bc.t0 = hal_millis();
    OTA_LOG("bc: unconfirmed image, verifying previous slot\r\n");
    bc.can_revert = (uint8_t)slot_image_ok(&ws, 1 - run);
    OTA_LOG("trial boot of slot %c (%s), previous image %s\r\n", 'A' + run,
            flag == WMPT_FLAG_TRIAL ? "new update" : flag == WMPT_FLAG_BOOTED ?
            "update, booted before" : "never confirmed",
            bc.can_revert ? "valid" : "INVALID - no revert");
    if (bc.can_revert && (flag == WMPT_FLAG_TRIAL || ws.active == run)) {
        /* Revert first: previous slot active again until ota_mark_good(). */
        OTA_LOG("bc: writing revert-first table\r\n");
        rc = table_switch(1 - run, WMPT_FLAG_NONE, WMPT_FLAG_BOOTED, CONF_KEEP);
        OTA_LOG("bc: revert-first table write rc=%d\r\n", rc);
        if (rc)
            OTA_LOG("revert-first table write failed: %s\r\n", ota_strerror(rc));
    } else if (!bc.can_revert && flag == WMPT_FLAG_TRIAL) {
        rc = table_switch(run, WMPT_FLAG_BOOTED, WMPT_FLAG_NONE, CONF_KEEP);
        if (rc)
            OTA_LOG("flag write failed: %s\r\n", ota_strerror(rc));
    }
    return 1;
}

int ota_mark_good(void)
{
    if (!bc.checked)
        (void)ota_boot_check();
    if (!bc.trial)
        return OTA_OK;
    int rc = wmpt_load(&ws);
    if (rc)
        return rc;
    if (!slot_image_ok(&ws, bc.run))
        return OTA_E_IMAGE;             /* never promote a bad image */
    rc = table_switch(bc.run, WMPT_FLAG_NONE, WMPT_FLAG_NONE, CONF_SET);
    if (rc) {
        OTA_LOG("confirm failed: %s\r\n", ota_strerror(rc));
        return rc;
    }
    bc.trial = 0;
    OTA_LOG("slot %c confirmed\r\n", 'A' + bc.run);
    return OTA_OK;
}

int ota_confirm_request(void)
{
    if (!bc.checked) {
        int rc = ota_boot_check();
        if (rc < 0)
            return rc;                  /* boot not classified: do nothing */
    }
    if (!bc.trial)
        return 1;                       /* confirmed image: harmless no-op */
    if ((uint32_t)(hal_millis() - bc.t0) < OTA_CONFIRM_MIN_UP_MS)
        return OTA_E_EARLY;
    OTA_LOG("confirm requested over HTTP\r\n");
    return ota_mark_good();
}

void ota_trial_poll(int ip_up, int ota_ready, int api_clients)
{
    if (!bc.checked || !bc.trial)
        return;
    uint32_t now = hal_millis();
    if (api_clients < 0) {
        bc.api_hook = 0;
        if (!bc.api_nohook_logged) {
            bc.api_nohook_logged = 1;
            OTA_LOG("trial: API session count unavailable, fallback confirm off "
                    "(POST /confirm only)\r\n");
        }
        return;
    }
    bc.api_hook = 1;
    if (api_clients == 0) {
        bc.api_up = 0;                  /* a break restarts the API timer */
        return;
    }
    if (!bc.api_up) {
        bc.api_up = 1;
        bc.api_since = now;
        if (!bc.api_seen)
            OTA_LOG("trial: API client session up\r\n");
        bc.api_seen = 1;
    }
    if ((uint32_t)(now - bc.t0) < OTA_SELF_CONFIRM_MS ||
        (uint32_t)(now - bc.api_since) < OTA_SELF_CONFIRM_API_MS ||
        !ip_up || !ota_ready || o.st == ST_RECV)
        return;
    if (bc.tried && (uint32_t)(now - bc.last_try) < OTA_CONFIRM_RETRY_MS)
        return;
    bc.tried = 1;
    bc.last_try = now;
    OTA_LOG("trial healthy for %u s (API session %u s), self-confirming\r\n",
            (unsigned)((now - bc.t0) / 1000u), (unsigned)((now - bc.api_since) / 1000u));
    int rc = ota_mark_good();
    if (rc)
        OTA_LOG("self-confirm failed: %s, retrying\r\n", ota_strerror(rc));
}

int ota_trial_revert_armed(void)
{
    /* also while the timeout revert reboots: hal_reboot_hook() flushes the
     * EEPROM over I2C, and a stuck bus there must still reset */
    return bc.reverting || (bc.checked && bc.trial && bc.can_revert);
}

void ota_tick(void)
{
    if (!bc.trial || !bc.can_revert || o.st == ST_RECV)
        return;
    if ((uint32_t)(hal_millis() - bc.t0) < OTA_CONFIRM_TIMEOUT_MS)
        return;
    OTA_LOG("not confirmed within %u ms, reverting\r\n", (unsigned)OTA_CONFIRM_TIMEOUT_MS);
    int rc = wmpt_load(&ws);
    if (rc == OTA_OK && ws.active == bc.run)
        rc = table_switch(1 - bc.run, WMPT_FLAG_NONE, WMPT_FLAG_BOOTED, CONF_KEEP);
    if (rc == OTA_OK && ws.active == 1 - bc.run) {
        bc.reverting = 1;
        bc.trial = 0;
        hal_reboot();
        return;
    }
    OTA_LOG("revert impossible (%s), staying\r\n", ota_strerror(rc));
    bc.can_revert = 0;
}

/* ------------------------------------------------------------ info */

int ota_get_info(ota_info_t *info)
{
    memset(info, 0, sizeof *info);
    info->running = info->next_boot = '?';
    info->last_error = o.last_error;
    info->in_progress = o.st == ST_RECV;
    info->received = o.received;
    info->declared = o.declared;
    info->update_pending = o.update_pending;
    info->trial = bc.trial;
    info->can_revert = bc.can_revert;
    if (bc.trial) {
        uint32_t el = hal_millis() - bc.t0;
        if (bc.can_revert)
            info->confirm_left_ms = el < OTA_CONFIRM_TIMEOUT_MS ? OTA_CONFIRM_TIMEOUT_MS - el : 0;
        info->trial_up_ms = el;
        info->confirm_wait_ms = el < OTA_CONFIRM_MIN_UP_MS ? OTA_CONFIRM_MIN_UP_MS - el : 0;
        info->api_seen = bc.api_seen;
        info->api_hook = bc.api_hook;
    }
    if (o.st == ST_RECV)
        return OTA_OK;                  /* ws is in use; keep it stable */
    if (ota_flash_open() != 0)
        return OTA_E_FLASH;
    int rc = wmpt_load(&ws);
    if (rc)
        return rc;
    info->table_gen = ws.t.gen;
    info->gen[0] = ws.t.e[ws.app[0]].gen;
    info->gen[1] = ws.t.e[ws.app[1]].gen;
    info->next_boot = (char)('A' + ws.active);
    int run = running_index(&ws);
    if (run >= 0) {
        uint8_t h[12];
        info->running = (char)('A' + run);
        info->running_addr = ws.t.e[ws.app[run]].start;
        if (ota_flash_read(info->running_addr, h, sizeof h) == 0)
            info->image_time = (uint32_t)h[8] | (uint32_t)h[9] << 8 |
                               (uint32_t)h[10] << 16 | (uint32_t)h[11] << 24;
    }
    return OTA_OK;
}
