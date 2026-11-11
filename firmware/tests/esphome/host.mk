# tests/esphome/host.mk - host build + tests of the ESPHome API server.
# Run from firmware/:
#   make -f tests/esphome/host.mk                 build host server + unit test
#   make -f tests/esphome/host.mk test PYTHON=... unit test + aioesphomeapi test
#   make -f tests/esphome/host.mk test-mdns PYTHON=...   zeroconf test
#   make -f tests/esphome/host.mk arm             cross-compile check + sizes
# PYTHON must have aioesphomeapi (and zeroconf for test-mdns) installed.
CC      ?= cc
PYTHON  ?= python3
BUILD   := build/host/esphome
CFLAGS  := -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -O1 -g -Wall -Wextra \
           -Werror -Iinclude -Isrc/esphome -Isrc/host -fsanitize=address,undefined
SRCS    :=
include src/esphome/module.mk
# diagnostics (api_entities.c): fake diag.h / ota_get_info / hal_reboot
DIAG_HOST_SRCS := src/diag/diag_reason.c src/host/diag_stub.c
# log ring (R3): console log buffer streamed by api_server.c
LOG_SRCS := src/log/logring.c
HOST_SRCS := $(ESPHOME_SRCS) $(LOG_SRCS) src/host/netio_posix.c src/host/purifier_stub.c \
             $(DIAG_HOST_SRCS) src/host/rng_posix.c tests/esphome/host_main.c

ARM_GCC  ?= arm-none-eabi-gcc
ARM_SIZE ?= arm-none-eabi-size
ARM_FLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -Os \
             -Wall -Wextra -Iinclude -ffunction-sections -fdata-sections

all: $(BUILD)/esphome_host $(BUILD)/test_pb $(BUILD)/test_server $(BUILD)/test_crypto \
     $(BUILD)/test_logring $(BUILD)/test_logs

$(BUILD)/esphome_host: $(HOST_SRCS) $(wildcard src/esphome/*.h include/*.h) src/host/purifier_stub.h src/host/diag_stub.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(HOST_SRCS)

$(BUILD)/test_pb: tests/esphome/test_pb.c src/esphome/pb.c src/esphome/pb.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/esphome/test_pb.c src/esphome/pb.c

# api_server.c against a scripted in-memory netio (deterministic)
$(BUILD)/test_server: tests/esphome/test_server.c $(ESPHOME_SRCS) $(LOG_SRCS) src/host/purifier_stub.c $(DIAG_HOST_SRCS) $(wildcard src/esphome/*.h include/*.h) src/host/diag_stub.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/esphome/test_server.c $(ESPHOME_SRCS) $(LOG_SRCS) src/host/purifier_stub.c $(DIAG_HOST_SRCS)

# log ring unit test (wrap, lag/drop, line splitting, threads, retention)
$(BUILD)/test_logring: tests/esphome/test_logring.c $(LOG_SRCS) include/logring.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -pthread -o $@ tests/esphome/test_logring.c $(LOG_SRCS)

# api_server log streaming (replay, live, levels, slow client), mock netio
$(BUILD)/test_logs: tests/esphome/test_logs.c $(ESPHOME_SRCS) $(LOG_SRCS) $(DIAG_HOST_SRCS) src/host/purifier_stub.c $(wildcard src/esphome/*.h include/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/esphome/test_logs.c $(ESPHOME_SRCS) $(LOG_SRCS) $(DIAG_HOST_SRCS) src/host/purifier_stub.c

# crypto test vectors (RFC 7748, RFC 8439, RFC 4231, NIST, Noise NNpsk0)
$(BUILD)/test_crypto: tests/esphome/test_crypto.c src/esphome/noise.c src/esphome/rng.c src/esphome/crypto/sha256_mbedtls.c src/esphome/crypto/monocypher/monocypher.c $(wildcard src/esphome/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

test: all
	$(BUILD)/test_pb
	$(BUILD)/test_crypto
	$(BUILD)/test_server 2>/dev/null
	$(BUILD)/test_logring
	$(BUILD)/test_logs 2>/dev/null
	$(PYTHON) tests/esphome/test_api.py --binary $(BUILD)/esphome_host
	$(PYTHON) tests/esphome/test_logs.py --binary $(BUILD)/esphome_host

test-mdns: all
	$(PYTHON) tests/esphome/test_mdns.py --binary $(BUILD)/esphome_host

arm:
	@mkdir -p $(BUILD)/arm
	@for f in $(ESPHOME_SRCS) $(LOG_SRCS); do \
	  echo "ARM CC $$f"; \
	  $(ARM_GCC) $(ARM_FLAGS) -c $$f -o $(BUILD)/arm/$$(basename $$f .c).o || exit 1; \
	done
	$(ARM_SIZE) -t $(BUILD)/arm/*.o

clean:
	rm -f $(BUILD)/esphome_host $(BUILD)/test_pb $(BUILD)/test_server $(BUILD)/test_crypto \
	      $(BUILD)/test_logring $(BUILD)/test_logs $(BUILD)/arm/*.o

.PHONY: all test test-mdns arm clean
