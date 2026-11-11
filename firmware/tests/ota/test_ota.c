/* test_ota.c - host tests for src/ota (core, table, boot confirm, HTTP).
 *
 * usage: test_ota IMAGE_DIR OUT_DIR
 *   IMAGE_DIR holds appfwA.raw / appfwB.raw (stock slots, used only as MRVL
 *   payloads); OUT_DIR receives flash files for tools/wmpt.py validation.
 * The simulated flash is built here from the documented layout; the real
 * flash dump is never read. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ota_internal.h"
#include "ota_crc.h"
#include "netio.h"
#include "sim.h"

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define SLOT_A 0x00A000u
#define SLOT_B 0x08A000u

static uint8_t *imgA, *imgB;
static size_t lenA, lenB;
static uint8_t base[OTA_FLASH_SIZE], snap[OTA_FLASH_SIZE];
static const char *out_dir;

/* ------------------------------------------------------------ helpers */

static uint8_t *load_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f);
    *len = (size_t)n;
    return b;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Reference CRC (bitwise, independent of ota_crc.c). */
static uint32_t crc_ref(const uint8_t *p, size_t n)
{
    uint32_t c = 0;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c;
}

/* MRVL image length = end of the last segment. */
static size_t mrvl_len(const uint8_t *s)
{
    size_t end = 0;
    for (uint32_t i = 0; i < rd32(s + 12); i++) {
        const uint8_t *h = s + 20 + 20 * i;
        size_t e = rd32(h + 4) + rd32(h + 8);
        if (e > end) end = e;
    }
    return end;
}

/* Build a WMPT table independently of wmpt.c (layout per the evidence doc). */
static size_t ref_table(uint8_t *out, uint32_t gen, uint32_t gen_a, uint32_t gen_b)
{
    static const struct { uint8_t type; const char *name; uint32_t start, size; } L[8] = {
        { 0, "boot2", 0x000000, 0x06000 }, { 4, "psm", 0x006000, 0x04000 },
        { 1, "appfw", 0x00A000, 0x80000 }, { 1, "appfw", 0x08A000, 0x80000 },
        { 2, "wififw", 0x10A000, 0x30000 }, { 2, "wififw", 0x13A000, 0x30000 },
        { 5, "mcufw", 0x16A000, 0x60000 }, { 6, "userdata", 0x1CA000, 0x36000 },
    };
    memset(out, 0, 16 + 24 * 8 + 4);
    memcpy(out, "WMPT", 4);
    out[4] = 1; out[6] = 8;
    wr32(out + 8, gen);
    wr32(out + 12, crc_ref(out, 12));
    for (int i = 0; i < 8; i++) {
        uint8_t *e = out + 16 + 24 * i;
        e[0] = L[i].type;
        memcpy(e + 2, L[i].name, strlen(L[i].name));
        wr32(e + 12, L[i].start);
        wr32(e + 16, L[i].size);
        wr32(e + 20, i == 2 ? gen_a : i == 3 ? gen_b : 1);
    }
    wr32(out + 16 + 192, crc_ref(out + 16, 192));
    return 16 + 192 + 4;
}

/* Set/clear the 'C' (confirmed by our firmware) mark in pad1 of both appfw
 * entries of the table at addr, fixing the trailer CRC. */
static void set_conf_marks(uint8_t *flash, uint32_t addr, uint8_t mark)
{
    uint8_t *t = flash + addr;
    t[16 + 24 * 2 + 11] = mark;
    t[16 + 24 * 3 + 11] = mark;
    wr32(t + 16 + 192, crc_ref(t + 16, 192));
}

/* confirmed = 0: tables exactly as stock wrote them (pad1 = 0, i.e. images
 * installed by something else and never confirmed by our firmware).
 * confirmed = 1: both images already confirmed (the usual test world). */
static void build_base_c(int confirmed)
{
    memset(base, 0xFF, sizeof base);
    memset(base, 0xB2, 0x4000);                 /* boot2 stand-in */
    memset(base + 0x6000, 0x5A, 0x4000);        /* PSM stand-in */
    memset(base + 0x10A000, 0x3C, 0x60000);     /* wififw stand-ins */
    memset(base + 0x16A000 + 0x1000, 0x77, 0x100); /* something in mcufw */
    ref_table(base + 0x4000, 2, 3, 2);          /* as on the unit: gen 2, A=3, B=2 */
    ref_table(base + 0x5000, 1, 2, 2);
    memcpy(base + SLOT_A, imgA, lenA);          /* stock images as payloads */
    memcpy(base + SLOT_B, imgB, lenB);
    if (confirmed) {
        set_conf_marks(base, 0x4000, WMPT_CONF_MARK);
        set_conf_marks(base, 0x5000, WMPT_CONF_MARK);
    }
}

static void build_base(void) { build_base_c(1); }

static void reset_world(void)
{
    memcpy(sim_flash, base, sizeof base);
    sim_faults_clear();
    sim_ops = 0;
    sim_nor_violations = 0;
    sim_running = SLOT_A;
    sim_boot_flags = -1;
    ota_reset_state();
    fake_ms = 1000;
    fake_reboots = 0;
}

static int region_eq(const uint8_t *a, uint32_t lo, uint32_t hi)
{
    return memcmp(sim_flash + lo, a + lo, hi - lo) == 0;
}

/* boot2, its partition table and PSM, wififw/mcufw/userdata: never written. */
static void check_untouched(const uint8_t *ref, const char *what)
{
    CHECK(region_eq(ref, 0x0000, 0x4000), "%s: boot2 changed", what);
    CHECK(region_eq(ref, 0x6000, 0xA000), "%s: PSM changed", what);
    CHECK(region_eq(ref, 0x10A000, OTA_FLASH_SIZE), "%s: wififw/mcufw/userdata changed", what);
}

static int boot(uint32_t *slot)
{
    int fb = 0;
    int r = sim_boot2(slot, &fb);
    if (r != 0)
        return -1;
    return fb ? 1 : 0;      /* 1 = needed boot2's (unproven) fallback */
}

/* Power-on: what boot2 boots becomes the running slot; RAM state is lost. */
static uint32_t reboot_device(void)
{
    uint32_t slot = 0;
    int r = boot(&slot);
    CHECK(r == 0, "boot2 model: %s", r < 0 ? "HANG" : "needed fallback");
    sim_running = slot;
    ota_reset_state();
    return slot;
}

static void md5_of(const uint8_t *p, size_t n, uint8_t out[16])
{
    ota_md5_ctx c;
    ota_md5_init(&c);
    ota_md5_update(&c, p, n);
    ota_md5_final(&c, out);
}

static unsigned rng = 12345;
static unsigned rnd(void) { rng = rng * 1103515245u + 12345u; return (rng >> 8) & 0xFFFFFF; }

/* chunk = 0: random chunk sizes 1..6000. */
static int upload(const uint8_t *img, size_t len, const uint8_t *md5, size_t chunk, size_t send_len)
{
    int rc = ota_begin((uint32_t)len);
    if (rc)
        return rc;
    size_t pos = 0;
    while (pos < send_len) {
        size_t n = chunk ? chunk : 1 + rnd() % 6000;
        if (n > send_len - pos) n = send_len - pos;
        rc = ota_write(img + pos, n);
        if (rc) {
            ota_abort();
            return rc;
        }
        pos += n;
    }
    rc = ota_end_md5(md5);
    if (rc)
        ota_abort();
    return rc;
}

static void save(const char *name)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", out_dir, name);
    CHECK(sim_save(p) == 0, "save %s", p);
}

static int table_valid(uint32_t addr)
{
    wmpt_table_t t;
    return wmpt_parse(sim_flash + addr, WMPT_MAX_BYTES, &t) == OTA_OK;
}

static uint8_t entry_flag(int idx)
{
    wmpt_state_t s;
    if (wmpt_load(&s) != OTA_OK) return 0xEE;
    return s.t.e[s.app[idx]].flag;
}

static uint8_t entry_mark(int idx)
{
    wmpt_state_t s;
    if (wmpt_load(&s) != OTA_OK) return 0xEE;
    return s.t.e[s.app[idx]].pad1;
}

/* ------------------------------------------------------------ tests */

