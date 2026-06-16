#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct DuoOpts {
 const char *rom_dir;

 const char *workdir_a, *workdir_b;
 const char *events_a, *events_b;
 const char *save_eeprom_a, *save_eeprom_b;

 uint64_t max_cycles; /* applied to both peers (0 = unlimited) */

 uint16_t break_a[16];
 int break_a_count;
 uint16_t break_b[16];
 int break_b_count;

 bool trace_pc;
 bool lcd_ascii;
 bool rtc_enabled;
} DuoOpts;

/* Fork into two walker peers connected by a socketpair. Blocks until
 * both peers exit. Returns a non-zero process exit code if either
 * peer ended in error. */
int duo_main(const DuoOpts *o);

/* `pwdbg pair` — like duo, but each peer is a scripted REPL instance,
 * so the scripts can inject keys (menu navigation into CONECTAR),
 * dump lcd/mem/eeprom per instance, and tap/feed the IR stream.
 * Each ROM dir may differ (orig↔orig, custom↔orig, ...). */
typedef struct PairOpts {
 const char *rom_a, *rom_b; /* per-instance ROM dirs */
 const char *script_a, *script_b; /* per-instance repl scripts (required) */
 const char *workdir_a, *workdir_b; /* optional persistent workdirs */
 const char *events_a, *events_b; /* optional JSON event files */
 const char *log_a, *log_b; /* per-instance stdout (default pair_a.log/pair_b.log) */
 uint32_t latency; /* RX delivery delay in cycles (both dirs) */
 uint32_t quantum; /* lockstep sync interval in cycles (0 = default 8192) */
 bool no_lockstep; /* free-run (wall-clock) instead of lockstep */
} PairOpts;

int pair_main(const PairOpts *o);
