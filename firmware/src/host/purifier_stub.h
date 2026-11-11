/* purifier_stub.h - host-only test hooks of purifier_stub.c (a fake
 * purifier.h implementation used by tests/esphome).  Not for the device. */
#ifndef PURIFIER_STUB_H
#define PURIFIER_STUB_H
#include <stdbool.h>
#include <stdint.h>

/* Simulate changes coming from the hardware side. */
void purifier_stub_set_door(bool open);
void purifier_stub_set_pm25(uint16_t pm);
void purifier_stub_set_temp(int16_t temp_dC);
void purifier_stub_set_humidity(uint8_t rh);
/* One-line text dump of the state ("STATE power=1 mode=0 ..."). */
void purifier_stub_format(char *buf, unsigned size);
#endif
