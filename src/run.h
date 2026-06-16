#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Options for the batch `run` subcommand. */
typedef struct RunOpts {
 const char *rom_dir; /* default ./roms */
 const char *workdir; /* NULL = tempdir */
 const char *events_spec; /* NULL = off ("-" = stdout, path, stderr) */
 const char *lcd_pgm; /* NULL = skip PGM dump */
 const char *save_eeprom; /* NULL = don't copy pweep.rom out */
 uint64_t max_cycles; /* 0 = unlimited */
 uint16_t break_pcs[16];
 int break_count;
 uint16_t count_pcs[8]; /* PCs to tally (no stop) - paint-rate probe */
 int count_pc_count;
 bool watch_clear; /* histogram PCs that clear 0xF7B5 bit0 (diag) */
 bool no_stuck; /* disable the stuck-loop early abort */
 bool dump_er; /* dump ER0..7 + [0xF7B5] when PC == dump_er_pc */
 uint16_t dump_er_pc;
 bool hist_pc; /* histogram instr count per 64-byte PC bucket */
 bool trace_pc; /* emit one "pc" event per instruction */
 bool lcd_ascii; /* print LCD ASCII at end to stderr */
 bool rtc_enabled; /* fire quarterRTCInterrupt when due */
 int instance; /* JSON event "i" label (0 = single-peer) */
} RunOpts;

/* Run until max_cycles, a break hit, a stuck loop, an error, or SIGINT.
 * Returns the process exit code. */
int run_main(const RunOpts *opts);
