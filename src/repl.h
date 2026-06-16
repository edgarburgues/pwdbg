#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct ReplOpts {
 const char *rom_dir; /* default /workspace/05-roms/pokewalker */
 const char *workdir; /* NULL = tempdir */
 const char *events_spec; /* NULL = off ("-" stdout / "stderr" / path) */
 const char *script; /* NULL = interactive; else run commands from file */
 const char *ir_listen; /* Unix socket path to listen on (peer server) */
 const char *ir_connect; /* Unix socket path to connect to (peer client) */
 int instance; /* JSON "i" label (0=single, 1/2 for duo peers) */
 bool ir_sniff; /* start with IR event emission on */
} ReplOpts;

int repl_main(const ReplOpts *opts);

/* pwdbg pair: exchange a 1-byte token with the peer on `fd` every
 * `quantum` emulated cycles, bounding the two instances' clock skew.
 * Call before repl_main. quantum 0 = default (8192). */
void repl_set_lockstep(int fd, uint32_t quantum);
