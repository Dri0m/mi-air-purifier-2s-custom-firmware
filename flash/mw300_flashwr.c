/*
 * mw300_flashwr.c - 88MW300 port of the guarded flash writer (RAM stub).
 *
 * Loaded by the UART boot ROM at 0x00110000 together with a parameter block
 * at +FW_PARAM_OFF and the payload at +FW_PAYLOAD_OFF (see flashwr_core.h).
 * All the logic and every guard lives in flashwr_core.c; this file only
 * provides the pins:
 *   - SPI flash bit-banged on GPIO28..33 (pinmux function 1 = GPIO), as in
 *     the proven dumper mw300_stub.c;
 *   - report line bit-banged at 115200 8N1 on GPIO2, K = 67 delay loops/bit
 *     (the delay loop runs ~8.05 M iterations/s, mw300_bb2 sweep 64..71 OK);
 *   - our own fault vector table (as mw300_oracle.c), so a fault prints
 *     "ERROR fault" and still resets.
 * When fw_run() returns, the stub prints "RESET" and requests a system reset
 * through SCB->AIRCR.  With the BOOT straps grounded the chip is back in the
 * UART boot ROM and the next chunk needs no power cycle.
 */
#include <stdint.h>
#include "flashwr_core.h"

#define GPIO_BASE   0x46060000u
#define PINMUX_BASE 0x48010000u
#define PMU_PERI_CLK_EN (*(volatile uint32_t *)0x480A007Cu)

#define SCB_VTOR  (*(volatile uint32_t *)0xE000ED08u)
#define SCB_AIRCR (*(volatile uint32_t *)0xE000ED0Cu)
#define SCB_SHCSR (*(volatile uint32_t *)0xE000ED24u)

#define PIN_CS   28
#define PIN_CLK  29
#define PIN_MOSI 30
#define PIN_MISO 31
#define PIN_WP   32
#define PIN_HOLD 33
#define TX_PIN   2

#ifndef BB_K
#define BB_K 67u
#endif

static inline void gpio_dir_out(int pin)
{
	*(volatile uint32_t *)(GPIO_BASE + 0x0cu + 4u * (uint32_t)(pin >> 5)) |=
		1u << (pin & 31);
}

static inline void gpio_dir_in(int pin)
{
	*(volatile uint32_t *)(GPIO_BASE + 0x0cu + 4u * (uint32_t)(pin >> 5)) &=
		~(1u << (pin & 31));
}

/* GPSR (set) at 0x18 + 4*bank, GPCR (clear) at 0x24 + 4*bank. */
static inline void gpio_set(int pin, int value)
{
	*(volatile uint32_t *)(GPIO_BASE + (value ? 0x18u : 0x24u) +
			       4u * (uint32_t)(pin >> 5)) = 1u << (pin & 31);
}

static inline int gpio_get(int pin)
{
	return (int)((*(volatile uint32_t *)(GPIO_BASE + 4u * (uint32_t)(pin >> 5)) >>
		      (pin & 31)) & 1u);
}

static inline void pinmux_set(int pin, uint32_t func, int input)
{
	*(volatile uint32_t *)(PINMUX_BASE + 4u * (uint32_t)pin) =
		(func & 7u) | (input ? (1u << 3) : 0u);
}

static inline void dly(void)
{
	__asm__ volatile("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop"
			 ::: "memory");
}

static inline void bb_delay(uint32_t n)
{
	__asm__ volatile("1:\n\tsubs %0, %0, #1\n\tbne 1b" : "+r"(n) : : "cc");
}

/* ---------------------------------------------------------------- port */

void fwp_putc(char c)
{
	uint32_t frame = ((uint32_t)(uint8_t)c << 1) | 0x200u;

	for (int i = 0; i < 10; i++) {
		*(volatile uint32_t *)(GPIO_BASE + ((frame & 1u) ? 0x18u : 0x24u)) =
			1u << TX_PIN;
		frame >>= 1;
		bb_delay(BB_K);
	}
}

