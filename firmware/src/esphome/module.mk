# src/esphome/module.mk - ESPHome native API server + mDNS (portable) and
# the Noise encryption (vendored crypto under crypto/).  Included by the
# device Makefile and by tests/esphome/host.mk; paths relative to firmware/.
# Needs -Iinclude only.
ESPHOME_SRCS := src/esphome/pb.c src/esphome/api_entities.c \
                src/esphome/api_server.c src/esphome/mdns.c \
                src/esphome/noise.c src/esphome/rng.c \
                src/esphome/crypto/sha256_mbedtls.c \
                src/esphome/crypto/monocypher/monocypher.c
# Device-only (WMSDK headers): api_platform_entropy() for the MW300.
ESPHOME_DEVICE_SRCS := src/esphome/rng_mw300.c
SRCS += $(ESPHOME_SRCS) $(ESPHOME_DEVICE_SRCS)