static void test_checksums(void)
{
    CHECK(ota_crc32(0, "123456789", 9) == 0x2DFD2D88u, "crc KAT");
    CHECK(ota_crc32(ota_crc32(0, "1234", 4), "56789", 5) == 0x2DFD2D88u, "crc chaining");
    /* Stock segment CRCs must verify with our CRC (proves the algorithm). */
    for (int s = 0; s < 2; s++) {
        const uint8_t *im = s ? imgB : imgA;
        for (uint32_t i = 0; i < rd32(im + 12); i++) {
            const uint8_t *h = im + 20 + 20 * i;
            CHECK(ota_crc32(0, im + rd32(h + 4), rd32(h + 8)) == rd32(h + 16),
                  "stock image %c seg%u crc", 'A' + s, (unsigned)i);
        }
    }
    static const struct { const char *in; const char *hex; } kat[] = {
        { "", "d41d8cd98f00b204e9800998ecf8427e" },
        { "abc", "900150983cd24fb0d6963f7d28e17f72" },
        { "message digest", "f96b697d7cb7938d525a2f31aaf161d0" },
        { "12345678901234567890123456789012345678901234567890123456789012345678901234567890",
          "57edf4a22be3c955ac49da2e2107b67a" },
    };
    for (size_t k = 0; k < sizeof kat / sizeof kat[0]; k++) {
        uint8_t d[16];
        char hx[33];
        md5_of((const uint8_t *)kat[k].in, strlen(kat[k].in), d);
        for (int i = 0; i < 16; i++) sprintf(hx + 2 * i, "%02x", d[i]);
        CHECK(strcmp(hx, kat[k].hex) == 0, "md5 KAT %zu: %s", k, hx);
    }
    /* Streaming with odd chunking == one shot (1 MB of 'a'). */
    static uint8_t mb[1000000];
    memset(mb, 'a', sizeof mb);
    ota_md5_ctx c;
    uint8_t d1[16], d2[16];
    ota_md5_init(&c);
    for (size_t p = 0, n; p < sizeof mb; p += n) {
        n = 1 + rnd() % 777;
        if (n > sizeof mb - p) n = sizeof mb - p;
        ota_md5_update(&c, mb + p, n);
    }
    ota_md5_final(&c, d1);
    md5_of(mb, sizeof mb, d2);
    static const uint8_t want[16] = { 0x77, 0x07, 0xd6, 0xae, 0x4e, 0x02, 0x7c, 0x70,
                                      0xee, 0xa2, 0xa9, 0x35, 0xc2, 0x29, 0x6f, 0x21 };
    CHECK(memcmp(d1, want, 16) == 0 && memcmp(d2, want, 16) == 0, "md5 1M 'a'");
}

static void test_table(void)
{
    uint8_t ref[512], ours[512];
    size_t n = ref_table(ref, 2, 3, 2);
    wmpt_table_t t;
    CHECK(n == 0xD4, "table size");
    CHECK(crc_ref(ref, n) == 0, "residue 0 over header+entries+trailer");
    CHECK(wmpt_parse(ref, n, &t) == OTA_OK, "parse reference table");
    CHECK(t.n == 8 && t.gen == 2 && t.e[2].start == SLOT_A && t.e[3].gen == 2, "parsed fields");
    CHECK(wmpt_serialize(&t, ours, sizeof ours) == n && memcmp(ours, ref, n) == 0,
          "serialize reproduces the reference bytes");
    for (size_t i = 0; i < n; i++) {        /* every byte is CRC/magic covered */
        memcpy(ours, ref, n);
        ours[i] ^= 0x10;
        CHECK(wmpt_parse(ours, n, &t) != OTA_OK, "flip at %zu accepted", i);
    }
    CHECK(wmpt_parse(ref, n - 1, &t) != OTA_OK, "short table accepted");

    reset_world();
    wmpt_state_t s;
    CHECK(wmpt_load(&s) == OTA_OK && s.addr == 0x4000 && s.active == 0, "base: table 0x4000, A active");
    /* boot2 tie rule: equal gens -> primary. */
    ref_table(sim_flash + 0x5000, 2, 2, 5);
    CHECK(wmpt_load(&s) == OTA_OK && s.addr == 0x4000, "tie picks 0x4000");
    ref_table(sim_flash + 0x5000, 3, 2, 5);
    CHECK(wmpt_load(&s) == OTA_OK && s.addr == 0x5000 && s.active == 1, "higher backup wins, B");
    /* Blank (erased) selected table -> other one, like boot2. */
    memset(sim_flash + 0x5000, 0xFF, 0x1000);
    CHECK(wmpt_load(&s) == OTA_OK && s.addr == 0x4000, "erased backup ignored");
    /* Selected table with a bad CRC: refuse to derive anything. */
    sim_flash[0x4000 + 16 + 24 * 3 + 20] ^= 1;
    CHECK(wmpt_load(&s) == OTA_E_TABLE, "corrupt selected table refused");
    CHECK(ota_begin(lenB) != OTA_OK, "ota_begin refuses on corrupt table");
    /* Unexpected layout. */
    reset_world();
    ref_table(sim_flash + 0x4000, 9, 3, 2);
    wr32(sim_flash + 0x4000 + 16 + 24 * 3 + 12, 0x8B000);
    wr32(sim_flash + 0x4000 + 16 + 192, crc_ref(sim_flash + 0x4000 + 16, 192));
    CHECK(wmpt_load(&s) == OTA_E_LAYOUT, "moved slot B refused");
}

static void test_good_update(void)
{
    reset_world();
    uint8_t md5[16];
    md5_of(imgB, lenB, md5);
    memcpy(snap, sim_flash, sizeof snap);
    /* Overwrite slot B (passive) with image A's content, so a switch is visible. */
    int rc = upload(imgA, lenA, md5 /* wrong on purpose? no: */ , 0, lenA);
    CHECK(rc == OTA_E_MD5, "md5 of B given for A -> %s", ota_strerror(rc));
    md5_of(imgA, lenA, md5);
    rc = upload(imgA, lenA, md5, 0, lenA);
    CHECK(rc == OTA_OK, "good update: %s", ota_strerror(rc));
    check_untouched(snap, "good update");
    CHECK(region_eq(snap, SLOT_A, SLOT_A + 0x80000), "running slot A changed");
    CHECK(memcmp(sim_flash + SLOT_B, imgA, lenA) == 0, "slot B holds the new image");
    CHECK(region_eq(snap, 0x4000, 0x5000), "active table 0x4000 rewritten (must ping-pong)");
    CHECK(table_valid(0x5000) && rd32(sim_flash + 0x5000 + 8) == 3, "new table @0x5000 gen 3");
    CHECK(rd32(sim_flash + 0x5000 + 16 + 24 * 3 + 20) == 4, "B gen = A gen + 1 = 4");
    CHECK(sim_nor_violations == 0, "programmed non-erased flash");
    uint32_t slot;
    CHECK(boot(&slot) == 0 && slot == SLOT_B, "boot2 boots B after switch");
    CHECK(entry_mark(1) == 0 && entry_flag(1) == WMPT_FLAG_TRIAL, "new image: trial, not confirmed");
    save("good_after_commit.bin");

    /* Boot confirm: first boot of B re-activates A until confirmed. */
    CHECK(reboot_device() == SLOT_B, "rebooted into B");
    CHECK(ota_boot_check() == 1, "trial boot detected");
    CHECK(boot(&slot) == 0 && slot == SLOT_A, "revert-first: table points to A during trial");
    CHECK(entry_flag(1) == WMPT_FLAG_BOOTED, "B flagged booted");
    CHECK(ota_begin(lenA) == OTA_E_TRIAL, "OTA refused while unconfirmed");
    save("good_trial.bin");
    CHECK(ota_mark_good() == OTA_OK, "mark good");
    CHECK(boot(&slot) == 0 && slot == SLOT_B, "confirmed: B boots");
    CHECK(entry_flag(1) == WMPT_FLAG_NONE && entry_flag(0) == WMPT_FLAG_NONE, "flags cleared");
    CHECK(entry_mark(1) == WMPT_CONF_MARK && entry_mark(0) == WMPT_CONF_MARK, "both confirmed");
    CHECK(ota_mark_good() == OTA_OK, "mark good is idempotent");
    check_untouched(snap, "after confirm");
    save("good_confirmed.bin");

    ota_info_t in;
    CHECK(ota_get_info(&in) == OTA_OK && in.running == 'B' && in.next_boot == 'B' && !in.trial,
          "info after confirm");
}

