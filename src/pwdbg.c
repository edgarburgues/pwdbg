#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "run.h"
#include "repl.h"
#include "duo.h"

static void usage(FILE *f) {
 fputs(
 "pwdbg — Pokewalker debug emulator CLI\n"
 "\n"
 "usage:\n"
 " pwdbg run [options] run the emulator in batch mode\n"
 " pwdbg repl [options] interactive debugger (stdin commands)\n"
 " pwdbg duo [options] two walkers peered over virtual IR (batch)\n"
 " pwdbg pair [options] two SCRIPTED REPL walkers over virtual IR\n"
 " pwdbg help this message\n"
 " pwdbg version version info\n"
 "\n"
 "'pwdbg run' options:\n"
 " --rom-dir DIR source ROMs (default: /workspace/05-roms/pokewalker)\n"
 " --workdir DIR persistent working directory (default: tempdir)\n"
 " --cycles N stop after N H8 cycles (0 = unlimited; default: 10000000)\n"
 " --break PC break at PC (hex, repeatable; up to 16)\n"
 " --count-pc PC tally hits of PC without stopping (repeatable; up to 8)\n"
 " --watch-clear histogram the PCs that clear 0xF7B5 bit0 (diagnostic)\n"
 " --hist-pc histogram instruction count per 64-byte PC bucket (diag)\n"
 " --no-stuck do not early-abort on a detected tight loop\n"
 " --trace-pc emit one JSON event per instruction (very noisy)\n"
 " --lcd dump final LCD as ASCII to stderr\n"
 " --lcd-pgm FILE write final LCD to FILE as binary PGM (P5)\n"
 " --save-eeprom FILE after run, copy pweep.rom to FILE\n"
 " --events TARGET emit JSON Lines events to TARGET:\n"
 " '-' → stdout\n"
 " stderr → stderr\n"
 " <path> → file\n"
 " --no-rtc do not fire quarter-second RTC interrupts\n"
 "\n"
 "event format: one JSON object per line, fields:\n"
 " t total H8 cycles elapsed\n"
 " ev event kind: start | pc | ir_tx_start | ir_tx | ir_tx_end |\n"
 " ir_tx_flush | ir_rx_start | ir_rx | ir_rx_stop | break |\n"
 " rtc | stuck | error | signal | done\n"
 " plus kind-specific fields (pc, byte/bytes, reason, ...).\n",
 f);
}

static int parse_hex_u16(const char *s, uint16_t *out) {
 char *end = NULL;
 unsigned long v = strtoul(s, &end, 0);
 if (!end || *end != '\0' || v > 0xFFFF) return -1;
 *out = (uint16_t)v;
 return 0;
}

