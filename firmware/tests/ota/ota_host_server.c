/* ota_host_server.c - run the real OTA HTTP server on the host against a
 * simulated flash file, for tools/ota_upload.py end-to-end tests.
 *
 * usage: ota_host_server FLASH_FILE PORT PASSWORD [--confirm|--api|--no-api-hook]
 * A device reboot is simulated: flash saved, boot2 model picks the slot,
 * RAM state reset, server restarted.  Like the app loop, every pass calls
 * ota_trial_poll(ip up, ota_server_ready(), api_clients):
 *   (default)       api_clients = 0: no HA session, only POST /confirm
 *                   confirms, otherwise the timeout reverts
 *   --api           api_clients = 1 from 1 s after boot: fallback confirm
 *   --no-api-hook   api_clients = -1 (esphome hook missing)
 *   --confirm       old behaviour: ota_mark_good() right at boot
 * Built with scaled timings (tests/ota/host.mk), so a test takes seconds. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "ota_internal.h"
#include "sim.h"

static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* Simulated power-on: boot2 picks the slot, RAM state is lost.  The server
 * itself is restarted by ota_server_init() in main (closes the listener). */
static uint32_t boot_ms;

static int power_on(int confirm)
{
    uint32_t slot;
    int fb;
    if (sim_boot2(&slot, &fb) != 0) {
        printf("BOOT2 HANG\n");
        return -1;
    }
    sim_running = slot;
    ota_reset_state();
    boot_ms = fake_ms;
    int t = ota_boot_check();
    printf("BOOT slot %c%s%s\n", slot == 0xA000 ? 'A' : 'B', fb ? " (boot2 fallback!)" : "",
           t == 1 ? " trial" : "");
    if (confirm && t == 1)
        printf("CONFIRM %s\n", ota_strerror(ota_mark_good()));
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s FLASH PORT PASSWORD [--confirm]\n", argv[0]);
        return 2;
    }
    const char *flash = argv[1], *pw = argv[3];
    uint16_t port = (uint16_t)atoi(argv[2]);
    const char *mode = argc > 4 ? argv[4] : "";
    int confirm = strcmp(mode, "--confirm") == 0;
    int api = strcmp(mode, "--api") == 0 ? 1 : strcmp(mode, "--no-api-hook") == 0 ? -1 : 0;
    test_verbose = getenv("OTA_VERBOSE") != NULL;
    if (sim_load(flash) != 0) {
        fprintf(stderr, "cannot load %s\n", flash);
        return 2;
    }
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    fake_ms = now_ms();
    if (power_on(confirm) < 0 || ota_server_init(port, pw) != OTA_OK) {
        fprintf(stderr, "server start failed\n");
        return 3;
    }
    printf("LISTENING %u\n", port);
    fflush(stdout);
    int seen = 0;
    while (!stop) {
        fake_ms = now_ms();
        int was_trial = ota_boot_check() == 1;      /* cached: RAM only */
        ota_trial_poll(1, ota_server_ready(),
                       api > 0 && fake_ms - boot_ms < 1000u ? 0 : api);
        ota_server_poll();
        if (was_trial && ota_boot_check() == 0 && fake_reboots == seen) {
            printf("CONFIRMED after %u ms\n", (unsigned)(fake_ms - boot_ms));
            fflush(stdout);
        }
        if (fake_reboots != seen) {
            seen = fake_reboots;
            sim_save(flash);
            printf("REBOOT\n");
            if (power_on(confirm) < 0)
                return 4;
            if (ota_server_init(port, pw) != OTA_OK) {
                usleep(200000);
                ota_server_init(port, pw);
            }
        }
        usleep(500);
    }
    sim_save(flash);
    return 0;
}
