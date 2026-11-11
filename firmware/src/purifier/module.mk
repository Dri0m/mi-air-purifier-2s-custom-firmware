# src/purifier/module.mk - portable purifier logic (purifier.h, settings.h).
# Paths are relative to firmware/.  Uses only include/*.h and libc.
PURIFIER_SRCS := \
	src/purifier/purifier.c \
	src/purifier/logic.c \
	src/purifier/store.c \
	src/purifier/ui.c \
	src/purifier/hal_ext.c
SRCS += $(PURIFIER_SRCS)