static int cmd_run(int argc, char **argv) {
 RunOpts o = (RunOpts){
 .rom_dir = NULL,
 .workdir = NULL,
 .events_spec= NULL,
 .lcd_pgm = NULL,
 .save_eeprom= NULL,
 .max_cycles = 10000000ULL,
 .break_count= 0,
 .count_pc_count = 0,
 .trace_pc = false,
 .lcd_ascii = false,
 .rtc_enabled= true,
 .instance = 0,
 };

 for (int i = 0; i < argc; i++) {
 const char *a = argv[i];
 if (!strcmp(a, "--rom-dir") && i+1 < argc) o.rom_dir = argv[++i];
 else if (!strcmp(a, "--workdir") && i+1 < argc) o.workdir = argv[++i];
 else if (!strcmp(a, "--events") && i+1 < argc) o.events_spec = argv[++i];
 else if (!strcmp(a, "--lcd-pgm") && i+1 < argc) o.lcd_pgm = argv[++i];
 else if (!strcmp(a, "--save-eeprom") && i+1 < argc) o.save_eeprom = argv[++i];
 else if (!strcmp(a, "--cycles") && i+1 < argc) {
 char *end; o.max_cycles = strtoull(argv[++i], &end, 0);
 if (*end) { fprintf(stderr, "pwdbg: bad --cycles\n"); return 2; }
 }
 else if (!strcmp(a, "--break") && i+1 < argc) {
 if (o.break_count >= (int)(sizeof(o.break_pcs)/sizeof(o.break_pcs[0]))) {
 fprintf(stderr, "pwdbg: too many --break (max 16)\n"); return 2;
 }
 if (parse_hex_u16(argv[++i], &o.break_pcs[o.break_count]) != 0) {
 fprintf(stderr, "pwdbg: bad --break value '%s'\n", argv[i]);
 return 2;
 }
 o.break_count++;
 }
 else if (!strcmp(a, "--count-pc") && i+1 < argc) {
 if (o.count_pc_count >= (int)(sizeof(o.count_pcs)/sizeof(o.count_pcs[0]))) {
 fprintf(stderr, "pwdbg: too many --count-pc (max 8)\n"); return 2;
 }
 if (parse_hex_u16(argv[++i], &o.count_pcs[o.count_pc_count]) != 0) {
 fprintf(stderr, "pwdbg: bad --count-pc value '%s'\n", argv[i]);
 return 2;
 }
 o.count_pc_count++;
 }
 else if (!strcmp(a, "--trace-pc")) o.trace_pc = true;
 else if (!strcmp(a, "--watch-clear")) o.watch_clear = true;
 else if (!strcmp(a, "--hist-pc")) o.hist_pc = true;
 else if (!strcmp(a, "--no-stuck")) o.no_stuck = true;
 else if (!strcmp(a, "--dump-er-at") && i+1 < argc) {
 if (parse_hex_u16(argv[++i], &o.dump_er_pc) != 0) {
 fprintf(stderr, "pwdbg: bad --dump-er-at value '%s'\n", argv[i]); return 2;
 }
 o.dump_er = true;
 }
 else if (!strcmp(a, "--lcd")) o.lcd_ascii = true;
 else if (!strcmp(a, "--no-rtc")) o.rtc_enabled = false;
 else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); return 0; }
 else {
 fprintf(stderr, "pwdbg: unknown option '%s' (try 'pwdbg help')\n", a);
 return 2;
 }
 }

 return run_main(&o);
}