static void test_confirm_timeout(void)
{
    reset_world();
    uint32_t slot;
    CHECK(upload(imgA, lenA, NULL, 4096, lenA) == OTA_OK, "update");
    CHECK(reboot_device() == SLOT_B, "into B");
    CHECK(ota_boot_check() == 1, "trial");
    fake_ms += OTA_CONFIRM_TIMEOUT_MS - 10;
    ota_tick();
    CHECK(fake_reboots == 0, "no reboot before the timeout");
    fake_ms += 20;
    ota_tick();
    CHECK(fake_reboots == 1, "reboot at timeout");
    CHECK(boot(&slot) == 0 && slot == SLOT_A, "reverted to A");
    CHECK(reboot_device() == SLOT_A, "running A");
    CHECK(ota_boot_check() == 0, "A boots normally");
    ota_info_t in;
    CHECK(ota_get_info(&in) == OTA_OK && in.running == 'A' && !in.trial, "info A");
    /* A fresh update from A works again (overwrites the failed B). */
    CHECK(upload(imgB, lenB, NULL, 0, lenB) == OTA_OK, "update after revert");
    CHECK(boot(&slot) == 0 && slot == SLOT_B, "B again");
    save("timeout_then_update.bin");

    /* Crash/power cycle during the trial -> previous image. */
    reset_world();
    CHECK(upload(imgA, lenA, NULL, 0, lenA) == OTA_OK, "update 2");
    CHECK(reboot_device() == SLOT_B, "into B");
    CHECK(ota_boot_check() == 1, "trial");
    CHECK(reboot_device() == SLOT_A, "reset during trial boots A");
    CHECK(ota_boot_check() == 0, "A normal");

    /* First-boot revert write never happened (e.g. hang before it):
     * a second boot of the unconfirmed image does it again. */
    reset_world();
    CHECK(upload(imgA, lenA, NULL, 0, lenA) == OTA_OK, "update 3");
    CHECK(reboot_device() == SLOT_B, "into B");
    CHECK(reboot_device() == SLOT_B, "B again (boot_check never ran)");
    CHECK(ota_boot_check() == 1, "trial");
    CHECK(boot(&slot) == 0 && slot == SLOT_A, "revert armed");

    /* Previous image invalid: no revert possible, trial confirmable. */
    reset_world();
    CHECK(upload(imgA, lenA, NULL, 0, lenA) == OTA_OK, "update 4");
    sim_flash[SLOT_A + 0x4000] ^= 0xFF;     /* A's seg1 corrupt */
    memcpy(base + SLOT_A, sim_flash + SLOT_A, 0x80000);
    CHECK(reboot_device() == SLOT_B, "into B");
    CHECK(ota_boot_check() == 1, "trial");
    CHECK(boot(&slot) == 0 && slot == SLOT_B, "no revert to a broken image");
    fake_ms += OTA_CONFIRM_TIMEOUT_MS + 1;
    ota_tick();
    CHECK(fake_reboots == 0, "no pointless reboot");
    CHECK(ota_mark_good() == OTA_OK && entry_flag(1) == WMPT_FLAG_NONE, "confirm");
    build_base();
}

/* First boot of an image installed by stock miIO OTA or a UART flash: the
 * table has flag 0 and no 'C' mark.  It must boot as a trial (previous slot
 * re-armed first), so a broken first image falls back instead of needing
 * disassembly. */
static void test_first_install(void)
{
    uint32_t slot;
    build_base_c(0);                /* A = our new image, B = stock, A active */

    /* Healthy: trial, revert armed, then confirmed. */
    reset_world();
    memcpy(snap, sim_flash, sizeof snap);
    CHECK(reboot_device() == SLOT_A, "foreign install boots A");
    CHECK(ota_boot_check() == 1, "never-confirmed image is a trial");
    CHECK(boot(&slot) == 0 && slot == SLOT_B, "revert-first: stock B armed");
    CHECK(entry_flag(0) == WMPT_FLAG_BOOTED, "A flagged booted");
    CHECK(region_eq(snap, SLOT_B, SLOT_B + 0x80000), "stock slot untouched");
    CHECK(ota_begin(lenB) == OTA_E_TRIAL, "OTA refused until confirmed");
    CHECK(ota_mark_good() == OTA_OK, "confirm");
    CHECK(boot(&slot) == 0 && slot == SLOT_A, "confirmed A boots");
    CHECK(entry_mark(0) == WMPT_CONF_MARK && entry_flag(0) == WMPT_FLAG_NONE, "A marked");
    CHECK(entry_mark(1) == 0, "stock entry unmarked");
    CHECK(reboot_device() == SLOT_A && ota_boot_check() == 0, "then boots normally");
    check_untouched(snap, "first install");
    save("first_install_confirmed.bin");

    /* No Wi-Fi / never healthy: timeout reverts to stock. */
    reset_world();
    CHECK(reboot_device() == SLOT_A && ota_boot_check() == 1, "trial 2");
    fake_ms += OTA_CONFIRM_TIMEOUT_MS + 1;
    ota_tick();
    CHECK(fake_reboots == 1, "revert reboot");
    CHECK(boot(&slot) == 0 && slot == SLOT_B, "back on stock");
    save("first_install_reverted.bin");

    /* Crash loop after the boot check: every later boot is stock. */
    reset_world();
    CHECK(reboot_device() == SLOT_A && ota_boot_check() == 1, "trial 3");
    CHECK(reboot_device() == SLOT_B, "crash -> stock");
    CHECK(reboot_device() == SLOT_B, "and stays there");

    /* Stock slot unusable: still a trial, but nothing to revert to. */
    reset_world();
    sim_flash[SLOT_B + 0x4000] ^= 0xFF;
    CHECK(reboot_device() == SLOT_A && ota_boot_check() == 1, "trial 4");
    CHECK(boot(&slot) == 0 && slot == SLOT_A, "no revert to a broken image");
    CHECK(ota_mark_good() == OTA_OK && entry_mark(0) == WMPT_CONF_MARK, "confirmable");

    build_base();
}

static void corrupt_and_expect(const char *name, void (*mut)(uint8_t *img, size_t *len), int want)
{
    static uint8_t img[0x80000];
    size_t len = lenB;
    memcpy(img, imgB, lenB);
    mut(img, &len);
    reset_world();
    memcpy(snap, sim_flash, sizeof snap);
    int rc = upload(img, len, NULL, 0, len);
    CHECK(rc == want, "%s: got %s want %s", name, ota_strerror(rc), ota_strerror(want));
    uint32_t slot;
    CHECK(boot(&slot) == 0 && slot == SLOT_A, "%s: still boots A", name);
    CHECK(region_eq(snap, 0x4000, 0x6000), "%s: tables touched", name);
    CHECK(region_eq(snap, SLOT_A, SLOT_A + 0x80000), "%s: slot A touched", name);
    check_untouched(snap, name);
    if (rc != OTA_E_SIZE)   /* begin refused: B was never erased */
        CHECK(rd32(sim_flash + SLOT_B) != 0x4C56524Du, "%s: B has MRVL magic", name);
    else
        CHECK(region_eq(snap, SLOT_B, SLOT_B + 0x80000), "%s: B touched", name);
    CHECK(sim_nor_violations == 0, "%s: nor", name);
}

static void m_seg_byte(uint8_t *i, size_t *l) { (void)l; i[0x20000] ^= 0x40; }
static void m_last_byte(uint8_t *i, size_t *l) { i[*l - 1] ^= 1; }
static void m_magic(uint8_t *i, size_t *l) { (void)l; i[0] = 'X'; }
static void m_sig(uint8_t *i, size_t *l) { (void)l; i[4] ^= 1; }
static void m_nseg0(uint8_t *i, size_t *l) { (void)l; wr32(i + 12, 0); }
static void m_nseg10(uint8_t *i, size_t *l) { (void)l; wr32(i + 12, 10); }
static void m_type(uint8_t *i, size_t *l) { (void)l; wr32(i + 20, 1); }
static void m_off_hdr(uint8_t *i, size_t *l) { (void)l; wr32(i + 20 + 4, 0x10); }
static void m_seg_past(uint8_t *i, size_t *l) { wr32(i + 40 + 8, (uint32_t)*l); }
static void m_xip_off(uint8_t *i, size_t *l) { (void)l; wr32(i + 40 + 12, 0x1F0033E4); }
static void m_ram_boot2(uint8_t *i, size_t *l) { (void)l; wr32(i + 20 + 12, 0x0015B000); }
static void m_ram_bad(uint8_t *i, size_t *l) { (void)l; wr32(i + 60 + 12, 0x30000000); }
/* SRAM1 segment ending above the stock-proven 0x20010C6C (boot2's stack is
 * at the SRAM1 top), but still inside SRAM1.  Load address just below the
 * limit, so it holds for any test image whose SRAM1 segment is > 0x6C long
 * (stock: 0xC6C, our build: ~0x2EC). */
