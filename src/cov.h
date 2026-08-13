/* cov.h — opt-in per-PC execution coverage.
 *
 * Enabled by setting the env var PWDBG_COV=<path>. When set, cov_init()
 * allocates a 64 KiB-address coverage bitmap and registers an atexit dump
 * that writes every executed PC (one 4-hex-digit address per line) to the
 * path. Useful to see which code paths a run actually exercises; no effect when the
 * env var is unset (g_cov stays NULL, the hot-loop mark is a single branch).
 */
#ifndef PWDBG_COV_H
#define PWDBG_COV_H
#include <stdint.h>

extern uint8_t *g_cov;          /* NULL unless PWDBG_COV is set; 8192 bytes */
void cov_init(void);            /* idempotent; reads PWDBG_COV */

#define COV_MARK(pc) do { if (g_cov) g_cov[(uint16_t)(pc) >> 3] |= \
                          (uint8_t)(1u << ((pc) & 7)); } while (0)

#endif