static int cmd_duo(int argc, char **argv) {
 DuoOpts o = (DuoOpts){
 .rom_dir = NULL,
 .workdir_a = NULL,
 .workdir_b = NULL,
 .events_a = NULL,
 .events_b = NULL,
 .save_eeprom_a = NULL,
 .save_eeprom_b = NULL,
 .max_cycles = 10000000ULL,
 .break_a_count = 0,
 .break_b_count = 0,
 .trace_pc = false,
 .lcd_ascii = false,
 .rtc_enabled = true,
 };

 for (int i = 0; i < argc; i++) {
 const char *a = argv[i];
 if (!strcmp(a, "--rom-dir") && i+1 < argc) o.rom_dir = argv[++i];
 else if (!strcmp(a, "--workdir-a") && i+1 < argc) o.workdir_a = argv[++i];
 else if (!strcmp(a, "--workdir-b") && i+1 < argc) o.workdir_b = argv[++i];
 else if (!strcmp(a, "--events-a") && i+1 < argc) o.events_a = argv[++i];
 else if (!strcmp(a, "--events-b") && i+1 < argc) o.events_b = argv[++i];
 else if (!strcmp(a, "--save-eeprom-a")&& i+1 < argc) o.save_eeprom_a= argv[++i];
 else if (!strcmp(a, "--save-eeprom-b")&& i+1 < argc) o.save_eeprom_b= argv[++i];
 else if (!strcmp(a, "--cycles") && i+1 < argc) {
 char *end; o.max_cycles = strtoull(argv[++i], &end, 0);
 if (*end) { fprintf(stderr, "pwdbg duo: bad --cycles\n"); return 2; }
 }
 else if (!strcmp(a, "--break-a") && i+1 < argc) {
 if (o.break_a_count >= 16) { fprintf(stderr, "too many --break-a\n"); return 2; }
 if (parse_hex_u16(argv[++i], &o.break_a[o.break_a_count]) != 0) {
 fprintf(stderr, "pwdbg duo: bad --break-a\n"); return 2;
 }
 o.break_a_count++;
 }
 else if (!strcmp(a, "--break-b") && i+1 < argc) {
 if (o.break_b_count >= 16) { fprintf(stderr, "too many --break-b\n"); return 2; }
 if (parse_hex_u16(argv[++i], &o.break_b[o.break_b_count]) != 0) {
 fprintf(stderr, "pwdbg duo: bad --break-b\n"); return 2;
 }
 o.break_b_count++;
 }
 else if (!strcmp(a, "--trace-pc")) o.trace_pc = true;
 else if (!strcmp(a, "--no-rtc")) o.rtc_enabled = false;
 else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
 fputs(
 "pwdbg duo [options] two walker instances linked by a virtual IR channel.\n"
 "\n"
 " --rom-dir DIR shared ROM source\n"
 " --workdir-a/-b DIR persistent workdirs (default: tempdirs)\n"
 " --cycles N cycle limit applied to both peers\n"
 " --break-a/-b PC break PC in peer A or B (repeatable)\n"
 " --events-a/-b FILE JSON event file for each peer\n"
 " --save-eeprom-a/-b FILE copy final pweep.rom out\n"
 " --trace-pc emit one pc event per instruction (noisy)\n"
 " --no-rtc disable quarter-RTC interrupts in both\n"
 "\n"
 "events carry \"i\":1 (peer A) or \"i\":2 (peer B).\n",
 stdout);
 return 0;
 }
 else {
 fprintf(stderr, "pwdbg duo: unknown option '%s'\n", a);
 return 2;
 }
 }

 return duo_main(&o);
}

static int cmd_pair(int argc, char **argv) {
 PairOpts o = (PairOpts){0};
 for (int i = 0; i < argc; i++) {
 const char *a = argv[i];
 if (!strcmp(a, "--rom-a") && i+1 < argc) o.rom_a = argv[++i];
 else if (!strcmp(a, "--rom-b") && i+1 < argc) o.rom_b = argv[++i];
 else if (!strcmp(a, "--script-a") && i+1 < argc) o.script_a = argv[++i];
 else if (!strcmp(a, "--script-b") && i+1 < argc) o.script_b = argv[++i];
 else if (!strcmp(a, "--workdir-a") && i+1 < argc) o.workdir_a = argv[++i];
 else if (!strcmp(a, "--workdir-b") && i+1 < argc) o.workdir_b = argv[++i];
 else if (!strcmp(a, "--events-a") && i+1 < argc) o.events_a = argv[++i];
 else if (!strcmp(a, "--events-b") && i+1 < argc) o.events_b = argv[++i];
 else if (!strcmp(a, "--log-a") && i+1 < argc) o.log_a = argv[++i];
 else if (!strcmp(a, "--log-b") && i+1 < argc) o.log_b = argv[++i];
 else if (!strcmp(a, "--latency") && i+1 < argc)
 o.latency = (uint32_t)strtoul(argv[++i], NULL, 0);
 else if (!strcmp(a, "--quantum") && i+1 < argc)
 o.quantum = (uint32_t)strtoul(argv[++i], NULL, 0);
 else if (!strcmp(a, "--no-lockstep")) o.no_lockstep = true;
 else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
 fputs(
 "pwdbg pair [options] two scripted REPL walkers linked by virtual IR.\n"
 "\n"
 " --rom-a/-b DIR per-instance ROM dir (orig<->orig, the custom ROM<->orig, ...)\n"
 " --script-a/-b FILE per-instance repl script (REQUIRED; keys, runs, dumps)\n"
 " --workdir-a/-b DIR persistent workdirs (default: tempdirs)\n"
 " --events-a/-b FILE JSON event file per instance\n"
 " --log-a/-b FILE per-instance stdout (default pair_a.log/pair_b.log)\n"
 " --latency N delay RX delivery by N cycles in both directions\n"
 " --quantum N lockstep sync interval in cycles (default 8192)\n"
 " --no-lockstep free-run on wall-clock (nondeterministic)\n"
 "\n"
 "TX(A)->RX(B) and TX(B)->RX(A) over a socketpair. Inside the scripts the\n"
 "full repl is available: key, run, mem, eeprom, lcd pgm, ir tap/feed, ...\n",
 stdout);
 return 0;
 }
 else { fprintf(stderr, "pwdbg pair: unknown option '%s'\n", a); return 2; }
 }
 return pair_main(&o);
}