static void m_ram_stack(uint8_t *i, size_t *l) { (void)l; wr32(i + 60 + 12, 0x20010C00); }
static void m_entry_arm(uint8_t *i, size_t *l) { (void)l; wr32(i + 16, 0x1F0033E0); }
static void m_entry_out(uint8_t *i, size_t *l) { (void)l; wr32(i + 16, 0x00100001); }
static void m_overlap(uint8_t *i, size_t *l) { (void)l; wr32(i + 60 + 4, 0x33E0 + 0x100); }
static void m_crc_field(uint8_t *i, size_t *l) { (void)l; i[20 + 16] ^= 1; }
static void m_too_big(uint8_t *i, size_t *l) { (void)i; *l = 0x80001; }
static void m_tiny(uint8_t *i, size_t *l) { (void)i; *l = 100; }
static void m_garbage(uint8_t *i, size_t *l) { for (size_t k = 0; k < *l; k++) i[k] = (uint8_t)rnd(); }

static void test_bad_images(void)
{
    corrupt_and_expect("flipped code byte", m_seg_byte, OTA_E_CRC);
    corrupt_and_expect("flipped last byte", m_last_byte, OTA_E_CRC);
    corrupt_and_expect("bad magic", m_magic, OTA_E_IMAGE);
    corrupt_and_expect("bad sig", m_sig, OTA_E_IMAGE);
    corrupt_and_expect("0 segments", m_nseg0, OTA_E_IMAGE);
    corrupt_and_expect("10 segments", m_nseg10, OTA_E_IMAGE);
    corrupt_and_expect("segment type 1", m_type, OTA_E_IMAGE);
    corrupt_and_expect("segment inside header", m_off_hdr, OTA_E_IMAGE);
    corrupt_and_expect("segment past end", m_seg_past, OTA_E_IMAGE);
    corrupt_and_expect("XIP offset mismatch", m_xip_off, OTA_E_IMAGE);
    corrupt_and_expect("RAM seg over boot2", m_ram_boot2, OTA_E_IMAGE);
    corrupt_and_expect("RAM seg outside SRAM", m_ram_bad, OTA_E_IMAGE);
    corrupt_and_expect("SRAM1 seg into boot2 stack", m_ram_stack, OTA_E_IMAGE);
    corrupt_and_expect("entry without thumb bit", m_entry_arm, OTA_E_IMAGE);
    corrupt_and_expect("entry outside XIP", m_entry_out, OTA_E_IMAGE);
    corrupt_and_expect("overlapping segments", m_overlap, OTA_E_IMAGE);
    corrupt_and_expect("wrong CRC field", m_crc_field, OTA_E_CRC);
    corrupt_and_expect("bigger than slot", m_too_big, OTA_E_SIZE);
    corrupt_and_expect("tiny", m_tiny, OTA_E_SIZE);
    corrupt_and_expect("random garbage", m_garbage, OTA_E_IMAGE);

    uint8_t md5[16];
    uint32_t slot;
    /* Wrong MD5. */
    reset_world();
    md5_of(imgB, lenB, md5);
    md5[3] ^= 1;
    CHECK(upload(imgB, lenB, md5, 0, lenB) == OTA_E_MD5, "wrong md5");
    CHECK(boot(&slot) == 0 && slot == SLOT_A, "wrong md5 -> A");
    /* Truncated: fewer bytes than declared. */
    reset_world();
    CHECK(upload(imgB, lenB, NULL, 0, lenB - 1000) == OTA_E_LENGTH, "truncated");
    CHECK(boot(&slot) == 0 && slot == SLOT_A, "truncated -> A");
    /* More bytes than declared. */
    reset_world();
    CHECK(ota_begin((uint32_t)lenB - 10) == OTA_OK, "begin");
    CHECK(ota_write(imgB, lenB) == OTA_E_LENGTH, "overflow rejected");
    CHECK(ota_end() == OTA_E_STATE, "end after failure");
    ota_abort();
    CHECK(boot(&slot) == 0 && slot == SLOT_A, "overflow -> A");
    /* Sequence errors. */
    CHECK(ota_write(imgB, 10) == OTA_E_STATE, "write without begin");
    CHECK(ota_end() == OTA_E_STATE, "end without begin");
    CHECK(ota_begin((uint32_t)lenB) == OTA_OK && ota_begin((uint32_t)lenB) == OTA_E_BUSY, "busy");
    ota_abort();
    /* Flash bit flip while programming -> read-back mismatch. */
    reset_world();
    sim_flip_addr = SLOT_B + 0x30001;
    CHECK(upload(imgB, lenB, NULL, 0, lenB) == OTA_E_VERIFY, "bit flip in write caught");
    CHECK(boot(&slot) == 0 && slot == SLOT_A, "flip -> A");
    /* Bit flip in the padding between header and first segment (not CRC'd). */
    reset_world();
    sim_flip_addr = SLOT_B + 0x50;
    CHECK(upload(imgB, lenB, NULL, 0, lenB) == OTA_E_VERIFY, "flip in padding caught");
    /* Flash op reports an error. */
    reset_world();
    sim_fail_op = 37;
    CHECK(upload(imgB, lenB, NULL, 0, lenB) == OTA_E_FLASH, "flash error");
    CHECK(boot(&slot) == 0 && slot == SLOT_A, "flash error -> A");
    /* After all failures, a good upload still works. */
    sim_fail_op = -1;
    CHECK(upload(imgB, lenB, NULL, 0, lenB) == OTA_OK, "recovers");
}

static void test_guard(void)
{
    reset_world();
    memcpy(snap, sim_flash, sizeof snap);
    uint8_t z[16] = { 0 };
    CHECK(ota_guarded_write(0x100, z, 16) == OTA_E_GUARD, "unarmed write");
    CHECK(ota_guarded_erase(SLOT_B) == OTA_E_GUARD, "unarmed erase");
    CHECK(ota_begin((uint32_t)lenB) == OTA_OK, "begin");
    CHECK(ota_guarded_write(0x0, z, 16) == OTA_E_GUARD, "boot2 write");
    CHECK(ota_guarded_write(0x6000, z, 16) == OTA_E_GUARD, "psm write");
    CHECK(ota_guarded_erase(SLOT_A) == OTA_E_GUARD, "running slot erase");
    CHECK(ota_guarded_write(SLOT_B + 0x80000 - 8, z, 16) == OTA_E_GUARD, "past slot end");
    CHECK(ota_guarded_erase(0x10A000) == OTA_E_GUARD, "wififw erase");
    CHECK(ota_guarded_erase(0x4000) == OTA_E_GUARD, "table erase during upload");
    ota_abort();
    CHECK(memcmp(sim_flash, snap, sizeof snap) == 0, "guard tests changed flash");

    /* Running slot = B per hardware while the table says A (boot2 fell back
     * or GPIO43): target must be A, and the table is re-pointed at B first. */
    reset_world();
    sim_running = SLOT_B;
    memcpy(snap, sim_flash, sizeof snap);
    CHECK(upload(imgB, lenB, NULL, 0, lenB) == OTA_OK, "update while running B");
    CHECK(region_eq(snap, SLOT_B, SLOT_B + 0x80000), "running B untouched");
    uint32_t slot;
    CHECK(boot(&slot) == 0 && slot == SLOT_A, "new image in A boots");
    /* Running slot from boot flags only. */
    reset_world();
    sim_running = -1;
    sim_boot_flags = 0x01;
    memcpy(snap, sim_flash, sizeof snap);
    CHECK(upload(imgA, lenA, NULL, 0, lenA) == OTA_OK, "boot flags say B");
    CHECK(region_eq(snap, SLOT_B, SLOT_B + 0x80000), "B untouched (boot flags)");
}

