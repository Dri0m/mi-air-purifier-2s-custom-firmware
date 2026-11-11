/*
 * mw300_stub.c - tiny RAM program loaded by the 88MW300 UART boot ROM.
 *
 * It bit-bangs the QSPI flash on GPIO28..33 (single-SPI read command 0x03)
 * and streams the contents out as bit-banged 115200 8N1 on GPIO2.  No PLL,
 * UART or QSPI-controller setup is needed; the QSPI pins default to their
 * QSPI function (pinmux 0), which we switch to GPIO (pinmux 1).
 *
 * Output protocol (ASCII lines, then raw bytes):
 *   MWDUMPER 2\r\n
 *   JEDEC EF 40 16\r\n
 *   BEGIN <length>\r\n
 *   <length raw bytes>
 *   \r\nDONE CRC xxxxxxxx\r\n   (CRC-32 as crc32_mw over the data)
 *
 * The length is read from word DUMP_LEN_ADDR (0x00112000 in the shipped
 * _110000 build, set by build_stub.sh).  The loader writes it there.
 * Zero means "use the JEDEC capacity table" (default 4 MiB if unknown).
 *
 * Wiring assumptions: QSPI_SSn=GPIO28, QSPI_CLK=GPIO29, QSPI_D0=GPIO30,
 * QSPI_D1=GPIO31, QSPI_D2=GPIO32, QSPI_D3=GPIO33 (MW300 datasheet Tables
 * 10/11).  D2/D3 are driven high so /WP and /HOLD are inactive.
 *
 * This program never writes to the flash and never touches the OTP.
 */
#include <stdint.h>

#define UART0_BASE 0x46040000u
#define UART_THR   (*(volatile uint32_t *)(UART0_BASE + 0x00))
#define UART_IER   (*(volatile uint32_t *)(UART0_BASE + 0x04))
#define UART_FCR   (*(volatile uint32_t *)(UART0_BASE + 0x08))
#define UART_LCR   (*(volatile uint32_t *)(UART0_BASE + 0x0c))
#define UART_LSR   (*(volatile uint32_t *)(UART0_BASE + 0x14))

#define GPIO_BASE   0x46060000u
#define PINMUX_BASE 0x48010000u
#define PMU_BASE    0x480a0000u
#define PMU_PERI_CLK_EN (*(volatile uint32_t *)(PMU_BASE + 0x07c))

#define PIN_CS   28
#define PIN_CLK  29
#define PIN_MOSI 30
#define PIN_MISO 31
#define PIN_WP   32
#define PIN_HOLD 33

/* Word written by the loader: requested dump length, 0 = auto.
 * Override at build time when linking at a base other than 0x00100000:
 *   -DDUMP_LEN_ADDR=0x00112000u  (for base 0x00110000) */
#ifndef DUMP_LEN_ADDR
#define DUMP_LEN_ADDR 0x00102000u
#endif

static inline uint32_t gpio_bank(int pin) { return (uint32_t)(pin >> 5); }
static inline uint32_t gpio_mask(int pin) { return 1u << (pin & 31); }

static inline void gpio_dir_out(int pin)
{
	*(volatile uint32_t *)(GPIO_BASE + 0x0c + 4 * gpio_bank(pin)) |= gpio_mask(pin);
}

static inline void gpio_dir_in(int pin)
{
	*(volatile uint32_t *)(GPIO_BASE + 0x0c + 4 * gpio_bank(pin)) &= ~gpio_mask(pin);
}

/* GPSR (set) at 0x18 + 4*bank, GPCR (clear) at 0x24 + 4*bank. */
static inline void gpio_set(int pin, int value)
{
	*(volatile uint32_t *)(GPIO_BASE + (value ? 0x18 : 0x24) +
			       4 * gpio_bank(pin)) = gpio_mask(pin);
}

static inline int gpio_get(int pin)
{
	return (int)((*(volatile uint32_t *)(GPIO_BASE + 0x00 +
					     4 * gpio_bank(pin)) >>
		      (pin & 31)) & 1u);
}

/* FSEL selects the pin function; DI_EN=1 enables the input buffer. */
static inline void pinmux_set(int pin, uint32_t func, int input)
{
	*(volatile uint32_t *)(PINMUX_BASE + 4 * pin) =
		(func & 7u) | (input ? (1u << 3) : 0u);
}

