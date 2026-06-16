#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "events.h"

/* Shared helpers used by every subcommand. */

/* Prepare a working directory with pwflash.rom + pweep.rom copied in.
 * rom_dir source ROM directory (e.g. /workspace/05-roms/pokewalker)
 * workdir if non-NULL and non-empty: use as-is, create if missing,
 * keep after exit. If NULL or empty: mktemp, delete on
 * cleanup.
 * workdir_out on success, receives the path actually used. Must be
 * at least PATH_MAX bytes. Caller owns the storage.
 * persist_out on success, set true iff workdir was user-provided.
 *
 * Returns 0 on success. */
int common_setup_workdir(const char *rom_dir, const char *workdir,
 char *workdir_out, bool *persist_out);

/* Remove a tempdir created by common_setup_workdir (if persist==false). */
void common_cleanup_workdir(const char *workdir, bool persist);

/* Copy workdir/pweep.rom to out_path, for --save-eeprom. Returns 0 on ok. */
int common_copy_eeprom(const char *workdir, const char *out_path);

/* Resolve an --events target spec:
 * "-" → stdout
 * "stderr" → stderr
 * "<path>" → fopen for write
 * NULL → NULL (events disabled)
 * Caller is responsible for fclose() on user-supplied paths; stdout/stderr
 * are never closed. *needs_fclose_out is set accordingly. */
FILE *common_open_events(const char *spec, bool *needs_fclose_out);

/* Look up a symbol's address in an `nm`-style symbols file (e.g.
 * build/syms.nm). Matches the symbol with or without a leading underscore.
 * On success writes the address to *out_addr and returns 0; returns non-zero
 * if the file can't be opened or the symbol isn't found. */
int common_lookup_symbol(const char *nm_path, const char *symbol,
 uint32_t *out_addr);

/* Resolve the custom ROM button-inject hook (ui_keypoll_main) into the
 * walker's walkerV2KeypollPC global, so the hook survives firmware rebuilds
 * that move the function. Only acts when entry_pc == 0x0080 (the custom ROM);
 * the original ROM uses a different, PC-stable hook. The symbols file is
 * $PWDBG_V2_SYMS if set, else "<launch_dir>/build/syms.nm". */
void common_resolve_v2_keypoll_hook(uint16_t entry_pc, const char *launch_dir);

/* IR stubs back-channel — optional hook used by the REPL. */
void ir_stubs_set_events(EventStream *s);
void ir_stubs_inject_rx(const unsigned char *buf, size_t n);

/* IR peer harness primitives (see IR_PEER_HARNESS.md):
 * - tap: dump every SCI3 TX byte as "<cycle> <hexbyte>" lines.
 * - scheduled injection: deliver bytes to RX at an absolute cycle.
 * - feed: replay a tap file with stamps re-based to "now".
 * - latency: delay bridge (peer) bytes by N cycles before delivery.
 * - cycle ptr: where the stubs read the instance's cycle counter. */
void ir_stubs_set_cycle_ptr(const uint64_t *p);
int ir_stubs_tap_open(const char *path); /* NULL = close; 0 on ok */
void ir_stubs_tap_close(void);
void ir_stubs_inject_rx_at(const unsigned char *buf, size_t n, uint64_t due_abs);
long ir_stubs_feed_file(const char *path, int abs); /* bytes scheduled, -1 on error */
void ir_stubs_set_latency(uint32_t cycles);
void ir_stubs_set_bridge_framed(int on); /* pair: [stamp][len][payload] frames */
