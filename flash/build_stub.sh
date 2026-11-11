#!/bin/sh
# Rebuild the two 88MW300 UART-boot RAM programs into build/ and compare them
# with the shipped binaries.
#
#   mw300_stub_110000.bin     read-only QSPI flash dumper (uartboot_load.py)
#   mw300_flashwr_110000.bin  guarded flash writer        (uartboot_flash.py)
#
# The shipped .bin files next to this script are the exact ones that ran on
# real hardware, and the tools use THEM by default.  Their bit-banged serial
# output is timed by a delay loop, so a different compiler can shift the
# timing.  Use a rebuilt binary (--stub build/...) only if you know why.
#
# Needs an arm-none-eabi GCC (the shipped ones were built with 10.3.1).
set -eu
cd "$(dirname "$0")"

if [ -n "${TC:-}" ] && [ -x "$TC/arm-none-eabi-gcc" ]; then
	:
elif command -v arm-none-eabi-gcc >/dev/null 2>&1; then
	TC=$(dirname "$(command -v arm-none-eabi-gcc)")
elif [ -x "$HOME/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-gcc" ]; then
	TC="$HOME/.platformio/packages/toolchain-gccarmnoneeabi/bin"
else
	echo "no arm-none-eabi-gcc found; set TC=/path/to/toolchain/bin" >&2
	exit 1
fi

CC="$TC/arm-none-eabi-gcc"
LD="$TC/arm-none-eabi-ld"
OBJCOPY="$TC/arm-none-eabi-objcopy"
B=build
mkdir -p $B

CFLAGS="-mcpu=cortex-m4 -mthumb -mfloat-abi=soft -Os -ffreestanding -fno-builtin -ffunction-sections -fdata-sections -Wall -Wextra"

"$CC" $CFLAGS -c startup.S -o $B/startup.o
"$CC" $CFLAGS -DDUMP_LEN_ADDR=0x00112000u -c mw300_stub.c -o $B/mw300_stub_110000.o
"$CC" $CFLAGS -c flashwr_core.c -o $B/flashwr_core.o
"$CC" $CFLAGS -c mw300_flashwr.c -o $B/mw300_flashwr.o

"$LD" -T mw300_stub_110000.ld --gc-sections -Map=$B/mw300_stub_110000.map \
	-o $B/mw300_stub_110000.elf $B/startup.o $B/mw300_stub_110000.o
"$OBJCOPY" -O binary $B/mw300_stub_110000.elf $B/mw300_stub_110000.bin

# Guarded flash writer: core + MW300 port, linked below its parameter block
# at +0x0FC0 (payload at +0x1000).  With the default 4 KB chunk the whole ROM
# load is 0x2000 bytes, the size proven on hardware.
"$LD" -T mw300_flashwr_110000.ld --gc-sections -Map=$B/mw300_flashwr_110000.map \
	-o $B/mw300_flashwr_110000.elf $B/startup.o $B/mw300_flashwr.o $B/flashwr_core.o
"$OBJCOPY" -O binary $B/mw300_flashwr_110000.elf $B/mw300_flashwr_110000.bin
[ "$(wc -c < $B/mw300_flashwr_110000.bin)" -le 4032 ] || { echo "flash writer exceeds its 0x0FC0 code budget" >&2; exit 1; }

for f in mw300_stub_110000.bin mw300_flashwr_110000.bin; do
	if cmp -s "$f" "$B/$f"; then r="identical to the shipped binary"; else r="DIFFERS from the shipped binary"; fi
	printf '%-26s %s bytes, %s\n' "$B/$f" "$(wc -c < "$B/$f" | tr -d ' ')" "$r"
done

# Host test of the real guard core against a simulated W25Q16 (needs a host
# C compiler; skipped if none).  Python offline tests: test_flash.py,
# test_protocol.py.
if command -v cc >/dev/null 2>&1; then
	echo "building + running host core test (flashwr_core.c vs simulated W25Q16)"
	cc -I. -Wall -Wextra -DHOST_TEST flashwr_core.c w25q_model.c test_flashwr.c \
		-o $B/flashwr_hosttest && $B/flashwr_hosttest || {
		echo "host core test FAILED" >&2; exit 1; }
fi
