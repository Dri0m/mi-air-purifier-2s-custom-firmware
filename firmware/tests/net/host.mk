# tests/net/host.mk - host unit tests of src/net_mw300/wifi_policy.c (the
# reconnect / rescue-network state machine; the SDK glue is not host-built).
# Run from firmware/:  make -f tests/net/host.mk test
CC ?= cc
BUILD := build/host/net
CFLAGS := -std=c99 -Wall -Wextra -Werror -O1 -g -Isrc/net_mw300 \
	-fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS := -fsanitize=address,undefined
BIN := $(BUILD)/test_wifi_policy

.PHONY: test clean
test: $(BIN)
	$(BIN)

$(BIN): src/net_mw300/wifi_policy.c tests/net/test_wifi_policy.c src/net_mw300/wifi_policy.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/net_mw300/wifi_policy.c tests/net/test_wifi_policy.c

clean:
	rm -rf $(BUILD)
