/* ui.c - buzzer patterns (5.2), status LEDs + dimming (5.3, hardware map
 * 6.3) and a simple 1 bpp OLED renderer (5.4; not pixel-exact to stock).
 *
 * Framebuffer format assumed: row-major, stride (w+7)/8 bytes, MSB =
 * leftmost pixel, 1 = lit.  The HAL converts to the SSD1327's 4 bpp.
 */
#include <string.h>
#include "hal.h"
#include "pur_int.h"

#define BEEP_GAP_MS 100

static struct {
	/* buzzer */
	uint8_t bz_count;          /* tones left, including the current */
	bool bz_on;
	uint16_t bz_on_ms;
	uint32_t bz_t;
	/* screens */
	uint8_t trans;             /* transient screen, SCR_OFF if none */
	uint32_t trans_t, trans_ms;
	uint32_t wake_t, wake_ms;
	uint8_t shown;             /* screen seen by the last ui_tick */
	uint32_t shown_t;          /* when it last changed */
	/* outputs (cached, sent on change) */
	bool disp_on;
	int16_t contrast;          /* -1 = unknown */
	uint8_t led_r, led_g, led_o;
	bool leds_known;
	uint8_t last_screen;
	uint32_t fb_sum;
	uint32_t render_t;
	bool rendered;
} u;

void ui_init(void)
{
	memset(&u, 0, sizeof u);
	u.contrast = -1;
	u.last_screen = 0xff;
	u.shown_t = hal_millis();
	hal_buzzer(false);
}

/* ---------- buzzer ---------- */

void ui_beep(int pattern, bool enabled)
{
	if (pattern != BEEP_TRIPLE && !enabled)
		return;
	u.bz_count = pattern == BEEP_DOUBLE ? 2 : pattern == BEEP_TRIPLE ? 3 : 1;
	u.bz_on_ms = pattern == BEEP_LONG ? 500 : 100;
	u.bz_on = true;
	u.bz_t = hal_millis();
	hal_buzzer(true);
}

bool ui_buzzer_busy(void)
{
	return u.bz_count != 0;
}

static void buzzer_tick(uint32_t now)
{
	if (!u.bz_count)
		return;
	if (u.bz_on) {
		if (now - u.bz_t >= u.bz_on_ms) {
			hal_buzzer(false);
			u.bz_on = false;
			u.bz_t = now;
			if (--u.bz_count == 0)
				return;
		}
	} else if (now - u.bz_t >= BEEP_GAP_MS) {
		hal_buzzer(true);
		u.bz_on = true;
		u.bz_t = now;
	}
}

/* ---------- screen selection ---------- */

void ui_show(uint8_t screen, uint32_t ms)
{
	u.trans = screen;
	u.trans_t = hal_millis();
	u.trans_ms = ms;
}

void ui_clear_transient(void)
{
	u.trans = SCR_OFF;
	u.wake_ms = 0;
}

uint32_t ui_screen_since(void)
{
	return u.shown_t;
}

void ui_wake(uint32_t ms)
{
	u.wake_t = hal_millis();
	u.wake_ms = ms;
}

uint8_t ui_current_screen(const purifier_state *st, bool fault)
{
	uint32_t now = hal_millis();

	if (u.trans != SCR_OFF) {
		if (now - u.trans_t < u.trans_ms)
			return u.trans;
		u.trans = SCR_OFF;
	}
	if (fault)
		return SCR_FAULT;
	if (st->power && st->led)
		return SCR_MAIN;
	if (u.wake_ms && now - u.wake_t < u.wake_ms)
		return SCR_MAIN;
	u.wake_ms = 0;
	return SCR_OFF;
}

/* ---------- drawing ---------- */

static uint8_t *fb;
static uint16_t fbw, fbh, stride;

static void px(int x, int y)
{
	if (x < 0 || y < 0 || x >= fbw || y >= fbh)
		return;
	fb[y * stride + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
}

static void rect(int x, int y, int w, int h)
{
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++)
			px(x + i, y + j);
}

