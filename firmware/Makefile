# Device build for the Mi Air Purifier 2S custom firmware (Marvell 88MW300).
#
#   make            -> build/purifier.elf, build/purifier.mrvl (+ .map, .bin)
#   make check      -> static checks: image layout/CRCs, packer vs stock image
#   make size       -> section sizes
#   make clean
#
# Needs (see ../README.md):
#   - an arm-none-eabi GCC in PATH (tested: 10.3.1), or TOOLCHAIN=/path/to/bin
#   - the Amazon FreeRTOS tree with the Marvell WMSDK, AFR=/path (default
#     ~/sdk/amazon-freertos), commit 41244b00, with the freertos_kernel and
#     freertos_plus_tcp submodules
#   - secrets.h (cp secrets.example.h secrets.h, then edit)
#
# The SDK is NOT built with its own scripts.  We compile the needed WMSDK /
# FreeRTOS / FreeRTOS+TCP sources ourselves into build/libsdk.a and link the
# prebuilt WMSDK libs (Wi-Fi driver, WLAN manager, partition, PSM, rfget).
#
# Module integration contract
#   Each src/<module>/module.mk may append to:
#     SRCS          C sources, paths relative to firmware/
#     CFLAGS_EXTRA  extra compiler flags (applied to all our sources)
#   Every module source is compiled with $(CFLAGS), which already contains
#   $(SDK_INCLUDES) and $(SDK_DEFINES): SDK headers are available as e.g.
#   <mdev_gpio.h>, <mdev_gpt.h>, <mdev_i2c.h>, <mdev_uart.h>, <mdev_ssp.h>,
#   <mdev_adc.h>, <lowlevel_drivers.h>, <wm_os.h>, <partition.h>,
#   <firmware_structure.h>, <flash.h>, "FreeRTOS.h", "FreeRTOS_Sockets.h".
#   Do NOT list SDK sources in module.mk: every WMSDK mw300 driver
#   (lowlevel + mdev) is already in libsdk.a and the linker pulls in what
#   is referenced.

# TOOLCHAIN = directory holding arm-none-eabi-gcc; empty = take it from PATH.
TOOLCHAIN ?=
CROSS     := $(if $(TOOLCHAIN),$(TOOLCHAIN)/)arm-none-eabi-
CC        := $(CROSS)gcc
AR        := $(CROSS)ar
OBJCOPY   := $(CROSS)objcopy
SIZE      := $(CROSS)size
NM        := $(CROSS)nm
PYTHON    ?= python3

AFR       ?= $(HOME)/sdk/amazon-freertos
WMSDK     := $(AFR)/vendors/marvell/WMSDK/mw320
SDKSRC    := $(WMSDK)/sdk/src
KERNEL    := $(AFR)/freertos_kernel
TCPDIR    := $(AFR)/libraries/freertos_plus/standard/freertos_plus_tcp
SDKLIBDIR := $(WMSDK)/bin/libs

BUILD     := build
TARGET    := $(BUILD)/purifier

# --- CPU / ABI ---------------------------------------------------------------
# softfp, not hard: the prebuilt WMSDK libs (libwifidriver.a etc.) are built
# for the base AAPCS (no Tag_ABI_VFP_args); ld refuses to mix them with
# -mfloat-abi=hard objects.  softfp still uses the FPU for arithmetic.
ARCH_FLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=softfp -mfpu=fpv4-sp-d16

# --- SDK headers / config ----------------------------------------------------
# autoconf.h is the SDK's own Kconfig output; the prebuilt libs were built
# against it, so it is used unchanged.  platform/ comes first so our
# FreeRTOSConfig.h / FreeRTOSIPConfig.h win.
SDK_INCLUDES := \
	-Iplatform \
	-I$(SDKSRC) \
	-I$(SDKSRC)/incl \
	-I$(SDKSRC)/incl/libc/arm_gcc \
	-I$(SDKSRC)/incl/sdk \
	-I$(SDKSRC)/incl/sdk/drivers \
	-I$(SDKSRC)/incl/sdk/drivers/wlan \
	-I$(SDKSRC)/incl/sdk/drivers/mw300 \
	-I$(SDKSRC)/incl/sdk/drivers/mw300/regs \
	-I$(SDKSRC)/drivers/common/hardfault \
	-I$(SDKSRC)/boards \
	-I$(SDKSRC)/incl/platform/os/freertos \
	-I$(SDKSRC)/incl/platform/net/freertos-plus-tcp \
	-I$(KERNEL)/include \
	-I$(KERNEL)/portable/GCC/ARM_CM4F \
	-I$(TCPDIR)/include \
	-I$(TCPDIR)/portable/Compiler/GCC

