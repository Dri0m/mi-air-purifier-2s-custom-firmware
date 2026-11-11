# src/diag: diagnostics behind the HA diagnostic entities (include/diag.h).
# diag_reason.c is portable (also linked by tests/esphome/host.mk);
# diag_mw300.c is device-only (PMU reset cause, retention RAM, FreeRTOS).
DIAG_SRCS := src/diag/diag_reason.c
SRCS += $(DIAG_SRCS) src/diag/diag_mw300.c
