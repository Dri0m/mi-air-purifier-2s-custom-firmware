/* host_main.c - host test server: src/esphome + netio_posix + purifier_stub.
 *
 *   esphome_host [-n name] [-f friendly_name] [-p password] [-v version]
 *                [-k base64_encryption_key]
 *
 * Serves the ESPHome API on :6053 and mDNS.  Stdin accepts test commands
 * (one per line): "door 0|1", "pm N", "temp dC", "hum N", "pmfault 0|1",
 * "motorfault 0|1", "state", "quit", and for the log ring (R3):
 * "log TEXT" (one console line), "logpart TEXT" (no newline),
 * "flood N" (N numbered 64-byte lines in one go).
 * Before READY, a few "[boot] ..." lines go into the log ring (the history
 * a log subscriber must get replayed).
 * Diagnostics (U2, src/host/diag_stub.c): "upload 0|1" (OTA upload in
 * progress), "trial 0|1" (unconfirmed trial that can revert), "heap N",
 * "rstcause N" (PMU reset cause bits); Restart logs "STUB reboot 1 -> 0".
 * "state" prints the stub state as "STATE ..." on stdout. */
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "esphome_api.h"
#include "logring.h"
#include "hal.h"
#include "purifier.h"
#include "purifier_stub.h"
#include "diag_stub.h"

uint32_t hal_millis(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* Faults are not settable through purifier_stub.h: set the stub's state
 * (a non-const static behind purifier_get()) directly, as the real
 * purifier does on a fault: pm_fault also makes pm25 a placeholder. */
static void set_fault(bool pm, bool on)
{
	purifier_state *s = (purifier_state *)purifier_get();
	if (pm) {
		s->pm_fault = s->pm25_unknown = on;
		s->changed |= PF_PM_FAULT | PF_PM_LIVE;
	} else {
		s->motor_fault = on;
		s->changed |= PF_MOTOR_FAULT;
	}
}

static void command(char *line)
{
	char buf[256];
	int v = 0;
	char cmd[16] = "";
	if (sscanf(line, "%15s %d", cmd, &v) < 1)
		return;
	if (!strcmp(cmd, "log") || !strcmp(cmd, "logpart")) {
		const char *t = line + strlen(cmd);
		if (*t == ' ')
			t++;
		logring_puts(t);
		if (!strcmp(cmd, "log"))
			logring_puts("\r\n");
	} else if (!strcmp(cmd, "flood")) {
		for (int i = 0; i < v; i++) {
			snprintf(buf, sizeof(buf), "[test] flood line %06d %-38s\r\n", i, "x");
			logring_puts(buf);
		}
	} else if (!strcmp(cmd, "door"))
		purifier_stub_set_door(v != 0);
	else if (!strcmp(cmd, "pm"))
		purifier_stub_set_pm25((uint16_t)v);
	else if (!strcmp(cmd, "temp"))
		purifier_stub_set_temp((int16_t)v);
	else if (!strcmp(cmd, "hum"))
		purifier_stub_set_humidity((uint8_t)v);
	else if (!strcmp(cmd, "pmfault"))
		set_fault(true, v != 0);
	/* diagnostics (U2): fake OTA state, heap, reset cause */
	else if (!strcmp(cmd, "upload"))
		diag_stub.in_progress = (uint8_t)(v != 0);
	else if (!strcmp(cmd, "trial"))
		diag_stub.trial = diag_stub.can_revert = (uint8_t)(v != 0);
	else if (!strcmp(cmd, "heap"))
		diag_stub.heap = (uint32_t)v;
	else if (!strcmp(cmd, "rstcause"))
		diag_stub.pmu_cause = (uint32_t)v;
	else if (!strcmp(cmd, "motorfault"))
		set_fault(false, v != 0);
	else if (!strcmp(cmd, "quit"))
		exit(0);
	else if (!strcmp(cmd, "state")) {
		purifier_stub_format(buf, sizeof(buf));
		printf("%s\n", buf);
	} else
		printf("ERR unknown command\n");
	fflush(stdout);
}

int main(int argc, char **argv)
{
	api_config cfg = {
		.name = "air-purifier-2s-test",
		.friendly_name = "Air Purifier 2S (host test)",
		.password = "",
		.version = "0.1.0-host",
		.mac_str = NULL,                /* from netio_mac() */
		.encryption_key = "",
	};
	char line[256];
	size_t llen = 0;
	uint32_t last_tick, t_start;
	int opt, infd = 0;

	while ((opt = getopt(argc, argv, "n:f:p:v:k:")) != -1) {
		switch (opt) {
		case 'n': cfg.name = optarg; break;
		case 'f': cfg.friendly_name = optarg; break;
		case 'p': cfg.password = optarg; break;
		case 'v': cfg.version = optarg; break;
		case 'k': cfg.encryption_key = optarg; break;
		default:
			fprintf(stderr, "usage: %s [-n name] [-f friendly] [-p password] [-v ver] [-k key]\n",
				argv[0]);
			return 2;
		}
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	logring_puts("\r\n=== host test boot ===\r\n");
	logring_puts("[boot] main reached\r\n");
	logring_puts("[net] wlan_connect failed -5 (will retry)\r\n");
	logring_puts("[app] servers started\r\n");
	purifier_init();
	diag_stub.version = cfg.version;
	t_start = hal_millis();
	switch (api_server_init(&cfg)) {
	case 0:
		break;
	case -1:
		fprintf(stderr, "api_server_init: listen failed (will retry)\n");
		break;
	default:
		fprintf(stderr, "api_server_init: invalid encryption key / no entropy\n");
		return 1;
	}
	mdns_init(&cfg);
	printf("READY\n");
	fflush(stdout);
	fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
	last_tick = hal_millis();

	for (;;) {
		struct pollfd p = { .fd = infd, .events = POLLIN };
		if (poll(&p, 1, 5) > 0 && (p.revents & (POLLIN | POLLHUP))) {
			char c;
			ssize_t n;
			while ((n = read(infd, &c, 1)) == 1) {
				if (c == '\n') {
					line[llen] = 0;
					command(line);
					llen = 0;
				} else if (llen < sizeof(line) - 1) {
					line[llen++] = c;
				}
			}
			if (n == 0) {           /* stdin closed: keep serving */
				close(infd);
				infd = -1;
			}
		}
		if (hal_millis() - last_tick >= 50) {
			last_tick += 50;
			purifier_tick();
		}
		diag_stub.uptime_s = (hal_millis() - t_start) / 1000u;
		api_server_poll();
		mdns_poll();
	}
}
