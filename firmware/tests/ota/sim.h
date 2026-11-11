/* sim.h - host test doubles for the OTA module. */
#ifndef OTA_TEST_SIM_H
#define OTA_TEST_SIM_H
#include <stddef.h>
#include <stdint.h>
#include "ota_flash.h"

/* ---- simulated 2 MiB NOR flash (flash_sim.c) ---- */
extern uint8_t sim_flash[OTA_FLASH_SIZE];
extern long sim_budget;          /* mutating ops until power cut; <0 = never */
extern int sim_dead;             /* power has been cut */
extern long sim_ops;             /* mutating ops performed */
extern long sim_nor_violations;  /* program tried to turn a 0 bit into 1 */
extern int sim_cut_mode;         /* erase cut: 0 = 1st half erased, 1 = 2nd half */
extern long sim_running;         /* XIP offset reported, -1 = unknown */
extern int32_t sim_boot_flags;   /* -1 = unknown */
extern uint32_t sim_flip_addr;   /* one-shot: flip bit 0 when programming here */
extern long sim_fail_op;         /* op index that returns an error (no cut) */

void sim_faults_clear(void);
int sim_load(const char *path);
int sim_save(const char *path);

/* ---- boot2 model (flash_sim.c), from the boot2 disassembly ---- */
/* Returns 0 and the slot start that would run, -1 if boot2 would hang.
 * *fell_back = 1 if the preferred slot failed and the other one booted. */
int sim_boot2(uint32_t *slot, int *fell_back);

/* ---- hal_stub.c / netio_fake.c ---- */
extern uint32_t fake_ms;
extern int fake_reboots;
extern int test_verbose;

typedef struct {
    const uint8_t *in;
    size_t in_len, in_pos;
    size_t chunk;           /* max bytes per recv */
    size_t stop_at;         /* deliver up to here, then stall */
    int close_at_end;       /* recv returns 0 once everything delivered */
    int max_send;           /* bytes accepted per send call (0 = unlimited) */
    int accepted, closed;
    uint8_t out[16384];
    size_t out_len;
} fake_conn;

#define FAKE_MAX_CONN 16
extern fake_conn fake_conns[FAKE_MAX_CONN];
void fake_net_reset(void);
fake_conn *fake_connect(const uint8_t *req, size_t len, size_t chunk);
#endif