static int cmd_repl(int argc, char **argv) {
 ReplOpts o = (ReplOpts){
 .rom_dir = NULL,
 .workdir = NULL,
 .events_spec = NULL,
 .script = NULL,
 .ir_listen = NULL,
 .ir_connect = NULL,
 .instance = 0,
 .ir_sniff = false,
 };
 for (int i = 0; i < argc; i++) {
 const char *a = argv[i];
 if (!strcmp(a, "--rom-dir") && i+1 < argc) o.rom_dir = argv[++i];
 else if (!strcmp(a, "--workdir") && i+1 < argc) o.workdir = argv[++i];
 else if (!strcmp(a, "--events") && i+1 < argc) o.events_spec= argv[++i];
 else if (!strcmp(a, "--script") && i+1 < argc) o.script = argv[++i];
 else if (!strcmp(a, "--ir-listen") && i+1 < argc) o.ir_listen = argv[++i];
 else if (!strcmp(a, "--ir-connect")&& i+1 < argc) o.ir_connect = argv[++i];
 else if (!strcmp(a, "--instance") && i+1 < argc) o.instance = atoi(argv[++i]);
 else if (!strcmp(a, "--ir-sniff")) o.ir_sniff = true;
 else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
 fputs(
 "pwdbg repl [options] interactive debugger.\n"
 " --rom-dir DIR source ROMs (default /workspace/05-roms/pokewalker)\n"
 " --workdir DIR persistent working dir (default tempdir)\n"
 " --events TARGET JSON event stream: '-' | stderr | <path>\n"
 " --script FILE non-interactive: read commands from FILE\n"
 " --ir-sniff start with IR event emission enabled\n"
 " --ir-listen PATH listen on Unix socket (peer server)\n"
 " --ir-connect PATH connect to Unix socket (peer client)\n"
 " --instance N JSON 'i' label (1 or 2 for paired debugging)\n"
 "type 'help' at the pwdbg> prompt for command list.\n",
 stdout);
 return 0;
 }
 else { fprintf(stderr, "pwdbg repl: unknown option '%s'\n", a); return 2; }
 }
 return repl_main(&o);
}

int main(int argc, char **argv) {
 if (argc < 2) { usage(stderr); return 2; }

 const char *sub = argv[1];
 int sub_argc = argc - 2;
 char **sub_argv = argv + 2;

 if (!strcmp(sub, "run")) return cmd_run(sub_argc, sub_argv);
 if (!strcmp(sub, "repl")) return cmd_repl(sub_argc, sub_argv);
 if (!strcmp(sub, "duo")) return cmd_duo(sub_argc, sub_argv);
 if (!strcmp(sub, "pair")) return cmd_pair(sub_argc, sub_argv);
 if (!strcmp(sub, "help") ||
 !strcmp(sub, "-h") ||
 !strcmp(sub, "--help")) { usage(stdout); return 0; }
 if (!strcmp(sub, "version")) {
 puts("pwdbg 0.3 (run + repl + duo IR bridge)");
 return 0;
 }
 fprintf(stderr, "pwdbg: unknown subcommand '%s' (try 'pwdbg help')\n", sub);
 return 2;
}
