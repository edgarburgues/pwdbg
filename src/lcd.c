#include "lcd.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "walker.h"

/* Defeat the double-buffer toggle when dumping the LCD.
 *
 * fillVideoBuffer() copies the CURRENT video buffer then flips
 * currentBuffer, and SSU pixel writes only ever land in buffer 0, so a
 * single call may hand back the empty buffer (the dump comes out blank).
 * We call it TWICE - that returns both buffers and leaves currentBuffer
 * unchanged - and keep whichever has actual ink. This makes `lcd pgm` /
 * `lcd` deterministic regardless of how many times the screen flipped
 * during the run, so consecutive dumps no longer alternate real/blank. */
static long fb_ink(const uint32_t *fb) {
 long n = 0;
 for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
 if ((fb[i] & 0xFF) < 0xC0) { /* anything darker than the LCD background */
 n++;
 }
 }
 return n;
}

static void fill_painted(uint32_t *fb) {
 /* If the ROM actively page-flips the SSD1854 display-start-line (the
 * Nintendo ROM, and the custom ROM once Part 1 lands), fillVideoBuffer()
 * returns exactly the half the panel is scanning out — the front buffer.
 * Just take it. Note: "active" means it has set a NON-zero start-line; a
 * ROM that only ever sets line 0 while drawing into the upper half (the custom ROM
 * today, double-buffer not yet wired) is not flipping, so we must NOT
 * honour its stuck start-line or we'd show its blank half. */
 if (lcdStartLineActive()) {
 fillVideoBuffer(fb);
 return;
 }
 /* Fallback for ROMs that don't actively flip (the custom ROM today):
 * fillVideoBuffer() free-runs its half toggle, so a single call may hand
 * back the empty half. Call it twice (returns both halves, toggle nets to
 * a no-op) and keep whichever has actual ink, so dumps stay deterministic. */
 uint32_t a[LCD_WIDTH * LCD_HEIGHT];
 uint32_t b[LCD_WIDTH * LCD_HEIGHT];
 fillVideoBuffer(a); /* current buffer, then toggles */
 fillVideoBuffer(b); /* the other buffer, toggles back */
 const uint32_t *src = (fb_ink(a) >= fb_ink(b)) ? a : b;
 memcpy(fb, src, (size_t)(LCD_WIDTH * LCD_HEIGHT) * sizeof *fb);
}

void lcd_dump_ascii(FILE *out) {
 uint32_t fb[LCD_WIDTH * LCD_HEIGHT];
 fill_painted(fb);

 fprintf(out, "LCD %dx%d (2-bit gray, shown at %dx%d, shades \" .:#\"):\n",
 LCD_WIDTH, LCD_HEIGHT, LCD_WIDTH, LCD_HEIGHT / 2);
 const char shade[] = " .:#";
 for (int y = 0; y < LCD_HEIGHT; y += 2) {
 for (int x = 0; x < LCD_WIDTH; x++) {
 uint32_t px = fb[y * LCD_WIDTH + x];
 int lum = px & 0xFF; /* R=G=B for grayscale */
 int idx = lum < 0x40 ? 3 : lum < 0x80 ? 2 : lum < 0xC0 ? 1 : 0;
 fputc(shade[idx], out);
 }
 fputc('\n', out);
 }
}

int lcd_dump_pgm(const char *path) {
 uint32_t fb[LCD_WIDTH * LCD_HEIGHT];
 fill_painted(fb);

 FILE *f = fopen(path, "wb");
 if (!f) {
 fprintf(stderr, "pwdbg: cannot write PGM %s: %s\n", path, strerror(errno));
 return 1;
 }
 fprintf(f, "P5\n%d %d\n255\n", LCD_WIDTH, LCD_HEIGHT);
 for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
 uint8_t lum = fb[i] & 0xFF;
 fputc(lum, f);
 }
 fclose(f);
 return 0;
}
