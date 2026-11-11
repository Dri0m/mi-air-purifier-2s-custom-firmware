# tests/ota/host.mk - host (macOS/Linux) tests for src/ota.
# Run from firmware/:   make -f tests/ota/host.mk test-ota
# or include it from firmware/host.mk (adds test-ota to HOST_TESTS).
#
#   test-ota        C unit/integration tests (simulated 2 MiB flash) + wmpt.py checks
#   test-ota-e2e    real HTTP server on 127.0.0.1 driven by tools/ota_upload.py
#   ota-arm-check   compile-only check of src/ota with arm-none-eabi-gcc + SDK headers

OTA_T_DIR   ?= tests/ota
OTA_SRC_DIR ?= src/ota
OTA_BUILD   ?= build/host/ota
# Two valid MRVL images used as test payloads (appfwA.raw, appfwB.raw).  By
# default they are packed from the device build (run `make` first) with two
# different header timestamps; the original development used the stock images.
OTA_IMAGES  ?= $(OTA_BUILD)/images
HOST_CC     ?= cc
PYTHON      ?= python3

OTA_CFLAGS = -std=c99 -O2 -g -Wall -Wextra -Werror -Iinclude -I$(OTA_SRC_DIR) -I$(OTA_T_DIR) \
	-DOTA_LOG_FN=ota_test_log -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE

OTA_CORE = $(OTA_SRC_DIR)/ota.c $(OTA_SRC_DIR)/ota_server.c $(OTA_SRC_DIR)/wmpt.c \
	$(OTA_SRC_DIR)/ota_image.c $(OTA_SRC_DIR)/ota_crc.c \
	$(OTA_T_DIR)/flash_sim.c $(OTA_T_DIR)/hal_stub.c
OTA_HDRS = $(wildcard $(OTA_SRC_DIR)/*.h) $(OTA_T_DIR)/sim.h include/ota.h include/netio.h include/hal.h

HOST_TESTS += test-ota

.PHONY: test-ota test-ota-e2e ota-arm-check

$(OTA_BUILD)/test_ota: $(OTA_CORE) $(OTA_T_DIR)/netio_fake.c $(OTA_T_DIR)/test_ota.c $(OTA_HDRS)
	@mkdir -p $(OTA_BUILD)/out
	$(HOST_CC) $(OTA_CFLAGS) -o $@ $(OTA_CORE) $(OTA_T_DIR)/netio_fake.c $(OTA_T_DIR)/test_ota.c

# The e2e server runs on real time: confirm timings scaled down (1 s = 1 min
# roughly) so the trial/confirm/revert scenarios finish in seconds.
OTA_E2E_TIMING = -DOTA_CONFIRM_MIN_UP_MS=2000u -DOTA_SELF_CONFIRM_MS=4000u \
	-DOTA_SELF_CONFIRM_API_MS=1500u -DOTA_CONFIRM_TIMEOUT_MS=9000u

$(OTA_BUILD)/ota_host_server: $(OTA_CORE) $(OTA_T_DIR)/netio_posix.c $(OTA_T_DIR)/ota_host_server.c $(OTA_HDRS) $(OTA_T_DIR)/host.mk
	@mkdir -p $(OTA_BUILD)
	$(HOST_CC) $(OTA_CFLAGS) $(OTA_E2E_TIMING) -o $@ $(OTA_CORE) $(OTA_T_DIR)/netio_posix.c $(OTA_T_DIR)/ota_host_server.c

$(OTA_BUILD)/images/appfwA.raw: build/purifier.elf tools/mrvl_pack.py
	@mkdir -p $(dir $@)
	$(PYTHON) tools/mrvl_pack.py pack build/purifier.elf $(OTA_BUILD)/images/appfwA.raw --timestamp 1700000000 >/dev/null
	$(PYTHON) tools/mrvl_pack.py pack build/purifier.elf $(OTA_BUILD)/images/appfwB.raw --timestamp 1600000000 >/dev/null

build/purifier.elf:
	@echo "error: build the device image first (make), the OTA tests use it as payload" >&2; false

ota-images: $(if $(filter $(OTA_BUILD)/images,$(OTA_IMAGES)),$(OTA_BUILD)/images/appfwA.raw)

test-ota: $(OTA_BUILD)/test_ota ota-images
	@rm -f $(OTA_BUILD)/out/*.bin
	$(OTA_BUILD)/test_ota $(OTA_IMAGES) $(OTA_BUILD)/out
	$(PYTHON) tools/wmpt.py selftest
	@# Cross-check: wmpt.py's generator == the table the C test built.
	$(PYTHON) tools/wmpt.py gen $(OTA_BUILD)/stock_table.bin --gen 2 --gen-a 3 --gen-b 2 >/dev/null
	$(PYTHON) -c "import sys; a=open('$(OTA_BUILD)/stock_table.bin','rb').read(); \
	b=open('$(OTA_BUILD)/out/base.bin','rb').read()[0x4000:0x4000+len(a)]; \
	sys.exit(0 if a==b else 'wmpt.py gen != C reference table')"
	@# Every flash state the tests saved must validate with the Python tool.
	@set -e; for f in $(OTA_BUILD)/out/*.bin; do \
		printf '%-40s ' "$$(basename $$f)"; $(PYTHON) tools/wmpt.py validate $$f; done
	@echo "test-ota: PASS"

test-ota-e2e: $(OTA_BUILD)/ota_host_server ota-images
	$(PYTHON) $(OTA_T_DIR)/test_upload.py --server $(OTA_BUILD)/ota_host_server \
		--images $(OTA_IMAGES) --work $(OTA_BUILD)/e2e

# ---- compile-only check for the device (no link; the platform Makefile links)
ARM_GCC ?= arm-none-eabi-gcc
WMSDK   ?= $(HOME)/sdk/amazon-freertos/vendors/marvell/WMSDK/mw320
OTA_ARM_INC = -I$(WMSDK)/sdk/src/incl/sdk/drivers/mw300/regs -I$(WMSDK)/sdk/src/incl/sdk/drivers/mw300 \
	-I$(WMSDK)/sdk/src/incl -I$(WMSDK)/sdk/src/incl/sdk -I$(WMSDK)/sdk/src/incl/sdk/drivers \
	-I$(WMSDK)/sdk/src/incl/libc -I$(WMSDK)/sdk/src/incl/autoconf \
	-I$(WMSDK)/sdk/src/incl/platform/os/freertos
OTA_ARM_FLAGS = -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -D__FPU_PRESENT=1 \
	-DCONFIG_CPU_MW300 -Os -std=gnu99 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
	-Iinclude -I$(OTA_SRC_DIR) $(OTA_ARM_INC) -DOTA_LOG_FN=wmprintf -DOTA_REQUIRE_RUNNING_SLOT
OTA_ARM_SRCS = ota.c ota_server.c wmpt.c ota_image.c ota_crc.c ota_flash_mw300.c

ota-arm-check:
	@mkdir -p build/arm-check/ota
	@set -e; for f in $(OTA_ARM_SRCS); do \
		echo "  ARM CC $(OTA_SRC_DIR)/$$f"; \
		$(ARM_GCC) $(OTA_ARM_FLAGS) -c $(OTA_SRC_DIR)/$$f -o build/arm-check/ota/$${f%.c}.o; done
	@arm-none-eabi-size build/arm-check/ota/*.o
