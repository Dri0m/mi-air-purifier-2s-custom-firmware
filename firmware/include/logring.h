/* logring.h - RAM ring buffer of all console output (R3 remote logs).
 *
 * Writers (any context: tasks, ISRs, fault handlers, interrupts disabled):
 * logring_write() copies bytes into a 4 KB ring under a short critical
 * section (PRIMASK on the device, a test hook on the host).  It never
 * blocks, never allocates and drops the oldest bytes when full.  '\r' and
 * NUL are dropped; lines end with '\n'.
 *
 * Readers (the app task only, src/esphome/api_server.c): each keeps its own
 * absolute read cursor and pulls whole lines with logring_read_line(), so a
 * slow reader never holds anything up; if the writers lap it, it is told how
 * many bytes it lost and skips ahead to the next line start.
 *
 * Retention: logring_retain_attach() points the ring at a block of RAM that
 * survives a soft reset (device: AON RAM, section .nvram_uninit).  The last
 * LOGRING_RETAIN_LOG bytes of output are mirrored there (32-bit word stores
 * only), plus a fault reason (logring_set_reason(), CRC-checked).  At attach
 * the previous boot's record, if its magic is valid, is moved into RAM
 * (logring_prev_*()) and the block is re-armed for this boot.
 *
 * Portable C99; no allocation. */
#ifndef LOGRING_H
#define LOGRING_H
#include <stddef.h>
#include <stdint.h>

#ifndef LOGRING_SIZE
#define LOGRING_SIZE 4096u         /* power of two */
#endif
#define LOGRING_LINE_MAX 160u      /* longer lines are split */
#define LOGRING_RETAIN_LOG 512u    /* power of two, divides LOGRING_SIZE */
#define LOGRING_REASON_MAX 96u     /* multiple of 4 */

/* ESPHome LogLevel values (api.proto) */
#define LOGRING_LVL_ERROR 1
#define LOGRING_LVL_WARN  2
#define LOGRING_LVL_INFO  3
#define LOGRING_LVL_CONFIG 4
#define LOGRING_LVL_DEBUG 5

/* Retained record (AON RAM on the device).  Accessed as 32-bit words only. */
typedef struct {
	uint32_t magic;                    /* LOGRING_RETAIN_MAGIC */
	uint32_t magic_inv;                /* ~LOGRING_RETAIN_MAGIC */
	uint32_t log_base;                 /* ring position at attach */
	uint32_t head;                     /* ring position mirrored up to */
	uint32_t reason_len;               /* 0 = no reason */
	uint32_t reason_crc;               /* CRC-32 of reason[0..reason_len) */
	uint32_t reason[LOGRING_REASON_MAX / 4];
	uint32_t log[LOGRING_RETAIN_LOG / 4];
} logring_retain;
#define LOGRING_RETAIN_MAGIC 0x4C4F4752u   /* "LOGR" */

/* ---- writers: any context ---- */
void logring_write(const char *s, size_t n);
void logring_puts(const char *s);
/* Record why the device is about to reset (fatal hooks): s up to its first
 * '\r'/'\n', at most LOGRING_REASON_MAX bytes.  Stored in the retained
 * record only; also call logring_write() for the ring itself. */
void logring_set_reason(const char *s);

/* ---- readers: one task ---- */
uint32_t logring_head(void);       /* total bytes written (mod 2^32) */
/* Cursor at the oldest complete line still in the ring (history replay). */
uint32_t logring_replay_cursor(void);
#define LOGRING_LOST (-1)
/* Read the line at *cur into buf (NUL-terminated, '\n' stripped).
 * Returns its length (> 0; an empty line is skipped) and advances *cur; 0
 * if no complete line is available (with flush_partial, an unterminated
 * tail is returned as a line instead); LOGRING_LOST if *cur was overwritten:
 * *lost = bytes skipped, *cur moved to the next line start.  Lines longer
 * than cap - 1 are returned in pieces.  cap >= 2. */
int logring_read_line(uint32_t *cur, char *buf, size_t cap, int flush_partial,
		      uint32_t *lost);
/* ESPHome level of a line: an "[E]" / "[W]" / "[I]" / "[C]" / "[D]" / "[V]"
 * prefix wins; else "***" = error, failure words = warning, else info. */
uint8_t logring_line_level(const char *line);

/* ---- retention ---- */
void logring_retain_attach(logring_retain *r);
/* The previous boot's retained log tail (complete lines, oldest first) and
 * reason.  prev_valid: 0 = no valid record (cold boot or not retained). */
int logring_prev_valid(void);
const char *logring_prev_reason(void);         /* "" if none */
/* Iterate previous-boot lines: *pos starts at 0; returns the length, 0 at
 * the end.  buf as in logring_read_line(). */
int logring_prev_line(unsigned *pos, char *buf, size_t cap);

/* ---- host builds only ---- */
#if !defined(__arm__)
/* Critical-section hooks (default: none, single-threaded). */
void logring_set_lock(void (*lock)(void), void (*unlock)(void));
void logring_reset(void);          /* empty the ring, detach retention */
#endif
#endif
