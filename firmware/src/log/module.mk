# src/log: console log ring + retained last-boot record (R3 remote logs).
# Portable (include/logring.h); the device build picks it up via the
# Makefile's src/*/module.mk include, host tests list it themselves.
SRCS += src/log/logring.c
