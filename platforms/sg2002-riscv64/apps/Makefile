CROSS_COMPILE ?= riscv64-linux-musl-
ifeq ($(origin CC),default)
CC := $(CROSS_COMPILE)gcc
endif

UAPI_DIR ?= ../../../include/uapi
PREFIX ?= /bin

CPPFLAGS += -I$(UAPI_DIR)
CFLAGS ?= -O2 -g -Wall -Wextra
LDFLAGS ?= -static

.PHONY: all clean install

all: uvcgrab

uvcgrab: uvcgrab.c $(UAPI_DIR)/avatar_uvc.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -o $@

install: uvcgrab
	install -D -m 0755 uvcgrab $(DESTDIR)$(PREFIX)/uvcgrab

clean:
	rm -f uvcgrab