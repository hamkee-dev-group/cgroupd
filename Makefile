CC      ?= gcc
CFLAGS  ?= -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
           -D_FILE_OFFSET_BITS=64
LDFLAGS ?=

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
TEST_RUNNER ?= $(shell if [ "$$(id -u)" -ne 0 ] && command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then printf sudo; fi)

BUILD   := build
SRC     := src

COMMON_OBJS := \
    $(BUILD)/log.o \
    $(BUILD)/util.o \
    $(BUILD)/cgroup.o \
    $(BUILD)/psi.o \
    $(BUILD)/proto.o

CGROUPD_OBJS := $(COMMON_OBJS) $(BUILD)/cgroupd.o
CTL_OBJS     := $(COMMON_OBJS) $(BUILD)/cgroupctl.o

BENCH_BIN    := $(BUILD)/memhog $(BUILD)/cpuhog

ALL := $(BUILD)/cgroupd $(BUILD)/cgroupctl $(BENCH_BIN)

.PHONY: all clean install test bench
all: $(ALL)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: $(SRC)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/cgroupd: $(CGROUPD_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/cgroupctl: $(CTL_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/memhog: bench/memhog.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BUILD)/cpuhog: bench/cpuhog.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -rf $(BUILD)

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BUILD)/cgroupd $(DESTDIR)$(BINDIR)/cgroupd
	install -m 0755 $(BUILD)/cgroupctl $(DESTDIR)$(BINDIR)/cgroupctl

test: all
	tests/protocol_injection.sh
	$(TEST_RUNNER) tests/smoke.sh
	$(TEST_RUNNER) tests/wait.sh
	$(TEST_RUNNER) tests/admission.sh
	$(TEST_RUNNER) tests/orchestration.sh
	$(TEST_RUNNER) tests/cleanup.sh
	$(TEST_RUNNER) tests/oom.sh
	$(TEST_RUNNER) tests/pressure.sh
	$(TEST_RUNNER) tests/limit_validation.sh

bench: all
	tests/run_bench.sh