static void test_pingpong(void)
{
    reset_world();
    memcpy(snap, sim_flash, sizeof snap);
    uint32_t prev_gen = rd32(sim_flash + 0x4000 + 8), running = SLOT_A;
    for (int i = 0; i < 6; i++) {
        uint32_t target = running == SLOT_A ? SLOT_B : SLOT_A, slot;
        const uint8_t *img = (i & 1) ? imgB : imgA;
        size_t len = (i & 1) ? lenB : lenA;
        uint8_t md5[16];
        md5_of(img, len, md5);
        CHECK(upload(img, len, md5, 0, len) == OTA_OK, "cycle %d upload", i);
        CHECK(boot(&slot) == 0 && slot == target, "cycle %d switch", i);
        CHECK(reboot_device() == target, "cycle %d reboot", i);
        CHECK(ota_boot_check() == 1, "cycle %d trial", i);
        CHECK(ota_mark_good() == OTA_OK, "cycle %d confirm", i);
        CHECK(reboot_device() == target, "cycle %d confirmed reboot", i);
        CHECK(ota_boot_check() == 0, "cycle %d normal", i);
        CHECK(table_valid(0x4000) && table_valid(0x5000), "cycle %d both tables valid", i);
        wmpt_state_t s;
        CHECK(wmpt_load(&s) == OTA_OK && s.t.gen == prev_gen + 3, "cycle %d table gen +3", i);
        prev_gen = s.t.gen;
        CHECK(sim_nor_violations == 0, "cycle %d nor", i);
        check_untouched(snap, "pingpong");
        char name[64];
        snprintf(name, sizeof name, "pingpong_%d.bin", i);
        save(name);
        running = target;
    }

    /* Two updates without a reboot in between: second one re-points the
     * table at the running slot before erasing the pending one. */
    reset_world();
    uint32_t slot;
    CHECK(upload(imgA, lenA, NULL, 0, lenA) == OTA_OK, "pending update");
    CHECK(upload(imgB, lenB, NULL, 0, lenB) == OTA_OK, "second update before reboot");
    CHECK(boot(&slot) == 0 && slot == SLOT_B, "second update active");
    CHECK(memcmp(sim_flash + SLOT_B, imgB, lenB) == 0, "B holds 2nd image");
    save("double_update.bin");
}

/* Power cut at every flash operation of a whole update lifecycle. */
static int lifecycle(long budget, int double_update)
{
    reset_world();
    sim_cut_mode = (int)(budget & 1);
    if (double_update)
        (void)upload(imgA, lenA, NULL, 4096, lenA);     /* committed, not rebooted */
    sim_ops = 0;
    sim_budget = budget;
    int rc = upload(imgB, lenB, NULL, 4096, lenB);
    if (!sim_dead && rc == OTA_OK) {
        reboot_device();            /* budget keeps counting across the reboot */
        (void)ota_boot_check();
        (void)ota_mark_good();
    }
    int completed = !sim_dead;
    sim_faults_clear();

    uint32_t slot;
    int r = boot(&slot);
    CHECK(r == 0, "cut@%ld%s: boot2 %s", budget, double_update ? " (double)" : "",
          r < 0 ? "HANGS" : "needs its fallback");
    if (r != 0)
        return completed;
    CHECK(slot == SLOT_A || slot == SLOT_B, "cut@%ld: odd slot", budget);
    CHECK(region_eq(base, 0, 0x4000) && region_eq(base, 0x6000, 0xA000) &&
          region_eq(base, 0x10A000, OTA_FLASH_SIZE), "cut@%ld: protected region", budget);
    CHECK(region_eq(base, SLOT_A, SLOT_A + 0x80000), "cut@%ld: slot A changed", budget);
    CHECK(sim_nor_violations == 0, "cut@%ld: nor", budget);
    if (completed)
        CHECK(slot == SLOT_B, "complete lifecycle ends in B");

    /* Recover from wherever we are, then a fresh update must succeed. */
    sim_running = slot;
    ota_reset_state();
    (void)ota_boot_check();
    CHECK(ota_mark_good() == OTA_OK, "cut@%ld: confirm after recovery", budget);
    uint32_t s2;
    CHECK(boot(&s2) == 0 && s2 == slot, "cut@%ld: stable after recovery", budget);
    const uint8_t *img = slot == SLOT_A ? imgB : imgA;
    size_t len = slot == SLOT_A ? lenB : lenA;
    CHECK(upload(img, len, NULL, 0, len) == OTA_OK, "cut@%ld: fresh update", budget);
    CHECK(boot(&s2) == 0 && s2 != slot, "cut@%ld: fresh update switches", budget);
    return completed;
}

static void test_power_cuts(void)
{
    for (int dbl = 0; dbl < 2; dbl++) {
        long k;
        int f0 = failures;
        for (k = 0; k < 100000; k++) {
            if (lifecycle(k, dbl))
                break;
            if (failures - f0 > 10)
                break;
        }
        printf("  power cut at each of %ld flash ops%s: %s\n", k,
               dbl ? " (2nd update before reboot)" : "", failures == f0 ? "ok" : "FAIL");
    }
}

/* ------------------------------------------------------------ trial confirm policy (R1) */

/* api(t) for the simulated app loop: sessions as seen at t ms after boot. */
typedef int (*api_fn)(uint32_t t);
static int api_none(uint32_t t) { (void)t; return 0; }
static int api_nohook(uint32_t t) { (void)t; return -1; }
static int api_from_20s(uint32_t t) { return t >= 20000u; }
static int api_from_270s(uint32_t t) { return t >= 270000u; }
/* HA connects, drops after 50 s, reconnects 1 s later, forever. */
static int api_flap(uint32_t t) { return t >= 20000u && (t - 20000u) % 51000u < 50000u; }

static int ip_always(uint32_t t) { (void)t; return 1; }
static int ip_from_7min(uint32_t t) { return t >= 7u * 60000u; }

/* Run the app loop (1 s steps) from now until t_end ms after the trial's
 * boot, or until a confirm or reboot.  Returns the trial-relative time of
 * the event, or 0 if none.  *what = 'C' confirmed, 'R' reboot, 0 none. */
static uint32_t run_trial(uint32_t boot_ms, uint32_t t_end, api_fn api, int (*ip)(uint32_t),
                          int ready, char *what)
{
    *what = 0;
    int r0 = fake_reboots;
    while (fake_ms - boot_ms <= t_end) {
        uint32_t t = fake_ms - boot_ms;
        ota_trial_poll(ip(t), ready, api(t));
        ota_tick();
        if (fake_reboots != r0) { *what = 'R'; return t; }
        if (ota_boot_check() == 0) { *what = 'C'; return t; }
        fake_ms += 1000;
    }
    return 0;
}

/* Upload A into B, reboot into the trial.  Returns the boot time. */
static uint32_t into_trial(void)
{
    reset_world();
    CHECK(upload(imgA, lenA, NULL, 0, lenA) == OTA_OK, "update");
    CHECK(reboot_device() == SLOT_B, "into B");
    uint32_t b = fake_ms;
    CHECK(ota_boot_check() == 1 && ota_trial_revert_armed() == 1, "trial, revert armed");
    return b;
}

