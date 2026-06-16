# pwdbg — Pokewalker debug CLI
# Links walker.c/queue.c from the emulator core as source: its .o files
# land here in build/.
# Compiles with -D__3DS__ so the SCI3↔IR bridge code in walker.c is active;
# shim headers in src/ satisfy the `#include "ir.h"` / `#include "irtrace.h"`
# that walker.c emits under that guard.
#
# SELF-CONTAINED: the emulator core (walker.c, walker.h, queue.c, queue.h,
# definitions.h, regRef.h, utils.c) is VENDORED into vendor/ — pinned
# copies taken from the `pokestride` repo. This repo builds standalone with
# no cross-repo path references. See vendor/README.md for the provenance
# and how to refresh the snapshot. Override POKESTRIDER to build against a
# live pokestride checkout instead:
# make POKESTRIDER=an external pokeStride checkout

CC ?= gcc
POKESTRIDER ?= vendor
BUILD = build

INCLUDES = -I src -I $(POKESTRIDER)
CFLAGS ?= -O2 -g -Wall -Wno-unused-variable -Wno-unused-but-set-variable \
 -Wno-unused-function
CPPFLAGS = $(INCLUDES) -D__3DS__
LDFLAGS ?=
LDLIBS ?= -lm

PWDBG_SRCS = src/pwdbg.c src/run.c src/repl.c src/duo.c src/events.c \
 src/ir_stubs.c src/ir_bridge.c \
 src/common.c src/lcd.c src/walker_ext.c
PWDBG_OBJS = $(patsubst src/%.c,$(BUILD)/%.o,$(PWDBG_SRCS))

# walker.c is unity-included by walker_ext.c — do NOT compile it
# separately or we get duplicate symbols.
CORE_OBJS = $(BUILD)/queue.o

TARGET = pwdbg

all: $(TARGET)

$(TARGET): $(PWDBG_OBJS) $(CORE_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(BUILD)/%.o: $(POKESTRIDER)/%.c | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD) $(TARGET)

.PHONY: all clean