/* 5x7 font, ASCII 32..90, column-major, bit 0 = top row. */
static const uint8_t font[][5] = {
	{0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00},
	{0x00,0x07,0x00,0x07,0x00}, {0x14,0x7F,0x14,0x7F,0x14},
	{0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62},
	{0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00},
	{0x00,0x1C,0x22,0x41,0x00}, {0x00,0x41,0x22,0x1C,0x00},
	{0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08},
	{0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08},
	{0x00,0x60,0x60,0x00,0x00}, {0x20,0x10,0x08,0x04,0x02},
	{0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
	{0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
	{0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
	{0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
	{0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E},
	{0x00,0x36,0x36,0x00,0x00}, {0x00,0x56,0x36,0x00,0x00},
	{0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14},
	{0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06},
	{0x32,0x49,0x79,0x41,0x3E}, {0x7E,0x11,0x11,0x11,0x7E},
	{0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
	{0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41},
	{0x7F,0x09,0x09,0x09,0x01}, {0x3E,0x41,0x49,0x49,0x7A},
	{0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00},
	{0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41},
	{0x7F,0x40,0x40,0x40,0x40}, {0x7F,0x02,0x0C,0x02,0x7F},
	{0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
	{0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E},
	{0x7F,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31},
	{0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F},
	{0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F},
	{0x63,0x14,0x08,0x14,0x63}, {0x07,0x08,0x70,0x08,0x07},
	{0x61,0x51,0x49,0x45,0x43},
};
static const uint8_t glyph_degree[5] = {0x00,0x06,0x09,0x09,0x06};

static const uint8_t *glyph(char c)
{
	if (c == '~')
		return glyph_degree;
	if (c >= 'a' && c <= 'z')
		c = (char)(c - 32);
	if (c < 32 || c > 'Z')
		c = '?';
	return font[c - 32];
}

static int text_w(const char *s, int scale)
{
	int n = (int)strlen(s);
	return n ? (n * 6 - 1) * scale : 0;
}

static void text(int x, int y, const char *s, int scale)
{
	for (; *s; s++, x += 6 * scale) {
		const uint8_t *g = glyph(*s);
		for (int cx = 0; cx < 5; cx++)
			for (int cy = 0; cy < 7; cy++)
				if (g[cx] >> cy & 1)
					rect(x + cx * scale, y + cy * scale,
					     scale, scale);
	}
}

static void text_c(int y, const char *s, int scale)
{
	text((fbw - text_w(s, scale)) / 2, y, s, scale);
}

static char *utoa10(char *p, unsigned v)  /* writes, returns end */
{
	char tmp[10];
	int n = 0;
	do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (n)
		*p++ = tmp[--n];
	*p = 0;
	return p;
}

/* 7-segment big digit: segments a b c d e f g as bits 0..6 */
static const uint8_t seg7[10] = {
	0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f };

static void big_digit(int x, int y, int w, int h, int t, int d)
{
	uint8_t s = seg7[d];
	int hh = h / 2;
	if (s & 0x01) rect(x, y, w, t);                     /* a */
	if (s & 0x02) rect(x + w - t, y, t, hh + t / 2);    /* b */
	if (s & 0x04) rect(x + w - t, y + hh - t / 2, t, h - hh + t / 2); /* c */
	if (s & 0x08) rect(x, y + h - t, w, t);             /* d */
	if (s & 0x10) rect(x, y + hh - t / 2, t, h - hh + t / 2);         /* e */
	if (s & 0x20) rect(x, y, t, hh + t / 2);            /* f */
	if (s & 0x40) rect(x, y + hh - t / 2, w, t);        /* g */
}

static void big_number(int y, unsigned v)
{
	int h = fbh * 11 / 24, w = h * 3 / 5, t = h / 7, gap = t;
	int nd = v >= 100 ? 3 : v >= 10 ? 2 : 1;
	int x = (fbw - (3 * w + 2 * gap)) / 2 + (3 - nd) * (w + gap);
	unsigned div = nd == 3 ? 100 : nd == 2 ? 10 : 1;
	for (; div; div /= 10, x += w + gap)
		big_digit(x, y, w, h, t, (int)(v / div % 10));
}

static const char *mode_name(purifier_mode m)
{
	return m == MODE_SILENT ? "SILENT" : m == MODE_FAVORITE ? "FAVORITE" : "AUTO";
}

static void draw_main(const ui_view *v)
{
	const purifier_state *st = v->st;
	char buf[24], *p;
	int h = fbh;

	/* top line: mode, lock */
	if (st->mode == MODE_FAVORITE) {
		p = buf;
		memcpy(p, "FAV ", 4);
		if (st->favorite_level < FAVORITE_LEVELS)
			utoa10(p + 4, st->favorite_level);
		else
			strcpy(p + 4, "RPM");
		text(0, 0, buf, 1);
	} else {
		text(0, 0, mode_name(st->mode), 1);
	}
	if (st->child_lock)
		text(fbw - text_w("LOCK", 1), 0, "LOCK", 1);

	/* PM2.5 big digits, clamped 1..600 as stock */
	unsigned pm = st->pm25 < 1 ? 1 : st->pm25 > 600 ? 600 : st->pm25;
	int y = h / 8 + 1;
	big_number(y, pm);
	y += h * 11 / 24 + 4;
	text_c(y, v->sensor_fault ? "SENSOR ERROR" : "PM2.5 UG/M3", 1);

	/* temperature (rounded to whole C) and humidity (0..99) */
	int t = st->temp_dC;
	t = t >= 0 ? (t + 5) / 10 : -((-t + 5) / 10);
	p = buf;
	if (t < 0) { *p++ = '-'; t = -t; }
	p = utoa10(p, (unsigned)t);
	memcpy(p, "~C  ", 4);
	p = utoa10(p + 4, st->humidity > 99 ? 99 : st->humidity);
	strcpy(p, "%");
	y += 12;
	text_c(y, buf, 1);

	/* filter warning */
	y += 12;
	if (st->filter_life == 0) {
		text_c(y, "REPLACE FILTER", 1);
	} else if (st->filter_life <= 10) {
		p = utoa10(buf + 7, st->filter_life);
		memcpy(buf, "FILTER ", 7);
		strcpy(p, "%");
		text_c(y, buf, 1);
	}
}

static void two_lines(const char *a, const char *b, int scale)
{
	int lh = 7 * scale, gap = 4 * scale;
	int y = (fbh - (b ? 2 * lh + gap : lh)) / 2;
	text_c(y, a, scale);
	if (b)
		text_c(y + lh + gap, b, scale);
}

void ui_render(uint8_t screen, const ui_view *v)
{
	char buf[16];

	fb = hal_display_fb();
	hal_display_size(&fbw, &fbh);
	stride = (uint16_t)((fbw + 7) / 8);
	memset(fb, 0, (size_t)stride * fbh);

	switch (screen) {
	case SCR_MAIN:
		draw_main(v);
		break;
	case SCR_MODE:
		if (v->st->mode == MODE_FAVORITE &&
		    v->st->favorite_level < FAVORITE_LEVELS) {
			memcpy(buf, "LEVEL ", 6);
			utoa10(buf + 6, v->st->favorite_level);
			two_lines("FAVORITE", buf, 2);
		} else {
			two_lines(mode_name(v->st->mode), NULL, 2);
		}
		break;
	case SCR_LOCK_ON:  two_lines("CHILD LOCK", "ON", 2); break;
	case SCR_LOCK_OFF: two_lines("CHILD LOCK", "OFF", 2); break;
	case SCR_RESET_OK: two_lines("FILTER", "RESET", 2); break;
	case SCR_RESET_REFUSED: two_lines("FILTER", "LOCKED", 2); break;
	case SCR_FILTER_LOW:
		if (v->st->filter_life == 0) {
			two_lines("REPLACE", "FILTER", 2);
		} else {
			strcpy(utoa10(buf, v->st->filter_life), "%");
			two_lines("FILTER", buf, 2);
		}
		break;
	case SCR_NO_FILTER: two_lines("NO", "FILTER", 2); break;
	case SCR_DOOR: two_lines("CLOSE", "DOOR", 2); break;
	case SCR_FAULT: two_lines("FAN", "ERROR", 2); break;
	default: break;
	}
}

/* ---------- per-tick outputs ---------- */

static uint32_t fb_checksum(void)
{
	uint32_t s = 2166136261u;
	for (size_t i = 0; i < (size_t)stride * fbh; i++)
		s = (s ^ fb[i]) * 16777619u;
	return s;
}

static void set_leds(uint8_t r, uint8_t g, uint8_t o)
{
	if (u.leds_known && r == u.led_r && g == u.led_g && o == u.led_o)
		return;
	u.leds_known = true;
	u.led_r = r; u.led_g = g; u.led_o = o;
	hal_leds(r, g, o);
}

void ui_tick(const ui_view *v, bool fault, uint8_t pwm, uint8_t contrast)
{
	uint32_t now = hal_millis();
	uint8_t scr = ui_current_screen(v->st, fault);
	bool lit = scr != SCR_OFF;

	buzzer_tick(now);
	if (scr != u.shown) {
		u.shown = scr;
		u.shown_t = now;
	}

	/* status LED: one colour by AQI zone (hardware map 6.3), dimmed
	 * by the light sensor; all off with the display off or in standby.
	 * While the filter popup is drawn stock lights red only (1f012580
	 * end: drawn screen 0x16 -> GPIO11/12 low, GPIO13 high). */
	if (lit && v->st->power && scr == SCR_FILTER_LOW) {
		set_leds(pwm, 0, 0);
	} else if (lit && v->st->power) {
		uint8_t z = v->aqi_state;
		set_leds(z >= 4 ? pwm : 0, z <= 1 ? pwm : 0,
			 (z == 2 || z == 3) ? pwm : 0);
	} else {
		set_leds(0, 0, 0);
	}

	if (lit != u.disp_on) {
		u.disp_on = lit;
		hal_display_power(lit);
		u.rendered = false;
		u.contrast = -1;
	}
	if (!lit)
		return;
	if (contrast != u.contrast) {
		u.contrast = contrast;
		hal_display_brightness(contrast);
	}
	/* re-render at most every 250 ms (or on a screen change), flush
	 * only when the pixels changed */
	if (u.rendered && scr == u.last_screen && now - u.render_t < 250)
		return;
	ui_render(scr, v);
	u.render_t = now;
	uint32_t sum = fb_checksum();
	if (!u.rendered || sum != u.fb_sum || scr != u.last_screen) {
		hal_display_flush();
		u.fb_sum = sum;
	}
	u.rendered = true;
	u.last_screen = scr;
}