void fwp_cs(int high)
{
	gpio_set(PIN_CS, high);
	dly();
}

/* Mode 0: data set while CLK is low, sampled by the flash on the rising
 * edge; MISO sampled while CLK is high (the dumper's proven timing). */
uint8_t fwp_xfer(uint8_t out)
{
	uint8_t in = 0;

	for (int i = 7; i >= 0; i--) {
		gpio_set(PIN_MOSI, (out >> i) & 1);
		dly();
		gpio_set(PIN_CLK, 1);
		dly();
		in = (uint8_t)((in << 1) | gpio_get(PIN_MISO));
		gpio_set(PIN_CLK, 0);
		dly();
	}
	return in;
}

void fwp_wait(void)
{
	bb_delay(800u);               /* ~100 us */
}

/* ---------------------------------------------------------------- reset */

static void puts_(const char *s)
{
	while (*s)
		fwp_putc(*s++);
}

static void __attribute__((noreturn)) reset(void)
{
	gpio_set(PIN_CS, 1);
	bb_delay(80000u);             /* ~10 ms: line idle, CS settled */
	__asm__ volatile("dsb" ::: "memory");
	SCB_AIRCR = (0x05FAu << 16) | (1u << 2);
	__asm__ volatile("dsb" ::: "memory");
	for (;;)
		;
}

static void __attribute__((noreturn)) fault(void)
{
	puts_("\r\nERROR fault\r\nRESET\r\n");
	reset();
}

/* Exceptions 1..15 only (interrupts stay masked, cpsid i in startup.S).
 * Filled at run time in .bss, which lives outside the load image. */
__attribute__((aligned(128)))
static void (*vectors[16])(void);

extern char _start[];

void main(void)
{
	/* _start is a Thumb symbol: bit 0 is set. */
	uint32_t base = (uint32_t)_start & ~1u;
	const struct fw_params *p = (const struct fw_params *)(base + FW_PARAM_OFF);
	const uint8_t *payload = (const uint8_t *)(base + FW_PAYLOAD_OFF);

	vectors[0] = (void (*)(void))0x00160000u;
	for (int i = 1; i < 16; i++)
		vectors[i] = fault;
	SCB_VTOR = (uint32_t)vectors;
	SCB_SHCSR |= (7u << 16);      /* MemManage/BusFault/UsageFault enable */
	__asm__ volatile("dsb\n\tisb" ::: "memory");

	/* Report line: GPIO clock on (PERI_CLK_EN bits are gates, 0 = on),
	 * GPIO2 function 0 = GPIO, idle high. */
	PMU_PERI_CLK_EN &= ~(1u << 4);
	pinmux_set(TX_PIN, 0, 0);
	gpio_set(TX_PIN, 1);
	gpio_dir_out(TX_PIN);
	bb_delay(800000u);            /* ~0.1 s idle line */

	/* QSPI pins to GPIO; levels first so CS never glitches low. */
	pinmux_set(PIN_CS, 1, 0);
	pinmux_set(PIN_CLK, 1, 0);
	pinmux_set(PIN_MOSI, 1, 0);
	pinmux_set(PIN_MISO, 1, 1);
	pinmux_set(PIN_WP, 1, 0);
	pinmux_set(PIN_HOLD, 1, 0);
	gpio_set(PIN_CS, 1);
	gpio_set(PIN_CLK, 0);
	gpio_set(PIN_MOSI, 0);
	gpio_set(PIN_WP, 1);          /* /WP high: not write protected by pin */
	gpio_set(PIN_HOLD, 1);        /* /HOLD inactive */
	gpio_dir_in(PIN_MISO);
	gpio_dir_out(PIN_CS);
	gpio_dir_out(PIN_CLK);
	gpio_dir_out(PIN_MOSI);
	gpio_dir_out(PIN_WP);
	gpio_dir_out(PIN_HOLD);

	(void)fw_run(p, payload);
	puts_("RESET\r\n");
	reset();
}