static inline void pinmux_gpio(int pin, int input)
{
	pinmux_set(pin, 1, input);
}

static inline void dly(void)
{
	__asm__ volatile("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop"
			 ::: "memory");
}

/* Bit-banged 115200 8N1 on GPIO2 (module TX, Debug_RX hole).  The ROM leaves
 * UART0 disabled, and the delay loop below runs ~8.05 M iterations/s; the
 * mw300_bb2 sweep decoded for 64..71, so 67 is the middle of the window. */
#ifndef BB_K
#define BB_K 67u
#endif
#define TX_PIN 2

static inline void bb_delay(uint32_t n)
{
	__asm__ volatile("1:\n\tsubs %0, %0, #1\n\tbne 1b" : "+r"(n) : : "cc");
}

static void putc_(char c)
{
	uint32_t frame = ((uint32_t)(uint8_t)c << 1) | 0x200u;

	for (int i = 0; i < 10; i++) {
		*(volatile uint32_t *)(GPIO_BASE + ((frame & 1u) ? 0x18 : 0x24)) =
			1u << TX_PIN;
		frame >>= 1;
		bb_delay(BB_K);
	}
}

static void puts_(const char *s)
{
	while (*s)
		putc_(*s++);
}

static void puthex8(uint8_t v)
{
	static const char hex[] = "0123456789ABCDEF";
	putc_(hex[v >> 4]);
	putc_(hex[v & 0x0f]);
}