static void test_trial_policy(void)
{
    uint32_t slot, t, b;
    char what;

    /* POST /confirm: too early, then accepted; trial table untouched until then. */
    b = into_trial();
    memcpy(snap, sim_flash, sizeof snap);
    fake_ms = b + OTA_CONFIRM_MIN_UP_MS - 1;
    CHECK(ota_confirm_request() == OTA_E_EARLY, "confirm before min uptime refused");
    CHECK(memcmp(sim_flash, snap, sizeof snap) == 0, "early confirm wrote flash");
    ota_info_t in;
    CHECK(ota_get_info(&in) == OTA_OK && in.trial && in.confirm_wait_ms == 1 &&
          in.trial_up_ms == OTA_CONFIRM_MIN_UP_MS - 1, "info: wait %u up %u",
          (unsigned)in.confirm_wait_ms, (unsigned)in.trial_up_ms);
    CHECK(boot(&slot) == 0 && slot == SLOT_A, "still reverts while unconfirmed");
    fake_ms += 1;
    CHECK(ota_confirm_request() == OTA_OK, "confirm at min uptime");
    CHECK(boot(&slot) == 0 && slot == SLOT_B && entry_mark(1) == WMPT_CONF_MARK, "B confirmed");
    CHECK(ota_trial_revert_armed() == 0, "guard disarmed after confirm");
    memcpy(snap, sim_flash, sizeof snap);
    CHECK(ota_confirm_request() == 1, "second confirm: no-op");
    CHECK(memcmp(sim_flash, snap, sizeof snap) == 0, "no-op confirm wrote flash");
    CHECK(reboot_device() == SLOT_B && ota_boot_check() == 0, "B boots normally");
    CHECK(ota_confirm_request() == 1 && memcmp(sim_flash, snap, sizeof snap) == 0,
          "confirm on a confirmed image after reboot: no-op");
    fake_ms += OTA_SELF_CONFIRM_MS * 2;
    ota_trial_poll(1, 1, 1);
    ota_tick();
    CHECK(fake_reboots == 0 && memcmp(sim_flash, snap, sizeof snap) == 0, "poll on confirmed: no-op");
    CHECK(ota_trial_revert_armed() == 0, "not armed on a normal boot");

    /* Fallback: healthy with HA from 20 s -> confirms at OTA_SELF_CONFIRM_MS, not before. */
    b = into_trial();
    t = run_trial(b, OTA_CONFIRM_TIMEOUT_MS + 1000, api_from_20s, ip_always, 1, &what);
    CHECK(what == 'C' && t == OTA_SELF_CONFIRM_MS, "fallback confirm at %u ms (%c)", (unsigned)t, what);
    CHECK(boot(&slot) == 0 && slot == SLOT_B, "fallback: B confirmed");
    fake_ms += OTA_CONFIRM_TIMEOUT_MS;
    ota_tick();
    CHECK(fake_reboots == 0, "no revert after a fallback confirm");

    /* HA session only from 4:30 -> needs 60 s unbroken -> 5:30. */
    b = into_trial();
    t = run_trial(b, OTA_CONFIRM_TIMEOUT_MS + 1000, api_from_270s, ip_always, 1, &what);
    CHECK(what == 'C' && t == 270000u + OTA_SELF_CONFIRM_API_MS, "late HA: confirm at %u ms", (unsigned)t);

    /* Never healthy -> revert at the timeout, never a confirm. */
    struct { api_fn api; int (*ip)(uint32_t); int ready; const char *what; } bad[] = {
        { api_none, ip_always, 1, "no HA session" },
        { api_nohook, ip_always, 1, "API hook missing" },
        { api_flap, ip_always, 1, "HA session keeps dropping" },
        { api_from_20s, ip_always, 0, "OTA server not listening" },
    };
    for (size_t k = 0; k < sizeof bad / sizeof bad[0]; k++) {
        b = into_trial();
        t = run_trial(b, OTA_CONFIRM_TIMEOUT_MS + 1000, bad[k].api, bad[k].ip, bad[k].ready, &what);
        CHECK(what == 'R' && t == OTA_CONFIRM_TIMEOUT_MS, "%s: revert at timeout (%c %u)",
              bad[k].what, what ? what : '-', (unsigned)t);
        CHECK(boot(&slot) == 0 && slot == SLOT_A, "%s: previous slot boots", bad[k].what);
        CHECK(entry_mark(1) != WMPT_CONF_MARK, "%s: B never confirmed", bad[k].what);
        CHECK(reboot_device() == SLOT_A && ota_boot_check() == 0, "%s: A normal", bad[k].what);
    }

    /* IP only from 7 min: confirm as soon as it is up (API from 20 s). */
    b = into_trial();
    t = run_trial(b, OTA_CONFIRM_TIMEOUT_MS + 1000, api_from_20s, ip_from_7min, 1, &what);
    CHECK(what == 'C' && t == 7u * 60000u, "ip late: confirm at %u ms", (unsigned)t);

    /* Reboot loop / crash before confirm: previous image, nothing confirmed;
     * a /confirm or poll on the reverted image is a no-op. */
    b = into_trial();
    fake_ms += 45000;
    CHECK(reboot_device() == SLOT_A, "crash in trial -> A");
    CHECK(ota_boot_check() == 0, "A normal");
    memcpy(snap, sim_flash, sizeof snap);
    fake_ms += OTA_CONFIRM_MIN_UP_MS;
    CHECK(ota_confirm_request() == 1, "confirm on the reverted image: no-op");
    fake_ms += OTA_SELF_CONFIRM_MS;
    ota_trial_poll(1, 1, 1);
    CHECK(memcmp(sim_flash, snap, sizeof snap) == 0, "reverted image: flash untouched");
    CHECK(entry_mark(1) != WMPT_CONF_MARK && reboot_device() == SLOT_A, "B stays unconfirmed");

    /* Hang (no loop passes): nothing confirms; the app loop's ota_tick or the
     * hang guard's reset both boot A.  A plain reset is the guard's action. */
    b = into_trial();
    fake_ms += OTA_CONFIRM_TIMEOUT_MS * 3;
    CHECK(ota_trial_revert_armed() == 1, "hung trial: guard sees revert armed");
    CHECK(reboot_device() == SLOT_A, "guard reset -> A");

    /* A failed confirm write is retried every OTA_CONFIRM_RETRY_MS. */
    b = into_trial();
    fake_ms = b + OTA_SELF_CONFIRM_MS;
    sim_fail_op = sim_ops;                  /* the confirm's first flash op fails */
    ota_trial_poll(1, 1, 1);                /* API session starts now */
    fake_ms += OTA_SELF_CONFIRM_API_MS;
    ota_trial_poll(1, 1, 1);
    CHECK(ota_boot_check() == 1, "failed confirm write: still a trial");
    CHECK(boot(&slot) == 0 && slot == SLOT_A, "failed confirm: still reverts");
    fake_ms += OTA_CONFIRM_RETRY_MS - 1;
    long ops = sim_ops;
    ota_trial_poll(1, 1, 1);
    CHECK(sim_ops == ops, "no retry before %u ms", (unsigned)OTA_CONFIRM_RETRY_MS);
    fake_ms += 1;
    ota_trial_poll(1, 1, 1);
    CHECK(ota_boot_check() == 0 && boot(&slot) == 0 && slot == SLOT_B, "retry confirmed");

    /* Previous image invalid: not revert-armed (guard inert), fallback still confirms. */
    reset_world();
    CHECK(upload(imgA, lenA, NULL, 0, lenA) == OTA_OK, "update");
    sim_flash[SLOT_A + 0x4000] ^= 0xFF;
    CHECK(reboot_device() == SLOT_B && ota_boot_check() == 1, "trial, no fallback");
    CHECK(ota_trial_revert_armed() == 0, "no revert possible -> guard inert");
    b = fake_ms;
    t = run_trial(b, OTA_CONFIRM_TIMEOUT_MS + 1000, api_from_20s, ip_always, 1, &what);
    CHECK(what == 'C' && t == OTA_SELF_CONFIRM_MS, "no-fallback trial self-confirms (%c %u)",
          what ? what : '-', (unsigned)t);

    /* First boot after a foreign install (never confirmed): same rules. */
    build_base_c(0);
    reset_world();
    CHECK(reboot_device() == SLOT_A && ota_boot_check() == 1, "foreign install: trial");
    fake_ms += 1000;
    CHECK(ota_confirm_request() == OTA_E_EARLY, "foreign install: early refused");
    b = fake_ms - 1000;
    t = run_trial(b, OTA_CONFIRM_TIMEOUT_MS + 1000, api_none, ip_always, 1, &what);
    CHECK(what == 'R' && boot(&slot) == 0 && slot == SLOT_B, "foreign install: reverts without HA");
    build_base();
}

/* ------------------------------------------------------------ HTTP */

static uint8_t req[0x90000];

static size_t mkreq(const char *method, const char *path, const char *hdrs,
                    const uint8_t *body, size_t blen, long clen)
{
    int n = snprintf((char *)req, 1024, "%s %s HTTP/1.1\r\nHost: x\r\n%s", method, path, hdrs);
    if (clen >= 0)
        n += snprintf((char *)req + n, 64, "Content-Length: %ld\r\n", clen);
    n += snprintf((char *)req + n, 8, "\r\n");
    if (blen)
        memcpy(req + n, body, blen);
    return (size_t)n + blen;
}

static void serve(fake_conn *c, int max_polls)
{
    for (int i = 0; i < max_polls && !(c && c->closed); i++) {
        ota_server_poll();
        fake_ms += 5;
    }
}

static int status_of(fake_conn *c)
{
    const char *o = (const char *)c->out, *last = NULL;
    c->out[c->out_len < sizeof c->out ? c->out_len : sizeof c->out - 1] = 0;
    for (const char *p = o; (p = strstr(p, "HTTP/1.1 ")); p++)
        last = p;
    return last ? atoi(last + 9) : -1;
}

static void http_setup(void)
{
    reset_world();
    fake_net_reset();
    ota_server_reset_state();
    CHECK(ota_server_init(8266, "s3cret") == OTA_OK, "server init");
}

