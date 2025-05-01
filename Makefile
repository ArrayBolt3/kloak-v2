#!/usr/bin/make -f

CC ?=gcc
INSTALL ?= install
PKG_CONFIG ?= pkg-config

CFLAGS ?= -O2 -g

prefix ?= /usr
sbindir ?= $(prefix)/sbin
datadir ?= $(prefix)/share

TARGETARCH=$(shell $(CC) -dumpmachine)
CC_VERSION=$(shell $(CC) --version)

WARN_CFLAGS := -Wall -Wformat -Wformat=2 -Wconversion -Wimplicit-fallthrough \
	-Werror=format-security -Werror=implicit -Werror=int-conversion \
	-Werror=incompatible-pointer-types

ifeq (,$(findstring clang,$(CC_VERSION)))
WARN_CFLAGS += -Wtrampolines -Wbidi-chars=any
endif

FORTIFY_CFLAGS := -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -fstack-clash-protection \
	-fstack-protector-strong -fno-delete-null-pointer-checks \
	-fno-strict-overflow -fno-strict-aliasing -fsanitize=undefined

ifeq (yes,$(patsubst x86_64%-linux-gnu,yes,$(TARGETARCH)))
FORTIFY_CFLAGS += -fcf-protection=full
endif
ifeq (yes,$(patsubst aarch64%-linux-gnu,yes,$(TARGETARCH)))
FORTIFY_CFLAGS += -mbranch-protection=standard
endif

BIN_CFLAGS := -fPIE

CFLAGS := $(WARN_CFLAGS) $(FORTIFY_CFLAGS) $(BIN_CFLAGS) $(CFLAGS)
LDFLAGS := -Wl,-z,nodlopen -Wl,-z,noexecstack -Wl,-z,relro -Wl,-z,now \
	-Wl,--as-needed -Wl,--no-copy-dt-needed-entries -pie $(LDFLAGS)

ifeq (, $(shell which $(PKG_CONFIG)))
$(error pkg-config not installed!)
endif

all : kloak

kloak : src/kloak.c src/kloak.h src/xdg-shell-protocol.h src/xdg-shell-protocol.c src/xdg-output-protocol.h src/xdg-output-protocol.c src/wlr-layer-shell.c src/wlr-layer-shell.h src/wlr-virtual-pointer.c src/wlr-virtual-pointer.h src/virtual-keyboard.c src/virtual-keyboard.h
	$(CC) -g src/kloak.c src/xdg-shell-protocol.c src/xdg-output-protocol.c src/wlr-layer-shell.c src/wlr-virtual-pointer.c src/virtual-keyboard.c -o kloak -lm -lrt $(shell $(PKG_CONFIG) --cflags --libs libinput) $(shell $(PKG_CONFIG) --cflags --libs libevdev) $(shell $(PKG_CONFIG) --cflags --libs wayland-client) $(shell $(PKG_CONFIG) --cflags --libs xkbcommon)

clean :
	rm -f kloak
