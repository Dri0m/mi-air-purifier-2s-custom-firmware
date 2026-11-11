# tests/purifier/host.mk - host unit tests of src/purifier.
# Run from firmware/:  make -f tests/purifier/host.mk test
CC ?= cc
BUILD := build/host/purifier
CFLAGS := -std=c99 -Wall -Wextra -Werror -O1 -g -DSTORE_FORMAT_BLANK=1 \
	-Iinclude -Isrc/host -Isrc/purifier \
	-fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS := -fsanitize=address,undefined

SRCS :=
include src/purifier/module.mk
SRCS += src/host/hal_fake.c tests/purifier/test_purifier.c
OBJS := $(patsubst %.c,$(BUILD)/%.o,$(SRCS))
BIN := $(BUILD)/test_purifier

.PHONY: test clean
test: $(BIN)
	$(BIN)

$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/%.o: %.c $(wildcard include/*.h src/purifier/*.h src/host/hal_fake.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD)
