#include "common.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "walker.h" /* walkerV2KeypollPC */

static int copy_file(const char *src, const char *dst) {
 FILE *in = fopen(src, "rb");
 if (!in) {
 fprintf(stderr, "pwdbg: cannot open %s: %s\n", src, strerror(errno));
 return 1;
 }
 FILE *out = fopen(dst, "wb");
 if (!out) {
 fprintf(stderr, "pwdbg: cannot write %s: %s\n", dst, strerror(errno));
 fclose(in);
 return 1;
 }
 unsigned char buf[8192];
 size_t n;
 while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
 if (fwrite(buf, 1, n, out) != n) {
 fprintf(stderr, "pwdbg: short write to %s: %s\n", dst, strerror(errno));
 fclose(in); fclose(out); return 1;
 }
 }
 fclose(in);
 fclose(out);
 return 0;
}

static bool file_exists(const char *p) {
 struct stat st;
 return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

int common_setup_workdir(const char *rom_dir, const char *workdir,
 char *workdir_out, bool *persist_out) {
 char path[PATH_MAX];

 if (workdir && *workdir) {
 /* user-provided — create if missing, keep contents if already there */
 if (mkdir(workdir, 0755) != 0 && errno != EEXIST) {
 fprintf(stderr, "pwdbg: mkdir(%s): %s\n", workdir, strerror(errno));
 return 1;
 }
 strncpy(workdir_out, workdir, PATH_MAX - 1);
 workdir_out[PATH_MAX - 1] = '\0';
 *persist_out = true;
 } else {
 char tmpl[] = "/tmp/pwdbg.XXXXXX";
 if (!mkdtemp(tmpl)) {
 fprintf(stderr, "pwdbg: mkdtemp: %s\n", strerror(errno));
 return 1;
 }
 strncpy(workdir_out, tmpl, PATH_MAX - 1);
 workdir_out[PATH_MAX - 1] = '\0';
 *persist_out = false;
 }

 /* copy ROMs in only if missing — persistent workdirs keep their state */
 const char *names[] = { "pwflash.rom", "pweep.rom" };
 for (int i = 0; i < 2; i++) {
 snprintf(path, sizeof path, "%s/%s", workdir_out, names[i]);
 if (file_exists(path)) continue;
 char src[PATH_MAX];
 snprintf(src, sizeof src, "%s/%s", rom_dir, names[i]);
 if (copy_file(src, path) != 0) return 1;
 }
 return 0;
}

void common_cleanup_workdir(const char *workdir, bool persist) {
 if (persist || !workdir || !*workdir) return;
 /* best-effort cleanup: only the known files + the directory. */
 char path[PATH_MAX];
 const char *names[] = { "pwflash.rom", "pweep.rom", "pedometer_state.bin" };
 for (size_t i = 0; i < sizeof(names)/sizeof(*names); i++) {
 snprintf(path, sizeof path, "%s/%s", workdir, names[i]);
 unlink(path);
 }
 rmdir(workdir);
}

int common_lookup_symbol(const char *nm_path, const char *symbol,
 uint32_t *out_addr) {
 FILE *f = fopen(nm_path, "r");
 if (!f) return 1;
 char line[256];
 int rc = 1;
 while (fgets(line, sizeof line, f)) {
 /* nm line: "0000201c T _ui_keypoll_main" */
 unsigned long addr;
 char type, name[160];
 if (sscanf(line, "%lx %c %159s", &addr, &type, name) != 3)
 continue;
 const char *n = (name[0] == '_') ? name + 1 : name;
 if (strcmp(n, symbol) == 0) {
 *out_addr = (uint32_t)addr;
 rc = 0;
 break;
 }
 }
 fclose(f);
 return rc;
}

void common_resolve_v2_keypoll_hook(uint16_t entry_pc, const char *launch_dir) {
 /* Only the custom ROM (entry 0x0080) drains keys through this hook; the
 * original ROM (entry 0x02C4) has its own PC-stable hook in walker.c. */
 if (entry_pc != 0x0080) return;

 const char *symspec = getenv("PWDBG_V2_SYMS");
 char nmpath[PATH_MAX];
 if (symspec && *symspec)
 snprintf(nmpath, sizeof nmpath, "%s", symspec);
 else
 snprintf(nmpath, sizeof nmpath, "%s/build/syms.nm",
 (launch_dir && *launch_dir) ? launch_dir : ".");

 uint32_t addr;
 if (common_lookup_symbol(nmpath, "ui_keypoll_main", &addr) == 0) {
 walkerV2KeypollPC = addr;
 fprintf(stderr, "pwdbg: the custom ROM keypoll hook @0x%04X (ui_keypoll_main)\n",
 (unsigned)addr);
 } else {
 fprintf(stderr, "pwdbg: warning: could not resolve ui_keypoll_main "
 "from %s; menu LEFT/RIGHT injection disabled\n", nmpath);
 }
}

int common_copy_eeprom(const char *workdir, const char *out_path) {
 char src[PATH_MAX];
 snprintf(src, sizeof src, "%s/pweep.rom", workdir);
 return copy_file(src, out_path);
}

FILE *common_open_events(const char *spec, bool *needs_fclose_out) {
 *needs_fclose_out = false;
 if (!spec) return NULL;
 if (strcmp(spec, "-") == 0) return stdout;
 if (strcmp(spec, "stderr") == 0) return stderr;
 FILE *f = fopen(spec, "w");
 if (!f) {
 fprintf(stderr, "pwdbg: cannot write events to %s: %s\n",
 spec, strerror(errno));
 return NULL;
 }
 *needs_fclose_out = true;
 return f;
}
