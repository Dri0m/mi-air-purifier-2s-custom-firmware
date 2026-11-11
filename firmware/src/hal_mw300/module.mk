# src/hal_mw300: device HAL (include/hal.h) for the 88MW300 on the MHCW02P.
# Uses WMSDK lowlevel drivers (+ mdev_gpio for the GPIO IRQ, arch_reboot,
# boot_reset_cause) and FreeRTOS; no SDK sources listed here (libsdk.a).
SRCS += src/hal_mw300/hal_core.c src/hal_mw300/hal_i2c.c \
	src/hal_mw300/hal_sht.c src/hal_mw300/hal_eeprom.c \
	src/hal_mw300/hal_pm.c src/hal_mw300/hal_fan.c src/hal_mw300/hal_io.c \
	src/hal_mw300/hal_display.c src/hal_mw300/hal_rfid.c \
	src/hal_mw300/hal_proto.c
