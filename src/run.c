#include "run.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "events.h"
#include "lcd.h"
#include "walker.h"
#include "walker_ext.h"

/* Default ROM location. Overridable via --rom-dir. */
#define DEFAULT_ROM_DIR "./roms"

/* RTC quarter-second: 921600 CPU cycles at 3.6864 MHz. */
#define RTC_QUARTER_CYCLES 921600ULL

static volatile sig_atomic_t stopRequested = 0;
static void on_sigint(int sig) { (void)sig; stopRequested = 1; }

static bool is_break(const RunOpts *o, uint16_t pc) {
 for (int i = 0; i < o->break_count; i++)
 if (o->break_pcs[i] == pc) return true;
 return false;
}

int run_main(const RunOpts *opts) {
 char workdir[PATH_MAX];
 bool persist = false;
 const char *rom_dir = opts->rom_dir ? opts->rom_dir : DEFAULT_ROM_DIR;

 /* Capture the launch directory before chdir — the custom ROM symbol table
 * (build/syms.nm) is resolved relative to it. */
 char origcwd[PATH_MAX];
 if (!getcwd(origcwd, sizeof origcwd)) origcwd[0] = '\0';

 if (common_setup_workdir(rom_dir, opts->workdir, workdir, &persist) != 0)
 return 2;

 if (chdir(workdir) != 0) {
 perror("pwdbg: chdir");
 common_cleanup_workdir(workdir, persist);
 return 2;
 }

 bool events_owned = false;
 FILE *events_out = common_open_events(opts->events_spec, &events_owned);

 /* Live counters, referenced by the event stream. */
 uint64_t totalCycles = 0;
 uint16_t currentPC = 0;

 EventStream ev_store;
 EventStream *ev = events_out ? &ev_store : NULL;
 if (ev) ev_init(ev, events_out, &totalCycles, &currentPC, opts->instance);
 ir_stubs_set_events(ev);

 signal(SIGINT, on_sigint);

 initWalker();
 currentPC = getPC();
 common_resolve_v2_keypoll_hook(getEntry(), origcwd);
 ir_stubs_set_cycle_ptr(&totalCycles);

 ev_emit(ev, "start",
 "\"pc\":\"0x%04X\",\"max_cycles\":%llu,\"rtc\":%s",
 currentPC, (unsigned long long)opts->max_cycles,
 opts->rtc_enabled ? "true" : "false");

 fprintf(stderr, "pwdbg: entry 0x%04X, workdir %s%s\n",
 currentPC, workdir, persist ? " (persistent)" : "");

 /* Loop detection state (same idea as pokestroller's cli_main.c). */
 uint16_t pcHist[8] = {0};
 int pcHistIdx = 0;
 uint32_t stuckCount = 0;
 uint64_t instrCount = 0;
 int exit_reason = 0; /* 0=limit, 1=break, 2=stuck, 3=error, 4=signal */

 uint64_t rtcAccum = 0;
 uint16_t lastBreakPC = 0xFFFF;
 uint64_t rtcFires = 0;
 uint64_t count_hits[8] = {0};

 /* --watch-clear diagnostic: histogram which PC clears/sets 0xF7B5 bit0. */
 uint8_t prev_f7b5 = (uint8_t)readMem(0xF7B5);
 uint16_t clr_pc[32]; uint64_t clr_cnt[32]; int clr_n = 0;
 uint16_t set_pc[32]; uint64_t set_cnt[32]; int set_n = 0;
 int dumpedEr = 0;
 uint64_t gateHits = 0;
 static uint32_t pchist[1024]; /* --hist-pc: instr count per 64-byte bucket */
 if (opts->hist_pc) memset(pchist, 0, sizeof pchist);

 while (!stopRequested) {
 if (opts->max_cycles && totalCycles >= opts->max_cycles) break;

 {
 /* Use next-to-run PC for break comparison — getPC()
 * returns lastExecPC which is post-execution. */
 WalkerRegs _rr; walker_get_regs(&_rr);
 currentPC = _rr.pc;
 if (opts->dump_er && currentPC == opts->dump_er_pc) {
 gateHits++;
 /* Sample spread across the run: every 160th hit, up to 16. */
 if ((gateHits % 160) == 1 && dumpedEr < 16) {
 fprintf(stderr, " [er@0x%04X hit#%llu] F7B5=%02X e5hi=%04X "
 "er2=%08X er5=%08X rtcSeen=%llu\n",
 currentPC, (unsigned long long)gateHits,
 (unsigned)readMem(0xF7B5), (unsigned)(_rr.er[5] >> 16),
 _rr.er[2], _rr.er[5], (unsigned long long)rtcFires);
 dumpedEr++;
 }
 }
 }

 if (opts->trace_pc) ev_emit_pc(ev, "pc", currentPC);

 for (int ci = 0; ci < opts->count_pc_count; ci++)
 if (currentPC == opts->count_pcs[ci]) count_hits[ci]++;

 if (opts->hist_pc) pchist[currentPC >> 6]++;

 if (is_break(opts, currentPC)) {
 ev_emit_pc(ev, "break", currentPC);
 lastBreakPC = currentPC;
 exit_reason = 1;
 break;
 }

 /* A sleeping CPU repeats the same PC until the next IRQ — idle,
 * not a tight loop. Only sample awake PCs for stuck detection. */
 if (!isSleeping()) pcHist[pcHistIdx++ & 7] = currentPC;
 if (!opts->no_stuck && !isSleeping() && instrCount > 100 &&
 pcHist[0] == pcHist[4] && pcHist[1] == pcHist[5] &&
 pcHist[2] == pcHist[6] && pcHist[3] == pcHist[7] &&
 pcHist[0] == currentPC) {
 if (++stuckCount > 10000) {
 ev_emit(ev, "stuck",
 "\"pc\":\"0x%04X\",\"loop\":[\"0x%04X\",\"0x%04X\",\"0x%04X\",\"0x%04X\"]",
 currentPC, pcHist[0], pcHist[1], pcHist[2], pcHist[3]);
 exit_reason = 2;
 break;
 }
 } else {
 stuckCount = 0;
 }

 walker_preexec_hook();
 uint64_t cycleCount = 0;
 int err = runNextInstruction(&cycleCount);
 totalCycles += cycleCount;
 instrCount++;

 if (opts->watch_clear) {
 uint8_t cur = (uint8_t)readMem(0xF7B5);
 uint16_t epc = getPC(); /* PC just executed */
 if ((prev_f7b5 & 1u) && !(cur & 1u)) {
 int j = 0;
 for (; j < clr_n; j++) if (clr_pc[j] == epc) break;
 if (j == clr_n && clr_n < 32) { clr_pc[clr_n] = epc; clr_cnt[clr_n] = 0; clr_n++; }
 if (j < 32) clr_cnt[j]++;
 }
 if (!(prev_f7b5 & 1u) && (cur & 1u)) {
 int j = 0;
 for (; j < set_n; j++) if (set_pc[j] == epc) break;
 if (j == set_n && set_n < 32) { set_pc[set_n] = epc; set_cnt[set_n] = 0; set_n++; }
 if (j < 32) set_cnt[j]++;
 }
 prev_f7b5 = cur;
 }

 if (opts->rtc_enabled) {
 rtcAccum += cycleCount;
 while (rtcAccum >= RTC_QUARTER_CYCLES) {
 rtcAccum -= RTC_QUARTER_CYCLES;
 quarterRTCInterrupt();
 rtcFires++;
 ev_emit(ev, "rtc", NULL);
 }
 }

 if (err) {
 uint16_t errPC = getPC();
 ev_emit(ev, "error",
 "\"pc\":\"0x%04X\","
 "\"bytes\":\"%02X %02X %02X %02X %02X %02X %02X %02X\"",
 errPC,
 readMem(errPC+0), readMem(errPC+1), readMem(errPC+2), readMem(errPC+3),
 readMem(errPC+4), readMem(errPC+5), readMem(errPC+6), readMem(errPC+7));
 exit_reason = 3;
 break;
 }
 }

 if (stopRequested) {
 ev_emit(ev, "signal", "\"name\":\"SIGINT\"");
 exit_reason = 4;
 }

 { WalkerRegs _rr; walker_get_regs(&_rr); currentPC = _rr.pc; }

 ev_emit(ev, "done",
 "\"pc\":\"0x%04X\",\"instr\":%llu,\"steps\":%u,\"watts\":%u,"
 "\"reason\":\"%s\"",
 currentPC,
 (unsigned long long)instrCount,
 (unsigned)getWalkerSteps(), (unsigned)getWalkerWatts(),
 exit_reason == 0 ? "limit"
 : exit_reason == 1 ? "break"
 : exit_reason == 2 ? "stuck"
 : exit_reason == 3 ? "error"
 : "signal");

 /* Human summary on stderr — safe even when events go to stdout. */
 fprintf(stderr, "\npwdbg: summary\n");
 fprintf(stderr, " cycles: %llu\n", (unsigned long long)totalCycles);
 fprintf(stderr, " instr: %llu\n", (unsigned long long)instrCount);
 fprintf(stderr, " final PC: 0x%04X\n", currentPC);
 fprintf(stderr, " steps: %u watts: %u\n",
 (unsigned)getWalkerSteps(), (unsigned)getWalkerWatts());
 if (exit_reason == 1)
 fprintf(stderr, " break at 0x%04X\n", lastBreakPC);
 if (exit_reason == 2)
 fprintf(stderr, " stuck in loop at 0x%04X\n", currentPC);
 if (opts->rtc_enabled)
 fprintf(stderr, " rtc fires: %llu\n", (unsigned long long)rtcFires);
 for (int ci = 0; ci < opts->count_pc_count; ci++)
 fprintf(stderr, " count 0x%04X: %llu\n",
 opts->count_pcs[ci], (unsigned long long)count_hits[ci]);
 if (opts->hist_pc) {
 fprintf(stderr, " PC histogram (top 12 buckets of 64 bytes, instr count):\n");
 for (int rank = 0; rank < 12; rank++) {
 int mx = -1; uint32_t mv = 0;
 for (int b = 0; b < 1024; b++) if (pchist[b] > mv) { mv = pchist[b]; mx = b; }
 if (mx < 0 || mv == 0) break;
 fprintf(stderr, " 0x%04X-0x%04X: %u\n",
 (unsigned)(mx << 6), (unsigned)((mx << 6) + 63), mv);
 pchist[mx] = 0;
 }
 }
 if (opts->watch_clear) {
 fprintf(stderr, " F7B5 bit0 0->1 SETS by PC (top):\n");
 for (int a = 0; a < set_n; a++) {
 int mx = a;
 for (int b = a + 1; b < set_n; b++) if (set_cnt[b] > set_cnt[mx]) mx = b;
 if (mx != a) {
 uint16_t tp = set_pc[a]; set_pc[a] = set_pc[mx]; set_pc[mx] = tp;
 uint64_t tc = set_cnt[a]; set_cnt[a] = set_cnt[mx]; set_cnt[mx] = tc;
 }
 fprintf(stderr, " 0x%04X: %llu\n", set_pc[a], (unsigned long long)set_cnt[a]);
 }
 fprintf(stderr, " F7B5 bit0 1->0 CLEARS by PC (top):\n");
 /* simple selection sort of the small histogram, descending */
 for (int a = 0; a < clr_n; a++) {
 int mx = a;
 for (int b = a + 1; b < clr_n; b++) if (clr_cnt[b] > clr_cnt[mx]) mx = b;
 if (mx != a) {
 uint16_t tp = clr_pc[a]; clr_pc[a] = clr_pc[mx]; clr_pc[mx] = tp;
 uint64_t tc = clr_cnt[a]; clr_cnt[a] = clr_cnt[mx]; clr_cnt[mx] = tc;
 }
 fprintf(stderr, " 0x%04X: %llu\n", clr_pc[a], (unsigned long long)clr_cnt[a]);
 }
 }

 if (opts->lcd_ascii) {
 fputc('\n', stderr);
 lcd_dump_ascii(stderr);
 }
 if (opts->lcd_pgm) lcd_dump_pgm(opts->lcd_pgm);

 /* Persist EEPROM into workdir, then optionally copy it out. */
 saveEeprom();
 if (opts->save_eeprom)
 common_copy_eeprom(workdir, opts->save_eeprom);

 if (events_out && events_owned) fclose(events_out);
 common_cleanup_workdir(workdir, persist);

 return exit_reason == 3 ? 1 : 0;
}
