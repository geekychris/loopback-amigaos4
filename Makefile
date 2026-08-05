# loopback.device — Makefile.
#
# Invoked inside the walkero AmigaOS4 GCC 11 Docker container by
# scripts/build.sh. Do not run this Makefile directly on the host — the
# CC name (ppc-amigaos-gcc) only exists inside the container.

CC     = ppc-amigaos-gcc
STRIP  = ppc-amigaos-strip

CFLAGS = -mcrt=newlib -mhard-float -O2 -mcpu=440 -Wall -Wextra \
         -D__PPC__ -D__USE_OLD_TIMEVAL__ \
         -I./include

DEVLDFLAGS = -nostartfiles \
             -Wl,-z,common-page-size=4096 \
             -Wl,-z,max-page-size=4096

TESTLDFLAGS = -lauto

BUILD    = build
DEV      = $(BUILD)/loopback.device
DEV_DBG  = $(BUILD)/loopback.device.debug

TEST_NAMES = testopen teststat testloop testhello
TESTS      = $(patsubst %,$(BUILD)/%,$(TEST_NAMES))

DEV_SRC  = src/device.c
DEV_OBJ  = $(BUILD)/device.o
DEV_OBJ_DBG = $(BUILD)/device.dbg.o
DEV_HDRS = include/version.h include/loopback.h

.PHONY: all clean test

all: $(DEV) $(DEV_DBG) $(TESTS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c $(DEV_HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.dbg.o: src/%.c $(DEV_HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -DDEBUG -g -c $< -o $@

$(DEV): $(DEV_OBJ)
	$(CC) $(DEV_OBJ) -o $@ $(DEVLDFLAGS)
	$(STRIP) --strip-all $@

$(DEV_DBG): $(DEV_OBJ_DBG)
	$(CC) $(DEV_OBJ_DBG) -o $@ $(DEVLDFLAGS)

$(BUILD)/%: tests/%.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@ $(TESTLDFLAGS)

test: $(TESTS)

clean:
	rm -rf $(BUILD)