static void test_http(void)
{
    char hdr[256], hex[33];
    uint8_t md5[16];
    uint32_t slot;
    md5_of(imgA, lenA, md5);
    for (int i = 0; i < 16; i++) sprintf(hex + 2 * i, "%02x", md5[i]);

    http_setup();
    fake_conn *c = fake_connect(req, mkreq("GET", "/version", "", NULL, 0, -1), 0);
    c->max_send = 7;                            /* force partial sends */
    serve(c, 2000);
    CHECK(status_of(c) == 200 && strstr((char *)c->out, "\"running\":\"A\""), "GET /version: %s", c->out);
    CHECK(c->closed, "version conn closed");

    /* Good upload, MD5, Expect: 100-continue, 1460-byte segments. */
    http_setup();
    snprintf(hdr, sizeof hdr, "X-OTA-Password: s3cret\r\nX-OTA-MD5: %s\r\nExpect: 100-continue\r\n", hex);
    size_t total = mkreq("POST", "/update", hdr, imgA, lenA, (long)lenA);
    c = fake_connect(req, total, 1460);
    c->stop_at = total - lenA;                  /* client waits for 100 */
    c->close_at_end = 0;
    serve(c, 50);
    CHECK(c->out_len == 25 && memcmp(c->out, "HTTP/1.1 100 Continue\r\n\r\n", 25) == 0,
          "100-continue sent before the body");
    c->stop_at = total;
    c->close_at_end = 1;
    serve(c, 20000);
    CHECK(status_of(c) == 200 && strstr((char *)c->out, "\"ok\":true"), "upload: %s", c->out);
    CHECK(boot(&slot) == 0 && slot == SLOT_B, "http upload switched");
    CHECK(fake_reboots == 0, "no reboot before delay");
    serve(NULL, 400);
    CHECK(fake_reboots == 1, "reboot after delay (%d)", fake_reboots);
    save("http_good.bin");

    /* Trial: further uploads refused until confirmed. */
    reboot_device();
    fake_net_reset();
    ota_server_reset_state();
    ota_server_init(8266, "s3cret");            /* runs ota_boot_check() */
    c = fake_connect(req, mkreq("POST", "/update", "X-OTA-Password: s3cret\r\n", imgB, lenB, (long)lenB), 0);
    serve(c, 20000);
    CHECK(status_of(c) == 409 && strstr((char *)c->out, "unconfirmed"), "trial 409: %s", c->out);
    c = fake_connect(req, mkreq("GET", "/version", "", NULL, 0, -1), 0);
    serve(c, 2000);
    CHECK(strstr((char *)c->out, "\"trial\":true") && strstr((char *)c->out, "\"running\":\"B\""),
          "version in trial: %s", c->out);
    ota_mark_good();

    /* Wrong password: 401, flash unchanged, then lockout. */
    http_setup();
    memcpy(snap, sim_flash, sizeof snap);
    c = fake_connect(req, mkreq("POST", "/update", "X-OTA-Password: nope\r\n", imgA, lenA, (long)lenA), 0);
    serve(c, 20000);
    CHECK(status_of(c) == 401, "wrong password: %d", status_of(c));
    CHECK(memcmp(sim_flash, snap, sizeof snap) == 0, "wrong password wrote flash");
    c = fake_connect(req, mkreq("POST", "/update", "X-OTA-Password: s3cret\r\n", imgA, lenA, (long)lenA), 0);
    serve(c, 20000);
    CHECK(status_of(c) == 429, "lockout: %d", status_of(c));
    fake_ms += 4000;
    c = fake_connect(req, mkreq("POST", "/update", "X-OTA-Password: s3cret\r\n", imgA, lenA, (long)lenA), 0);
    serve(c, 20000);
    CHECK(status_of(c) == 200, "after lockout: %d", status_of(c));

    /* Missing password, password prefix, no Content-Length, chunked, too big. */
    struct { const char *hdrs; long clen; int want; const char *what; } cases[] = {
        { "", (long)lenA, 401, "no password" },
        { "X-OTA-Password: s3cre\r\n", (long)lenA, 401, "password prefix" },
        { "X-OTA-Password: s3cret\r\n", -1, 411, "no content-length" },
        { "X-OTA-Password: s3cret\r\nTransfer-Encoding: chunked\r\n", -1, 411, "chunked" },
        { "X-OTA-Password: s3cret\r\n", 0x80001, 413, "too large" },
        { "X-OTA-Password: s3cret\r\nX-OTA-MD5: zz\r\n", (long)lenA, 400, "bad md5 header" },
        { "X-OTA-Password: s3cret\r\nX-OTA-MD5: 00000000000000000000000000000000\r\n", (long)lenA, 422, "md5 mismatch" },
    };
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        http_setup();
        memcpy(snap, sim_flash, sizeof snap);
        size_t blen = cases[k].clen > 0 && (size_t)cases[k].clen <= lenA ? (size_t)cases[k].clen : lenA;
        c = fake_connect(req, mkreq("POST", "/update", cases[k].hdrs, imgA, blen, cases[k].clen), 0);
        serve(c, 20000);
        CHECK(status_of(c) == cases[k].want, "%s: %d (%s)", cases[k].what, status_of(c), c->out);
        CHECK(boot(&slot) == 0 && slot == SLOT_A, "%s: no switch", cases[k].what);
        CHECK(region_eq(snap, 0x4000, 0x6000), "%s: table touched", cases[k].what);
        CHECK(c->closed, "%s: closed", cases[k].what);
    }

    /* Corrupted image over HTTP -> 422. */
    http_setup();
    static uint8_t bad[0x80000];
    memcpy(bad, imgA, lenA);
    bad[0x30000] ^= 0x80;
    c = fake_connect(req, mkreq("POST", "/update", "X-OTA-Password: s3cret\r\n", bad, lenA, (long)lenA), 0);
    serve(c, 20000);
    CHECK(status_of(c) == 422 && strstr((char *)c->out, "segment_crc_mismatch"), "corrupt: %s", c->out);
    CHECK(boot(&slot) == 0 && slot == SLOT_A, "corrupt: no switch");
    serve(NULL, 400);
    CHECK(fake_reboots == 0, "corrupt: no reboot");

    /* Truncated upload: client hangs up half way -> no switch, then a good one works. */
    http_setup();
    size_t n = mkreq("POST", "/update", "X-OTA-Password: s3cret\r\n", imgA, lenA, (long)lenA);
    c = fake_connect(req, n, 0);
    c->stop_at = n / 2;
    serve(c, 20000);
    CHECK(c->closed && status_of(c) == -1, "truncated: closed without reply");
    CHECK(boot(&slot) == 0 && slot == SLOT_A, "truncated: no switch");
    ota_info_t in;
    CHECK(ota_get_info(&in) == OTA_OK && !in.in_progress, "truncated: idle again");
    c = fake_connect(req, n, 0);
    serve(c, 20000);
    CHECK(status_of(c) == 200, "after truncation: %d", status_of(c));

    /* Stalled client -> 408 after the timeout. */
    http_setup();
    c = fake_connect(req, n, 0);
    c->stop_at = n / 3;
    c->close_at_end = 0;
    serve(c, 10000);
    CHECK(status_of(c) == 408, "stall: %d", status_of(c));
    CHECK(boot(&slot) == 0 && slot == SLOT_A, "stall: no switch");

    /* Second client while busy -> 503; 404; 405. */
    http_setup();
    c = fake_connect(req, n, 0);
    c->stop_at = 5000;
    c->close_at_end = 0;
    serve(c, 50);
    fake_conn *c2 = fake_connect((const uint8_t *)"GET /version HTTP/1.1\r\n\r\n", 25, 0);
    serve(c2, 50);
    CHECK(c2->closed && strstr((char *)c2->out, "503"), "busy -> 503");
    c->stop_at = c->in_len;
    c->close_at_end = 1;
    serve(c, 20000);
    CHECK(status_of(c) == 200, "first client finished: %d", status_of(c));

    http_setup();
    static const char r404[] = "GET /nope HTTP/1.1\r\n\r\n", r405[] = "GET /update HTTP/1.1\r\n\r\n";
    c = fake_connect((const uint8_t *)r404, sizeof r404 - 1, 0);
    serve(c, 2000);
    CHECK(status_of(c) == 404, "404: %d", status_of(c));
    c = fake_connect((const uint8_t *)r405, sizeof r405 - 1, 0);
    serve(c, 2000);
    CHECK(status_of(c) == 405, "405: %d", status_of(c));

    /* Empty password configured: updates disabled. */
    reset_world();
    fake_net_reset();
    ota_server_reset_state();
    ota_server_init(8266, "");
    c = fake_connect(req, mkreq("POST", "/update", "X-OTA-Password: \r\n", imgA, lenA, (long)lenA), 0);
    serve(c, 20000);
    CHECK(status_of(c) == 403, "no password configured: %d", status_of(c));
}

