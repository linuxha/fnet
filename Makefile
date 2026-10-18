# fnetd — FLEX disk / S-Record paper tape server
# See docs/fnetd-fsd.md

VERSION  := 0.0.1
PREFIX   ?= /usr/local
SBINDIR  ?= $(PREFIX)/sbin
BINDIR   ?= $(PREFIX)/bin
SYSCONFDIR ?= /etc
UNITDIR  ?= /etc/systemd/system
SYSUSERSDIR ?= /usr/lib/sysusers.d

CC       ?= gcc
CSTD     := -std=c11 -D_GNU_SOURCE
WARN     := -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow \
            -Wstrict-prototypes -Wmissing-prototypes -Wvla
CFLAGS   ?= -O2 -g
ALL_CFLAGS = $(CSTD) $(WARN) -DFNET_VERSION=\"$(VERSION)\" -Isrc/lib $(CFLAGS)
LDFLAGS  ?=

BUILD    := build

LIB_SRC  := $(wildcard src/lib/*.c)
LIB_OBJ  := $(LIB_SRC:%.c=$(BUILD)/%.o)
LIB      := $(BUILD)/libfnet.a

FNETD_SRC := $(wildcard src/fnetd/*.c)
FNETD_OBJ := $(FNETD_SRC:%.c=$(BUILD)/%.o)

BINS     := $(BUILD)/fnetd $(BUILD)/fnetctl $(BUILD)/fnetimg

UNIT_SRC := $(wildcard tests/unit/test_*.c)
UNIT_BIN := $(UNIT_SRC:tests/unit/%.c=$(BUILD)/tests/%)

.PHONY: all clean test itest check asan install

all: $(BINS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) -MMD -MP -c -o $@ $<

$(LIB): $(LIB_OBJ)
	$(AR) rcs $@ $^

$(BUILD)/fnetd: $(FNETD_OBJ) $(LIB)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $^

$(BUILD)/fnetctl: $(BUILD)/src/fnetctl/fnetctl.o $(LIB)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $^

$(BUILD)/fnetimg: $(BUILD)/src/fnetimg/fnetimg.o $(LIB)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $^

# Unit tests link the self-contained daemon modules.
UNIT_FNETD_OBJ := $(BUILD)/src/fnetd/config.o $(BUILD)/src/fnetd/telnet.o

$(BUILD)/tests/%: tests/unit/%.c $(UNIT_FNETD_OBJ) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) -Isrc/fnetd -o $@ $^

test: $(UNIT_BIN) $(BUILD)/fnetd
	@set -e; for t in $(UNIT_BIN); do echo "== $$t"; $$t; done
	$(BUILD)/fnetd -t -c etc/fnetd.conf

itest: all
	python3 -m unittest discover -s tests/itest -p 'test_*.py' -v

check: test itest

asan:
	$(MAKE) BUILD=build-asan CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
	        LDFLAGS="-fsanitize=address,undefined" all test

install: all
	install -d $(DESTDIR)$(SBINDIR) $(DESTDIR)$(BINDIR)
	install -m 0755 $(BUILD)/fnetd $(DESTDIR)$(SBINDIR)/fnetd
	install -m 0755 $(BUILD)/fnetctl $(BUILD)/fnetimg $(DESTDIR)$(BINDIR)/
	install -d $(DESTDIR)$(SYSCONFDIR)/fnetd
	test -e $(DESTDIR)$(SYSCONFDIR)/fnetd/fnetd.conf || \
	    install -m 0644 etc/fnetd.conf $(DESTDIR)$(SYSCONFDIR)/fnetd/fnetd.conf
	install -d $(DESTDIR)$(UNITDIR) $(DESTDIR)$(SYSUSERSDIR)
	sed -e 's|@SBINDIR@|$(SBINDIR)|g' -e 's|@SYSCONFDIR@|$(SYSCONFDIR)|g' \
	    systemd/fnetd.service.in > $(DESTDIR)$(UNITDIR)/fnetd.service
	install -m 0644 systemd/fnetd.sysusers $(DESTDIR)$(SYSUSERSDIR)/fnetd.conf

clean:
	rm -rf build build-asan

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