static void putu32(uint32_t v)
{
	char buf[10];
	int i = 0;

	if (v == 0) {
		putc_('0');
		return;
	}
	while (v) {
		buf[i++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (i)
		putc_(buf[--i]);
}

static void spi_begin(void)
{
	gpio_set(PIN_CS, 0);
	dly();
}

static void spi_end(void)
{
	gpio_set(PIN_CS, 1);
	dly();
}

static void spi_tx(uint8_t b)
{
	int i;

	for (i = 7; i >= 0; i--) {
		gpio_set(PIN_MOSI, (b >> i) & 1);
		dly();
		gpio_set(PIN_CLK, 1);
		dly();
		gpio_set(PIN_CLK, 0);
		dly();
	}
}

static uint8_t spi_rx(void)
{
	uint8_t b = 0;
	int i;

	for (i = 0; i < 8; i++) {
		gpio_set(PIN_MOSI, 1);
		dly();
		gpio_set(PIN_CLK, 1);
		dly();
		b = (uint8_t)((b << 1) | gpio_get(PIN_MISO));
		gpio_set(PIN_CLK, 0);
		dly();
	}
	return b;
}

static uint32_t spi_jedec(void)
{
	uint32_t a, b, c;

	spi_begin();
	spi_tx(0x9f); /* JEDEC ID */
	a = spi_rx();
	b = spi_rx();
	c = spi_rx();
	spi_end();
	return (a << 16) | (b << 8) | c;
}

/* Standard single-SPI read (0x03) with a 24-bit address. */
static void spi_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
	uint32_t i;

	spi_begin();
	spi_tx(0x03);
	spi_tx((uint8_t)(addr >> 16));
	spi_tx((uint8_t)(addr >> 8));
	spi_tx((uint8_t)addr);
	for (i = 0; i < len; i++)
		buf[i] = spi_rx();
	spi_end();
}

static uint32_t jedec_size(uint32_t id)
{
	switch (id) {
	/* Winbond W25Qxx */
	case 0xef4014: return 1u << 20;
	case 0xef4015: return 2u << 20;
	case 0xef4016: return 4u << 20;
	case 0xef4017: return 8u << 20;
	case 0xef4018: return 16u << 20;
	/* GigaDevice GD25Qxx */
	case 0xc84014: return 1u << 20;
	case 0xc84015: return 2u << 20;
	case 0xc84016: return 4u << 20;
	case 0xc84017: return 8u << 20;
	case 0xc84018: return 16u << 20;
	/* EON EN25Qxx */
	case 0x1c4014: return 1u << 20;
	case 0x1c4015: return 2u << 20;
	case 0x1c4016: return 4u << 20;
	case 0x1c4017: return 8u << 20;
	case 0x1c4018: return 16u << 20;
	/* Micron / ST / Numonyx M25Pxx */
	case 0x204014: return 1u << 20;
	case 0x204015: return 2u << 20;
	case 0x204016: return 4u << 20;
	case 0x204017: return 8u << 20;
	case 0x204018: return 16u << 20;
	default:       return 4u << 20;
	}
}

/* PERI_CLK_EN bits are clock gates (0 = on).  The GPIO clock (bit 4) is
 * gated after reset.  GPIO2 function 0 = GPIO.  Verified with oracle.py. */
static void out_init(void)
{
	PMU_PERI_CLK_EN &= ~(1u << 4);           /* GPIO clock on */
	pinmux_set(TX_PIN, 0, 0);                /* GPIO2 = GPIO */
	gpio_set(TX_PIN, 1);                     /* idle high */
	gpio_dir_out(TX_PIN);
	bb_delay(800000);                        /* ~0.1 s of idle line */
}

/* CRC-32/IEEE reflected, init 0, no final xor: same as the boot ROM's and
 * uartboot_load.crc32_mw. */
static uint32_t crc32_upd(uint32_t crc, uint8_t b)
{
	crc ^= b;
	for (int k = 0; k < 8; k++)
		crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
	return crc;
}

static void puthex32(uint32_t v)
{
	puthex8((uint8_t)(v >> 24));
	puthex8((uint8_t)(v >> 16));
	puthex8((uint8_t)(v >> 8));
	puthex8((uint8_t)v);
}

/* Leave any continuous-read / power-down state.  Read-only commands:
 * 0xFF (exit continuous read), 0x66+0x99 (software reset), 0xAB (release
 * power-down). None of them write or erase. */
static void spi_wake(void)
{
	spi_begin(); spi_tx(0xff); spi_tx(0xff); spi_end();
	spi_begin(); spi_tx(0x66); spi_end();
	spi_begin(); spi_tx(0x99); spi_end();
	bb_delay(8000);
	spi_begin(); spi_tx(0xab); spi_end();
	bb_delay(8000);
}

void main(void)
{
	static uint8_t buf[256];
	uint32_t id, len, addr;

	uint32_t crc = 0;

	out_init();
	puts_("MWDUMPER 2\r\n");

	/* Switch the QSPI pins to GPIO.  Values are set before the pins
	 * become outputs so CS does not glitch low. */
	pinmux_gpio(PIN_CS, 0);
	pinmux_gpio(PIN_CLK, 0);
	pinmux_gpio(PIN_MOSI, 0);
	pinmux_gpio(PIN_MISO, 1);
	pinmux_gpio(PIN_WP, 0);
	pinmux_gpio(PIN_HOLD, 0);

	gpio_set(PIN_CS, 1);
	gpio_set(PIN_CLK, 0);
	gpio_set(PIN_MOSI, 0);
	gpio_set(PIN_WP, 1);
	gpio_set(PIN_HOLD, 1);

	gpio_dir_in(PIN_MISO);
	gpio_dir_out(PIN_CS);
	gpio_dir_out(PIN_CLK);
	gpio_dir_out(PIN_MOSI);
	gpio_dir_out(PIN_WP);
	gpio_dir_out(PIN_HOLD);

	spi_wake();
	id = spi_jedec();
	puts_("JEDEC ");
	puthex8((uint8_t)(id >> 16));
	putc_(' ');
	puthex8((uint8_t)(id >> 8));
	putc_(' ');
	puthex8((uint8_t)id);
	puts_("\r\n");

	if (id == 0x000000 || id == 0xffffff) {
		puts_("ERROR no-flash\r\n");
		for (;;)
			;
	}

	len = *(volatile uint32_t *)DUMP_LEN_ADDR;
	if (len == 0)
		len = jedec_size(id);

	puts_("BEGIN ");
	putu32(len);
	puts_("\r\n");

	addr = 0;
	while (addr < len) {
		uint32_t n = len - addr;
		uint32_t i;

		if (n > sizeof(buf))
			n = sizeof(buf);
		spi_read(addr, buf, n);
		for (i = 0; i < n; i++) {
			putc_((char)buf[i]);
			crc = crc32_upd(crc, buf[i]);
		}
		addr += n;
	}

	puts_("\r\nDONE CRC ");
	puthex32(crc);
	puts_("\r\n");
	for (;;)
		;
}
