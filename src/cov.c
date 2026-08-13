/* cov.c — see cov.h. Opt-in PC coverage bitmap dumped at exit. */
#include "cov.h"
#include <stdlib.h>
#include <stdio.h>

uint8_t *g_cov = NULL;
static const char *cov_path = NULL;

static void cov_dump(void) {
    if (!g_cov || !cov_path) return;
    FILE *f = fopen(cov_path, "w");
    if (!f) return;
    for (unsigned pc = 0; pc < 65536; pc++)
        if (g_cov[pc >> 3] & (1u << (pc & 7)))
            fprintf(f, "%04x\n", pc);
    fclose(f);
}

void cov_init(void) {
    if (g_cov) return;                      /* already initialised */
    cov_path = getenv("PWDBG_COV");
    if (!cov_path || !*cov_path) return;
    g_cov = calloc(8192, 1);                /* 65536 bits */
    if (g_cov) atexit(cov_dump);
}