SDK_DEFINES := -include $(SDKSRC)/incl/autoconf.h \
	-D__FPU_PRESENT=1 -DMW320 '-DSDK_VERSION="v1.2.r6.p1"'

COMMON_CFLAGS := $(ARCH_FLAGS) -Os -g3 -ffunction-sections -fdata-sections \
	-ffreestanding -fno-common -fno-strict-aliasing -MMD -MP \
	$(SDK_DEFINES) $(SDK_INCLUDES)

# Vendor code: compiled as the SDK does (c99 + gnu asm), warnings off.
SDK_CFLAGS := $(COMMON_CFLAGS) -std=gnu99 -w

# --- our sources (modules append via module.mk) -----------------------------
SRCS :=
CFLAGS_EXTRA :=
-include src/*/module.mk

PLATFORM_SRCS := \
	platform/board.c \
	platform/platform.c \
	platform/hooks.c

# Startup (vector table + reset handler) is linked as a plain object, not
# from the archive, so it is always present.
STARTUP_SRC := $(SDKSRC)/drivers/mw300/mdev_startup.c

# Firmware version (U1): `git describe --always --dirty --tags`, "unknown"
# without git; override with make FW_VERSION=...  Only [A-Za-z0-9._+-],
# at most 40 chars (it goes into JSON and the console banner unescaped).
# Written to $(BUILD)/gen/fw_version.h only when it changes and
# force-included into every app object, which all depend on it: a new
# commit or a dirty tree rebuilds them, so no object keeps a stale version.
FW_VERSION ?= $(shell git describe --always --dirty --tags 2>/dev/null)
override FW_VERSION := $(shell printf '%s' '$(subst ',,$(FW_VERSION))' | tr -cd 'A-Za-z0-9._+-' | cut -c1-40)
override FW_VERSION := $(if $(FW_VERSION),$(FW_VERSION),unknown)
FW_VERSION_H := $(BUILD)/gen/fw_version.h
FW_VERSION_LINE := \#define FW_VERSION "$(FW_VERSION)"
$(shell mkdir -p $(BUILD)/gen && { echo '$(FW_VERSION_LINE)' | cmp -s - $(FW_VERSION_H) || echo '$(FW_VERSION_LINE)' > $(FW_VERSION_H); })

# Wi-Fi regulatory domain (src/net_mw300/wifi_mw300.c): COUNTRY_EU (default,
# channels 1-13), COUNTRY_US (1-11), COUNTRY_CN, COUNTRY_JP, ... as in the
# SDK's wlan.h.  make NET_WIFI_COUNTRY=COUNTRY_US
NET_WIFI_COUNTRY ?= COUNTRY_EU

CFLAGS := $(COMMON_CFLAGS) -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
	-Wno-missing-field-initializers -Iinclude -I. -include $(FW_VERSION_H) \
	-DNET_WIFI_COUNTRY=$(NET_WIFI_COUNTRY) $(CFLAGS_EXTRA)

# Radio firmware: by default embed the SDK's matching W14.88.36.p135 image
# (the prebuilt driver targets it; stock wififw is p84).  WIFI_FW_EMBED=0
# loads the stock wififw partition instead.
WIFI_FW_EMBED ?= 1
WIFI_FW_XZ    ?= $(WMSDK)/wifi-firmware/mw30x/mw30x_uapsta_W14.88.36.p135.bin.xz
ifeq ($(WIFI_FW_EMBED),1)
GEN_SRCS := $(BUILD)/gen/wlfw_blob.c
CFLAGS += -DWIFI_FW_EMBED=1
endif

APP_SRCS := $(PLATFORM_SRCS) $(SRCS) $(GEN_SRCS)
APP_OBJS := $(patsubst %.c,$(BUILD)/obj/%.o,$(APP_SRCS))
STARTUP_OBJ := $(BUILD)/sdk/mdev_startup.o

# --- SDK sources -> build/libsdk.a -------------------------------------------
SDK_DRV_SRCS := \
	$(wildcard $(SDKSRC)/drivers/mw300/lowlevel/*.c) \
	$(filter-out %/mdev_startup.c,$(wildcard $(SDKSRC)/drivers/mw300/*.c)) \
	$(addprefix $(SDKSRC)/drivers/common/, cyccnt.c mdev_acomp.c \
		mdev_crc.c mdev_dac.c mdev_gpio.c mdev_gpt.c mdev_wdt.c \
		hardfault/hardfault.c)

SDK_CORE_SRCS := \
	$(SDKSRC)/core/mdev/mdev.c \
	$(addprefix $(SDKSRC)/core/util/, boot_flags.c wm_utils.c flash.c hexbin.c \
		dumphex.c panic.c crc/soft_crc16.c crc/soft_crc32.c crc/crc32.c \
		wm_wlan.c critical_error.c keystore.c) \
	$(addprefix $(SDKSRC)/core/wmstdio/, wmstdio.c console.c wmassert.c) \
	$(addprefix $(SDKSRC)/core/wmtime/, wmtime.c wmtime_cli.c hwrtc.c) \
	$(addprefix $(SDKSRC)/core/cli/, cli.c cli_utils.c cli_mem_simple.c) \
	$(addprefix $(SDKSRC)/core/pwrmgr/, pwrmgr.c pwrmgr_cli.c) \
	$(wildcard $(SDKSRC)/middleware/xz/*.c) \
	$(SDKSRC)/platform/os/freertos/os.c \
	$(SDKSRC)/platform/os/freertos/os_cli.c \
	$(SDKSRC)/platform/net/freertos-plus-tcp/net.c

# SDK mini libc (printf family without float, str*, ...); newlib fills the
# rest (memcpy/memset, ...).  atox/strtox are excluded as in the AFR build.
SDK_LIBC_SRCS := $(filter-out %/atox.c %/strtox.c,$(wildcard $(SDKSRC)/libc/src/*.c))
SDK_LIBC_OBJS = $(addprefix $(BUILD)/sdk/,$(notdir $(SDK_LIBC_SRCS:.c=.o)))

KERNEL_SRCS := \
	$(addprefix $(KERNEL)/, list.c queue.c tasks.c event_groups.c timers.c \
		stream_buffer.c portable/GCC/ARM_CM4F/port.c) \
	$(WMSDK)/sdk/external/FreeRTOS/portable/MemMang/heap_marvell.c

TCP_SRCS := \
	$(addprefix $(TCPDIR)/, FreeRTOS_ARP.c FreeRTOS_DHCP.c FreeRTOS_DNS.c \
		FreeRTOS_IP.c FreeRTOS_Sockets.c FreeRTOS_Stream_Buffer.c \
		FreeRTOS_TCP_IP.c FreeRTOS_TCP_WIN.c FreeRTOS_UDP_IP.c \
		portable/BufferManagement/BufferAllocation_2.c)

LIBSDK_SRCS := $(SDK_DRV_SRCS) $(SDK_CORE_SRCS) $(SDK_LIBC_SRCS) $(KERNEL_SRCS) $(TCP_SRCS)
# Object names keep the source basename: the linker script places
# mw300_flash.o / mw300_qspi.o / mw300_pmu.o / mdev_pm.o / mw300_clock.o in
# RAM by member name.
LIBSDK_OBJS := $(addprefix $(BUILD)/sdk/,$(notdir $(LIBSDK_SRCS:.c=.o)))
LIBSDK := $(BUILD)/libsdk.a

ifneq ($(words $(LIBSDK_OBJS)),$(words $(sort $(LIBSDK_OBJS))))
$(error duplicate SDK object basenames: rename handling needed)
endif

vpath %.c $(sort $(dir $(LIBSDK_SRCS) $(STARTUP_SRC)))

PREBUILT_LIBS := $(addprefix $(SDKLIBDIR)/, libwlcmgr.a libwifidriver.a \
	librfget.a libpsm.a libpart.a)

LDSCRIPT := platform/purifier.ld
LDFLAGS := $(ARCH_FLAGS) -nostartfiles -specs=nano.specs -specs=nosys.specs \
	-T $(LDSCRIPT) -Wl,--gc-sections -Wl,-Map=$(TARGET).map -Wl,--cref \
	-Wl,--wrap=malloc -Wl,--wrap=free -Wl,--wrap=calloc -Wl,--wrap=realloc \
	-Wl,--wrap=printf -Wl,--wrap=fprintf -Wl,--wrap=ll_printf -Wl,--wrap=_wm_assert

# --- rules -------------------------------------------------------------------
.PHONY: all clean size check sdk
all: $(TARGET).mrvl size

sdk: $(LIBSDK)

secrets.h:
	@echo "error: firmware/secrets.h is missing; cp secrets.example.h secrets.h and edit it" >&2
	@false

$(BUILD)/gen/wlfw_blob.c: $(WIFI_FW_XZ) tools/gen_wlfw.py
	@mkdir -p $(dir $@)
	@python3 -I tools/gen_wlfw.py $< $@

$(BUILD)/obj/%.o: %.c $(FW_VERSION_H) | secrets.h
	@mkdir -p $(dir $@)
	@echo "CC  $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Startup: a build-time copy of the SDK's mdev_startup.c with
# NVIC_SetPriorityGrouping(4) -> (3).  Grouping 4 leaves a sub-priority bit,
# and the FreeRTOS ARM_CM4F port asserts PRIGROUP <= 3 in
# xPortStartScheduler(), which on this chip runs BEFORE main().  That assert
# (plus a console-less assert hook) hung the first hardware boot silently.
# Grouping 3 = all 4 implemented bits are pre-emption bits, as FreeRTOS
# requires.  The SDK tree itself is never modified.
STARTUP_GEN := $(BUILD)/gen/mdev_startup.c
$(STARTUP_GEN): $(STARTUP_SRC) Makefile
	@mkdir -p $(dir $@)
	@sed 's/NVIC_SetPriorityGrouping(4);/NVIC_SetPriorityGrouping(3);/' $< > $@
	@grep -q 'NVIC_SetPriorityGrouping(3);' $@ || { echo "startup patch failed" >&2; rm -f $@; exit 1; }
	@! grep -q 'NVIC_SetPriorityGrouping(4)' $@

$(STARTUP_OBJ): $(STARTUP_GEN)
	@mkdir -p $(dir $@)
	@echo "CC  [sdk] mdev_startup.c (priority grouping 3)"
	@$(CC) $(SDK_CFLAGS) -c $< -o $@

$(BUILD)/sdk/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "CC  [sdk] $(notdir $<)"
	@$(CC) $(SDK_CFLAGS) -c $< -o $@

# The SDK libc headers (incl/libc) shadow the toolchain's; only the SDK libc
# itself is compiled against them (as in the SDK's libc/src/build.mk).
$(SDK_LIBC_OBJS): SDK_CFLAGS += -I$(SDKSRC)/incl/libc

$(LIBSDK): $(LIBSDK_OBJS)
	@rm -f $@
	@echo "AR  $@"
	@$(AR) rcs $@ $^

# Build time (U1): generated on every link into $(BUILD)/gen/build_info.c
# (fw_build_time, include/diag.h) and $(BUILD)/gen/build_epoch, which the
# packer uses as the MRVL header timestamp, so /version "build" and
# "image_time" name the same second.  SOURCE_DATE_EPOCH is honoured.
# Nothing depends on build_info.c, so it never triggers a rebuild itself.
BUILD_INFO_C := $(BUILD)/gen/build_info.c
BUILD_INFO_O := $(BUILD)/gen/build_info.o
BUILD_EPOCH  := $(BUILD)/gen/build_epoch

# Archive order matters: the prebuilt libs come before libsdk.a so their
# references (sprintf, snprintf, str*) resolve to the SDK libc, not newlib.
$(TARGET).elf: $(APP_OBJS) $(STARTUP_OBJ) $(LIBSDK) $(PREBUILT_LIBS) $(LDSCRIPT) tools/gen_build_info.py
	@$(PYTHON) -I tools/gen_build_info.py $(BUILD_INFO_C) $(BUILD_EPOCH)
	@$(CC) $(ARCH_FLAGS) -Os -c $(BUILD_INFO_C) -o $(BUILD_INFO_O)
	@echo "LD  $@ ($(FW_VERSION), built $$(cat $(BUILD_EPOCH)))"
	@$(CC) $(LDFLAGS) -o $@ $(STARTUP_OBJ) $(APP_OBJS) $(BUILD_INFO_O) \
		-Wl,--start-group $(PREBUILT_LIBS) $(LIBSDK) -lc -lgcc -lnosys -Wl,--end-group

$(TARGET).mrvl: $(TARGET).elf tools/mrvl_pack.py
	@echo "PACK $@"
	@$(PYTHON) tools/mrvl_pack.py pack $< $@ --timestamp $$(cat $(BUILD_EPOCH))
	@$(OBJCOPY) -O binary --only-section=.text $< $(TARGET).text.bin

size: $(TARGET).elf
	@$(SIZE) -A -x $< | grep -E '^\.(init|text|data|bss_sram0|bss_sram1|nvram)|section'
	@$(SIZE) $<

# Static verification (no hardware): the packer self-test, our image's
# layout and CRCs, and the boot-relevant symbols are present.
check: $(TARGET).mrvl
	@$(PYTHON) tools/mrvl_pack.py selftest
	@$(PYTHON) tools/mrvl_pack.py info $(TARGET).mrvl
	@$(PYTHON) tools/mrvl_pack.py check $(TARGET).elf $(TARGET).mrvl --map $(TARGET).map
	@$(PYTHON) tools/check_secrets.py secrets.h secrets.example.h

clean:
	rm -rf $(BUILD)

-include $(APP_OBJS:.o=.d) $(LIBSDK_OBJS:.o=.d) $(STARTUP_OBJ:.o=.d)
