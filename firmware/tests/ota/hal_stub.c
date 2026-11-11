/* hal_stub.c - hal.h time/reboot stubs and a log sink for host tests. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "hal.h"
#include "sim.h"

uint32_t fake_ms = 1000;
int fake_reboots;
int test_verbose;

uint32_t hal_millis(void) { return fake_ms; }
void hal_reboot(void) { fake_reboots++; }

int ota_test_log(const char *fmt, ...)
{
    if (!test_verbose)
        return 0;
    va_list ap;
    va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}
