#pragma once

/* LCD introspection helpers. The emulator exposes fillVideoBuffer()
 * that converts the PokéWalker's 96x64 2-bit grayscale LCD into a
 * 32-bit RGB framebuffer; pwdbg renders that into terminal-friendly
 * ASCII or a portable graymap (PGM) for offline inspection. */

/* Print the LCD to `out` as ASCII (rows are pairs of H8 rows, collapsed
 * for terminal aspect ratio). Four shade levels ` .:#`. */
#include <stdio.h>

void lcd_dump_ascii(FILE *out);

/* Write the LCD as a binary-PGM (P5) file with 4 gray levels (0,85,170,255).
 * Returns 0 on success. */
int lcd_dump_pgm(const char *path);
