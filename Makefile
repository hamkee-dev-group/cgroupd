CC      ?= gcc
CFLAGS  ?= -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
           -D_FILE_OFFSET_BITS=64
LDFLAGS ?=

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

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
	tests/smoke.sh
	tests/wait.sh
	tests/admission.sh
	tests/orchestration.sh
	tests/cleanup.sh
	tests/oom.sh
	tests/pressure.sh

bench: all
	tests/run_bench.sh
