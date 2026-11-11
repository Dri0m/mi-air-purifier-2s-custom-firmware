# src/ota/module.mk - included by the device Makefile.
# Appends the OTA sources and include path.  Paths are relative to firmware/.
#
# Needs from the platform build: SDK include paths (flash.h, lowlevel_drivers.h),
# the SDK flash stack compiled in (core/util/flash.c, drivers/mw300/mdev_iflash.c,
# lowlevel mw300_flash.c / mw300_flashc.c / mw300_qspi.c, mdev, wm_os) and
# hal_millis()/hal_reboot() from src/hal_mw300.
#
# Tunables (add to CFLAGS_EXTRA before/after including):
#   -DOTA_CONFIRM_TIMEOUT_MS=600000   boot-confirm window (default 10 min)
#   -DOTA_CONFIRM_MIN_UP_MS=150000     earliest POST /confirm (default 150 s)
#   -DOTA_SELF_CONFIRM_MS=300000      fallback self-confirm uptime (default 5 min)
#   -DOTA_SELF_CONFIRM_API_MS=60000   ... with an API session this long (60 s)
#   -DOTA_BOOT_CONFIRM=0              disable boot confirm (commit as confirmed)
#   -DFW_VERSION=\"x.y\"              shown by GET /version
#   -DOTA_REQUIRE_RUNNING_SLOT        refuse OTA if the XIP offset is unreadable

OTA_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))

SRCS += \
	$(OTA_DIR)/ota.c \
	$(OTA_DIR)/ota_server.c \
	$(OTA_DIR)/wmpt.c \
	$(OTA_DIR)/ota_image.c \
	$(OTA_DIR)/ota_crc.c \
	$(OTA_DIR)/ota_flash_mw300.c

override CFLAGS_EXTRA += -I$(OTA_DIR) -DOTA_LOG_FN=platform_logf -DOTA_REQUIRE_RUNNING_SLOT