static fake_conn *confirm_req(const char *hdrs, long clen)
{
    static uint8_t r[512];
    int n = snprintf((char *)r, sizeof r, "POST /confirm HTTP/1.1\r\nHost: x\r\n%s", hdrs);
    if (clen >= 0)
        n += snprintf((char *)r + n, sizeof r - (size_t)n, "Content-Length: %ld\r\n", clen);
    n += snprintf((char *)r + n, sizeof r - (size_t)n, "\r\n");
    if (clen > 0)
        n += snprintf((char *)r + n, sizeof r - (size_t)n, "%.*s", (int)clen, "xxxxxxxxxxxxxxxx");
    fake_conn *c = fake_connect(r, (size_t)n, 0);
    serve(c, 2000);
    return c;
}

static void test_http_confirm(void)
{
    static const char ok_pw[] = "X-OTA-Password: s3cret\r\n";
    uint32_t slot;
    fake_conn *c;

    /* Confirmed image: authenticated POST /confirm is a harmless no-op. */
    http_setup();
    memcpy(snap, sim_flash, sizeof snap);
    c = confirm_req(ok_pw, 0);
    CHECK(status_of(c) == 200 && strstr((char *)c->out, "\"confirmed\":false") &&
          strstr((char *)c->out, "\"trial\":false"), "confirm on normal image: %s", c->out);
    c = confirm_req(ok_pw, -1);
    CHECK(status_of(c) == 200, "confirm without Content-Length: %d", status_of(c));
    CHECK(memcmp(sim_flash, snap, sizeof snap) == 0, "no-op confirm wrote flash");
    serve(NULL, 400);
    CHECK(fake_reboots == 0, "no-op confirm: no reboot");

    /* Auth, shared lockout with /update, method, body. */
    c = confirm_req("X-OTA-Password: nope\r\n", 0);
    CHECK(status_of(c) == 401, "confirm wrong password: %d", status_of(c));
    c = confirm_req(ok_pw, 0);
    CHECK(status_of(c) == 429, "confirm locked out: %d", status_of(c));
    fake_ms += 4000;
    c = confirm_req("", 0);
    CHECK(status_of(c) == 401, "confirm without password: %d", status_of(c));
    c = fake_connect(req, mkreq("POST", "/update", ok_pw, imgA, lenA, (long)lenA), 0);
    serve(c, 20000);
    CHECK(status_of(c) == 429, "/update locked out by a bad /confirm: %d", status_of(c));
    fake_ms += 4000;
    c = fake_connect((const uint8_t *)"GET /confirm HTTP/1.1\r\n\r\n", 25, 0);
    serve(c, 2000);
    CHECK(status_of(c) == 405, "GET /confirm: %d", status_of(c));
    c = confirm_req(ok_pw, 5);
    CHECK(status_of(c) == 400 && c->closed, "confirm with a body: %d", status_of(c));
    CHECK(memcmp(sim_flash, snap, sizeof snap) == 0, "rejected confirms wrote flash");

    /* Trial image: too early -> 409 + retry hint, then confirmed. */
    reset_world();
    CHECK(upload(imgA, lenA, NULL, 0, lenA) == OTA_OK, "update");
    reboot_device();
    fake_net_reset();
    ota_server_reset_state();
    CHECK(ota_server_init(8266, "s3cret") == OTA_OK, "server init (trial)");
    CHECK(ota_boot_check() == 1, "trial");
    c = fake_connect(req, mkreq("GET", "/version", "", NULL, 0, -1), 0);
    serve(c, 2000);
    CHECK(strstr((char *)c->out, "\"trial\":true") && strstr((char *)c->out, "\"confirm\":\"post\"") &&
          strstr((char *)c->out, "\"confirm_in_s\":") && strstr((char *)c->out, "\"self_confirm\":false"),
          "version in trial: %s", c->out);
    c = confirm_req("X-OTA-Password: nope\r\n", 0);
    CHECK(status_of(c) == 401 && ota_boot_check() == 1, "trial: wrong password does not confirm");
    fake_ms += 4000;
    c = confirm_req(ok_pw, 0);
    CHECK(status_of(c) == 409 && strstr((char *)c->out, "too_early") &&
          strstr((char *)c->out, "\"retry_in_ms\":"), "trial early: %s", c->out);
    CHECK(ota_boot_check() == 1 && boot(&slot) == 0 && slot == SLOT_A, "early: still trial, reverts");
    ota_trial_poll(1, 1, 1);                /* API session reported */
    fake_ms += OTA_CONFIRM_MIN_UP_MS;
    c = fake_connect(req, mkreq("GET", "/version", "", NULL, 0, -1), 0);
    serve(c, 2000);
    CHECK(strstr((char *)c->out, "\"confirm_in_s\":0") && strstr((char *)c->out, "\"api_seen\":true") &&
          strstr((char *)c->out, "\"self_confirm\":true"), "version ready: %s", c->out);
    c = confirm_req(ok_pw, 0);
    CHECK(status_of(c) == 200 && strstr((char *)c->out, "\"confirmed\":true") &&
          strstr((char *)c->out, "\"running\":\"B\"") && strstr((char *)c->out, "\"next_boot\":\"B\""),
          "trial confirm: %s", c->out);
    CHECK(boot(&slot) == 0 && slot == SLOT_B && entry_mark(1) == WMPT_CONF_MARK, "confirmed over HTTP");
    serve(NULL, 400);
    CHECK(fake_reboots == 0, "confirm: no reboot");
    c = confirm_req(ok_pw, 0);
    CHECK(status_of(c) == 200 && strstr((char *)c->out, "\"confirmed\":false"), "repeat: no-op");
    c = fake_connect(req, mkreq("GET", "/version", "", NULL, 0, -1), 0);
    serve(c, 2000);
    CHECK(strstr((char *)c->out, "\"trial\":false") && strstr((char *)c->out, "\"running\":\"B\""),
          "version after confirm: %s", c->out);
    c = fake_connect(req, mkreq("POST", "/update", ok_pw, imgB, lenB, (long)lenB), 0);
    serve(c, 20000);
    CHECK(status_of(c) == 200, "upload accepted after HTTP confirm: %d", status_of(c));

    /* No password configured: /confirm disabled too. */
    reset_world();
    fake_net_reset();
    ota_server_reset_state();
    ota_server_init(8266, "");
    c = confirm_req("X-OTA-Password: \r\n", 0);
    CHECK(status_of(c) == 403, "confirm, no password configured: %d", status_of(c));
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s IMAGE_DIR OUT_DIR\n", argv[0]);
        return 2;
    }
    test_verbose = getenv("OTA_VERBOSE") != NULL;
    out_dir = argv[2];
    char p[512];
    size_t rawlen;
    snprintf(p, sizeof p, "%s/appfwA.raw", argv[1]);
    imgA = load_file(p, &rawlen);
    lenA = mrvl_len(imgA);
    snprintf(p, sizeof p, "%s/appfwB.raw", argv[1]);
    imgB = load_file(p, &rawlen);
    lenB = mrvl_len(imgB);
    printf("images: A %zu bytes, B %zu bytes\n", lenA, lenB);
    build_base_c(0);                /* as stock wrote it (wmpt.py cross-check) */
    memcpy(sim_flash, base, sizeof base);
    save("base.bin");
    build_base();

    struct { const char *name; void (*fn)(void); } tests[] = {
        { "checksums", test_checksums }, { "table", test_table },
        { "good update + confirm", test_good_update }, { "confirm timeout / revert", test_confirm_timeout },
        { "first boot after stock/UART install", test_first_install },
        { "bad images", test_bad_images }, { "write guard", test_guard },
        { "ping-pong", test_pingpong }, { "power cuts", test_power_cuts },
        { "trial confirm policy", test_trial_policy },
        { "http", test_http }, { "http /confirm", test_http_confirm },
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int f0 = failures;
        tests[i].fn();
        printf("%-28s %s\n", tests[i].name, failures == f0 ? "ok" : "FAIL");
    }
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
