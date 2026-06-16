#include "repl.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>

#include "common.h"
#include "events.h"
#include "ir_bridge.h"
#include "lcd.h"
#include "walker.h"
#include "walker_ext.h"

#define DEFAULT_ROM_DIR "/workspace/05-roms/pokewalker"
#define RTC_QUARTER_CYCLES 921600ULL
#define MAX_BREAKS 64
#define MAX_WATCHES 32

typedef struct Breakpoint { uint16_t pc; bool active; } Breakpoint;
typedef struct Watchpoint {
 uint16_t addr;
 uint8_t last;
 bool active;
 bool no_stop; /* emit event but don't halt execution */
} Watchpoint;

/* --- shared REPL state ------------------------------------------- */
typedef struct ReplState {
 uint64_t totalCycles;
 uint64_t totalInstrs;
 uint16_t currentPC; /* mirror for the EventStream */
 Breakpoint breaks[MAX_BREAKS];
 Watchpoint watches[MAX_WATCHES];
 bool rtc_enabled;
 bool ir_sniff;
 bool stuck_detect;
 uint64_t rtc_accum;
 EventStream ev_store;
 EventStream *ev;
} ReplState;

static volatile sig_atomic_t stopFlag = 0;
static void on_sigint(int sig) { (void)sig; stopFlag = 1; }

/* --- tiny parse helpers ------------------------------------------ */

static int parse_u32(const char *s, uint32_t *out) {
 if (!s || !*s) return -1;
 char *end = NULL;
 unsigned long v = strtoul(s, &end, 0);
 if (!end || *end != '\0' || v > 0xFFFFFFFFUL) return -1;
 *out = (uint32_t)v;
 return 0;
}
static int parse_u16(const char *s, uint16_t *out) {
 uint32_t v;
 if (parse_u32(s, &v) != 0 || v > 0xFFFF) return -1;
 *out = (uint16_t)v;
 return 0;
}
static int parse_u8(const char *s, uint8_t *out) {
 uint32_t v;
 if (parse_u32(s, &v) != 0 || v > 0xFF) return -1;
 *out = (uint8_t)v;
 return 0;
}

/* Hex-only byte parser for commands that take a byte stream (ir-send,
 * ir-inject, memw). Accepts "FC", "fc", "0xFC". Rejects decimals. */
static int parse_hexbyte(const char *s, uint8_t *out) {
 if (!s || !*s) return -1;
 if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
 char *end = NULL;
 unsigned long v = strtoul(s, &end, 16);
 if (!end || *end != '\0' || v > 0xFF) return -1;
 *out = (uint8_t)v;
 return 0;
}

/* split a line into whitespace-delimited tokens; modifies buf */
static int tokenize(char *buf, char **argv, int max) {
 int n = 0;
 char *s = buf;
 while (*s && n < max) {
 while (*s && isspace((unsigned char)*s)) s++;
 if (!*s) break;
 argv[n++] = s;
 while (*s && !isspace((unsigned char)*s)) s++;
 if (*s) *s++ = '\0';
 }
 return n;
}

/* --- inner stepping loop ----------------------------------------- */

typedef enum { SR_LIMIT, SR_BREAK, SR_STUCK, SR_ERROR, SR_WATCH, SR_SIGNAL,
 SR_INSTRS } StepReason;

typedef struct StepResult {
 StepReason reason;
 uint16_t hit_pc;
 int hit_watch;
} StepResult;

/* --- lockstep (pwdbg pair) ----------------------------------------------
 * When two paired instances free-run, their emulated clocks drift with
 * host scheduling and the walker protocol's reply windows are missed
 * nondeterministically. With lockstep, each instance exchanges a 1-byte
 * token with its peer every `lockstep_quantum` of its own cycles, so the
 * two emulated clocks never diverge by more than ~2 quanta. Peer exit
 * (EOF on the socket) silently disables lockstep — the survivor
 * free-runs to the end of its script. */
static int lockstep_fd = -1;
static uint32_t lockstep_quantum = 0;
static uint64_t lockstep_acc = 0;

void repl_set_lockstep(int fd, uint32_t quantum) {
 lockstep_fd = fd;
 lockstep_quantum = quantum ? quantum : 8192;
 lockstep_acc = 0;
}

static void lockstep_sync(uint64_t cycleCount) {
 if (lockstep_fd < 0) return;
 lockstep_acc += cycleCount;
 while (lockstep_acc >= lockstep_quantum) {
 lockstep_acc -= lockstep_quantum;
 uint8_t tok = 0x5A;
 if (write(lockstep_fd, &tok, 1) != 1 ||
 read(lockstep_fd, &tok, 1) != 1) {
 close(lockstep_fd);
 lockstep_fd = -1; /* peer gone — free-run */
 return;
 }
 }
}

static StepResult step_until(ReplState *st,
 uint64_t max_cycles, uint64_t max_instrs,
 bool trace_pc) {
 StepResult r = {SR_LIMIT, 0, -1};
 uint64_t startCycles = st->totalCycles;
 uint64_t startInstrs = st->totalInstrs;

 uint16_t pcHist[8] = {0};
 int pcHistIdx = 0;
 uint32_t stuck = 0;
 stopFlag = 0;

 while (!stopFlag) {
 if (max_cycles && (st->totalCycles - startCycles) >= max_cycles) break;
 if (max_instrs && (st->totalInstrs - startInstrs) >= max_instrs) {
 r.reason = SR_INSTRS; break;
 }

 {
 /* Use the "next instruction" PC for break comparison (gdb
 * semantics: stop BEFORE executing the break PC). getPC()
 * in walker.c returns lastExecPC, which is post-execution. */
 WalkerRegs _rr; walker_get_regs(&_rr);
 st->currentPC = _rr.pc;
 }
 if (trace_pc) ev_emit_pc(st->ev, "pc", st->currentPC);

 /* breakpoints */
 for (int i = 0; i < MAX_BREAKS; i++) {
 if (st->breaks[i].active && st->breaks[i].pc == st->currentPC) {
 ev_emit_pc(st->ev, "break", st->currentPC);
 r.reason = SR_BREAK; r.hit_pc = st->currentPC;
 return r;
 }
 }

 if (st->stuck_detect && !isSleeping()) {
 /* A sleeping CPU repeats the same PC until the next IRQ —
 * that's idle, not a tight loop. Only sample awake PCs. */
 pcHist[pcHistIdx++ & 7] = st->currentPC;
 if ((st->totalInstrs - startInstrs) > 100 &&
 pcHist[0] == pcHist[4] && pcHist[1] == pcHist[5] &&
 pcHist[2] == pcHist[6] && pcHist[3] == pcHist[7] &&
 pcHist[0] == st->currentPC) {
 if (++stuck > 10000) {
 ev_emit(st->ev, "stuck",
 "\"pc\":\"0x%04X\",\"loop\":[\"0x%04X\",\"0x%04X\",\"0x%04X\",\"0x%04X\"]",
 st->currentPC,
 pcHist[0], pcHist[1], pcHist[2], pcHist[3]);
 r.reason = SR_STUCK; r.hit_pc = st->currentPC;
 return r;
 }
 } else stuck = 0;
 }

 walker_preexec_hook();
 uint64_t cycleCount = 0;
 int err = runNextInstruction(&cycleCount);
 st->totalCycles += cycleCount;
 st->totalInstrs++;
 lockstep_sync(cycleCount);

 if (st->rtc_enabled) {
 st->rtc_accum += cycleCount;
 while (st->rtc_accum >= RTC_QUARTER_CYCLES) {
 st->rtc_accum -= RTC_QUARTER_CYCLES;
 quarterRTCInterrupt();
 ev_emit(st->ev, "rtc", NULL);
 }
 }

 if (err) {
 uint16_t errPC = getPC();
 ev_emit(st->ev, "error",
 "\"pc\":\"0x%04X\",\"bytes\":\"%02X %02X %02X %02X %02X %02X %02X %02X\"",
 errPC,
 walker_mem_read(errPC+0), walker_mem_read(errPC+1),
 walker_mem_read(errPC+2), walker_mem_read(errPC+3),
 walker_mem_read(errPC+4), walker_mem_read(errPC+5),
 walker_mem_read(errPC+6), walker_mem_read(errPC+7));
 r.reason = SR_ERROR; r.hit_pc = errPC;
 return r;
 }

 /* watchpoints — polled after each instruction */
 for (int i = 0; i < MAX_WATCHES; i++) {
 if (!st->watches[i].active) continue;
 uint8_t v = walker_mem_read(st->watches[i].addr);
 if (v != st->watches[i].last) {
 ev_emit(st->ev, "watch",
 "\"idx\":%d,\"addr\":\"0x%04X\",\"old\":\"0x%02X\","
 "\"new\":\"0x%02X\",\"pc\":\"0x%04X\"",
 i, st->watches[i].addr, st->watches[i].last, v,
 getPC());
 st->watches[i].last = v;
 if (!st->watches[i].no_stop) {
 r.reason = SR_WATCH; r.hit_pc = getPC(); r.hit_watch = i;
 return r;
 }
 }
 }
 }

 if (stopFlag) { r.reason = SR_SIGNAL; stopFlag = 0; }
 return r;
}

static const char *reason_name(StepReason r) {
 switch (r) {
 case SR_LIMIT: return "limit";
 case SR_INSTRS: return "instrs";
 case SR_BREAK: return "break";
 case SR_STUCK: return "stuck";
 case SR_ERROR: return "error";
 case SR_WATCH: return "watch";
 case SR_SIGNAL: return "signal";
 }
 return "?";
}

/* --- commands ---------------------------------------------------- */

static void cmd_help(void) {
 puts("commands:");
 puts(" step [N] run N instructions (default 1)");
 puts(" run [CYCLES] run CYCLES cycles (or until break/err/sig)");
 puts(" c|cont|continue alias for 'run' with no limit");
 puts(" reg print registers");
 puts(" reg set R VALUE R=0..7 (ER), 8 (PC), 9 (CCR)");
 puts(" mem ADDR [LEN] hex dump (default 64 bytes)");
 puts(" memw ADDR BYTE [BYTE...] write bytes to RAM/MMIO");
 puts(" eeprom ADDR [LEN] dump EEPROM region");
 puts(" eepromw ADDR BYTE [BYTE...] write raw EEPROM (bypasses SPI emu)");
 puts(" break add PC set breakpoint");
 puts(" break rm PC clear breakpoint");
 puts(" break list");
 puts(" watch add ADDR watch memory byte, stop on change");
 puts(" watch trace ADDR watch memory byte, emit event but keep running");
 puts(" watch rm IDX");
 puts(" watch list");
 puts(" lcd print LCD as ASCII");
 puts(" lcd pgm FILE save LCD as PGM");
 puts(" ir tap FILE|off dump every TX byte: '<cycle> <hex>' lines");
 puts(" ir inject [@+N] HEX... queue RX bytes (now, or in N cycles)");
 puts(" ir feed FILE [abs] replay a tap file (re-based, or abs stamps)");
 puts(" ir latency N delay peer-bridge bytes by N cycles");
 puts(" ir-inject HEX... feed bytes into LOCAL RX queue (no peer)");
 puts(" ir-send HEX... send bytes to peer over the IR bridge");
 puts(" ir-recv [N] read up to N bytes from bridge (nonblock)");
 puts(" ir-wait N [TIMEOUT_MS] block until N bytes arrive (default 5000ms)");
 puts(" sleep MS sleep for MS milliseconds (script sync)");
 puts(" ir-sniff on|off toggle IR events");
 puts(" key ENTER|LEFT|RIGHT queue a walker button press");
 puts(" stuck on|off toggle repeating-PC 'stuck' detector");
 puts(" peer-patch on|off patch identity checks (for peer-play testing)");
 puts(" force-slave on|off force this peer to be SLAVE on any SYN received");
 puts(" rtc fire one quarter-second RTC tick");
 puts(" rtc auto on|off enable/disable automatic RTC ticks");
 puts(" snapshot save PATH");
 puts(" snapshot load PATH");
 puts(" info show cycles/instrs/pc/steps/watts");
 puts(" quit|exit|q");
}

static void cmd_info(ReplState *st) {
 WalkerRegs r; walker_get_regs(&r);
 printf("cycles=%llu instrs=%llu pc=0x%04X steps=%u watts=%u sleep=%d\n",
 (unsigned long long)st->totalCycles,
 (unsigned long long)st->totalInstrs,
 r.pc, getWalkerSteps(), getWalkerWatts(), r.sleep);
}

static void cmd_reg(int argc, char **argv) {
 if (argc >= 1 && !strcmp(argv[0], "set")) {
 if (argc < 3) { puts("usage: reg set R VALUE (R=0..7 ER, 8 PC, 9 CCR)"); return; }
 uint32_t idx;
 if (parse_u32(argv[1], &idx) != 0 || idx > 9) { puts("bad R (0..9)"); return; }
 uint32_t v;
 if (parse_u32(argv[2], &v) != 0) { puts("bad VALUE"); return; }
 walker_set_reg(idx, v);
 printf("reg %u set to 0x%08X\n", idx, v);
 return;
 }
 WalkerRegs r; walker_get_regs(&r);
 printf("PC=0x%04X CCR=0x%02X [%c%c%c%c%c%c%c%c] sleep=%d\n",
 r.pc, r.ccr,
 (r.ccr & 0x80) ? 'I' : '-',
 (r.ccr & 0x40) ? 'U' : '-',
 (r.ccr & 0x20) ? 'H' : '-',
 (r.ccr & 0x10) ? 'U' : '-',
 (r.ccr & 0x08) ? 'N' : '-',
 (r.ccr & 0x04) ? 'Z' : '-',
 (r.ccr & 0x02) ? 'V' : '-',
 (r.ccr & 0x01) ? 'C' : '-',
 r.sleep);
 for (int i = 0; i < 8; i++) {
 printf("ER%d=0x%08X%s", i, r.er[i], (i == 3 || i == 7) ? "\n" : " ");
 }
}

static void hex_dump(uint32_t addr, uint32_t len, uint8_t (*rd)(uint32_t)) {
 for (uint32_t off = 0; off < len; off += 16) {
 printf("%08X:", addr + off);
 uint32_t line = len - off < 16 ? len - off : 16;
 for (uint32_t i = 0; i < line; i++)
 printf(" %02X", rd(addr + off + i));
 for (uint32_t i = line; i < 16; i++) printf(" ");
 printf(" ");
 for (uint32_t i = 0; i < line; i++) {
 uint8_t c = rd(addr + off + i);
 putchar((c >= 0x20 && c < 0x7f) ? c : '.');
 }
 putchar('\n');
 }
}

static uint8_t rd_mem(uint32_t a) { return walker_mem_read((uint16_t)a); }
static uint8_t rd_eeprom(uint32_t a) { return walker_eeprom_read(a); }

static void cmd_mem(int argc, char **argv) {
 if (argc < 1) { puts("usage: mem ADDR [LEN]"); return; }
 uint16_t addr; uint32_t len = 64;
 if (parse_u16(argv[0], &addr) != 0) { puts("bad ADDR"); return; }
 if (argc >= 2 && parse_u32(argv[1], &len) != 0) { puts("bad LEN"); return; }
 if (len > 0x10000) len = 0x10000;
 hex_dump(addr, len, rd_mem);
}

static void cmd_memw(int argc, char **argv) {
 if (argc < 2) { puts("usage: memw ADDR BYTE [BYTE...]"); return; }
 uint16_t addr;
 if (parse_u16(argv[0], &addr) != 0) { puts("bad ADDR"); return; }
 for (int i = 1; i < argc; i++) {
 uint8_t b;
 if (parse_hexbyte(argv[i], &b) != 0) { puts("bad byte"); return; }
 walker_mem_write(addr + (uint16_t)(i - 1), b);
 }
 printf("wrote %d bytes at 0x%04X\n", argc - 1, addr);
}

static void cmd_eeprom(int argc, char **argv) {
 if (argc < 1) { puts("usage: eeprom ADDR [LEN]"); return; }
 uint32_t addr, len = 256;
 if (parse_u32(argv[0], &addr) != 0) { puts("bad ADDR"); return; }
 if (argc >= 2 && parse_u32(argv[1], &len) != 0) { puts("bad LEN"); return; }
 if (addr >= walker_eeprom_size()) { puts("addr OOB"); return; }
 if (addr + len > walker_eeprom_size()) len = walker_eeprom_size() - addr;
 hex_dump(addr, len, rd_eeprom);
}

static void cmd_eepromw(int argc, char **argv) {
 if (argc < 2) { puts("usage: eepromw ADDR BYTE [BYTE...]"); return; }
 uint32_t addr;
 if (parse_u32(argv[0], &addr) != 0) { puts("bad ADDR"); return; }
 for (int i = 1; i < argc; i++) {
 uint8_t b;
 if (parse_hexbyte(argv[i], &b) != 0) { puts("bad byte"); return; }
 walker_eeprom_write(addr + (uint32_t)(i - 1), b);
 }
 printf("wrote %d bytes at eeprom 0x%04X\n", argc - 1, addr);
}

static void cmd_break(ReplState *st, int argc, char **argv) {
 if (argc < 1) { puts("usage: break add|rm|list ..."); return; }
 if (!strcmp(argv[0], "list")) {
 int n = 0;
 for (int i = 0; i < MAX_BREAKS; i++)
 if (st->breaks[i].active)
 printf(" [%d] 0x%04X\n", i, st->breaks[i].pc), n++;
 if (!n) puts(" (none)");
 return;
 }
 if (argc < 2) { puts("usage: break add|rm PC"); return; }
 uint16_t pc;
 if (parse_u16(argv[1], &pc) != 0) { puts("bad PC"); return; }
 if (!strcmp(argv[0], "add")) {
 for (int i = 0; i < MAX_BREAKS; i++) {
 if (!st->breaks[i].active) {
 st->breaks[i].active = true;
 st->breaks[i].pc = pc;
 printf("break [%d] added at 0x%04X\n", i, pc);
 return;
 }
 }
 puts("break table full");
 } else if (!strcmp(argv[0], "rm")) {
 for (int i = 0; i < MAX_BREAKS; i++)
 if (st->breaks[i].active && st->breaks[i].pc == pc) {
 st->breaks[i].active = false;
 printf("break removed at 0x%04X\n", pc);
 return;
 }
 puts("no break at that PC");
 } else {
 puts("usage: break add|rm|list");
 }
}

static void cmd_watch(ReplState *st, int argc, char **argv) {
 if (argc < 1) { puts("usage: watch add|rm|list ..."); return; }
 if (!strcmp(argv[0], "list")) {
 int n = 0;
 for (int i = 0; i < MAX_WATCHES; i++)
 if (st->watches[i].active)
 printf(" [%d] 0x%04X (last=0x%02X)\n",
 i, st->watches[i].addr, st->watches[i].last), n++;
 if (!n) puts(" (none)");
 return;
 }
 if (argc < 2) { puts("usage: watch add ADDR | watch rm IDX"); return; }
 if (!strcmp(argv[0], "add") || !strcmp(argv[0], "trace")) {
 uint16_t addr;
 if (parse_u16(argv[1], &addr) != 0) { puts("bad ADDR"); return; }
 bool trace = !strcmp(argv[0], "trace");
 for (int i = 0; i < MAX_WATCHES; i++) {
 if (!st->watches[i].active) {
 st->watches[i].active = true;
 st->watches[i].addr = addr;
 st->watches[i].last = walker_mem_read(addr);
 st->watches[i].no_stop = trace;
 printf("watch [%d]%s at 0x%04X (initial=0x%02X)\n",
 i, trace ? " (trace)" : "", addr, st->watches[i].last);
 return;
 }
 }
 puts("watch table full");
 } else if (!strcmp(argv[0], "rm")) {
 uint32_t idx;
 if (parse_u32(argv[1], &idx) != 0 || idx >= MAX_WATCHES) {
 puts("bad IDX"); return;
 }
 st->watches[idx].active = false;
 printf("watch [%u] removed\n", idx);
 } else {
 puts("usage: watch add|rm|list");
 }
}

static void cmd_step(ReplState *st, int argc, char **argv) {
 uint64_t n = 1;
 if (argc >= 1) {
 uint32_t v;
 if (parse_u32(argv[0], &v) != 0) { puts("bad N"); return; }
 n = v;
 }
 StepResult r = step_until(st, 0, n, false);
 WalkerRegs rr; walker_get_regs(&rr);
 printf("stopped: %s at PC=0x%04X (%llu cycles, %llu instrs elapsed)\n",
 reason_name(r.reason), rr.pc,
 (unsigned long long)st->totalCycles,
 (unsigned long long)st->totalInstrs);
}

static void cmd_run(ReplState *st, int argc, char **argv) {
 uint64_t cycles = 0;
 if (argc >= 1) {
 uint32_t v;
 if (parse_u32(argv[0], &v) != 0) { puts("bad CYCLES"); return; }
 cycles = v;
 }
 StepResult r = step_until(st, cycles, 0, false);
 WalkerRegs rr; walker_get_regs(&rr);
 printf("stopped: %s at PC=0x%04X (%llu cycles, %llu instrs elapsed)\n",
 reason_name(r.reason), rr.pc,
 (unsigned long long)st->totalCycles,
 (unsigned long long)st->totalInstrs);
}

static void cmd_lcd(int argc, char **argv) {
 if (argc >= 2 && !strcmp(argv[0], "pgm")) {
 if (lcd_dump_pgm(argv[1]) == 0)
 printf("wrote %s\n", argv[1]);
 return;
 }
 if (argc >= 1 && !strcmp(argv[0], "mem")) {
 /* Hex dump the raw lcd.memory[] buffer. Useful when fillVideo-
 * Buffer's currentBuffer toggle confuses the high-level dumps —
 * lets us see whether the SSU emulation actually wrote any
 * non-zero pixel bytes. */
 size_t limit = walker_lcd_mem_size();
 if (argc >= 2) {
 unsigned long u = strtoul(argv[1], NULL, 0);
 if (u && u < limit) limit = u;
 }
 size_t nonzero = 0;
 for (size_t i = 0; i < limit; i++) if (walker_lcd_mem_read(i)) nonzero++;
 printf("lcd.memory: size=%zu non-zero=%zu currentBuffer=%u "
 "currentPage=%u currentColumn=%u startLine=%u set=%u active=%u\n",
 walker_lcd_mem_size(), nonzero,
 walker_lcd_current_buffer(),
 walker_lcd_current_page(),
 walker_lcd_current_column(),
 (unsigned)lcdDisplayStartLine(),
 (unsigned)lcdStartLineSet(),
 (unsigned)lcdStartLineActive());
 for (size_t i = 0; i < limit; i += 16) {
 printf("%04zX:", i);
 for (size_t j = 0; j < 16 && i + j < limit; j++)
 printf(" %02X", walker_lcd_mem_read(i + j));
 putchar('\n');
 }
 return;
 }
 lcd_dump_ascii(stdout);
}

static void cmd_ir_inject(int argc, char **argv) {
 if (argc < 1) { puts("usage: ir-inject HEX [HEX...]"); return; }
 uint8_t buf[256];
 if (argc > 256) argc = 256;
 for (int i = 0; i < argc; i++) {
 if (parse_hexbyte(argv[i], &buf[i]) != 0) { puts("bad hex byte"); return; }
 }
 ir_stubs_inject_rx(buf, argc);
 printf("injected %d bytes into local RX queue\n", argc);
}

/* `ir` — the peer-harness primitives (IR_PEER_HARNESS.md):
 * ir tap FILE | ir tap off cycle-stamped dump of every TX byte
 * ir inject [@+N] HEX.. queue RX bytes (now, or now+N cycles)
 * ir feed FILE replay a tap file (stamps re-based)
 * ir latency N delay peer-bridge bytes by N cycles */
static void cmd_ir(ReplState *st, int argc, char **argv) {
 if (argc < 1) {
 puts("usage: ir tap FILE|off | ir inject [@+N] HEX.. | "
 "ir feed FILE | ir latency N");
 return;
 }
 if (!strcmp(argv[0], "tap")) {
 if (argc < 2) { puts("usage: ir tap FILE|off"); return; }
 if (!strcmp(argv[1], "off")) {
 ir_stubs_tap_close();
 puts("ir tap off");
 } else if (ir_stubs_tap_open(argv[1]) == 0) {
 printf("ir tap -> %s\n", argv[1]);
 } else {
 printf("cannot open %s\n", argv[1]);
 }
 return;
 }
 if (!strcmp(argv[0], "inject")) {
 int i = 1;
 uint64_t delay = 0;
 if (i < argc && argv[i][0] == '@' && argv[i][1] == '+') {
 delay = strtoull(argv[i] + 2, NULL, 0);
 i++;
 }
 uint8_t buf[256];
 int n = 0;
 for (; i < argc && n < 256; i++, n++) {
 if (parse_hexbyte(argv[i], &buf[n]) != 0) { puts("bad hex byte"); return; }
 }
 if (n == 0) { puts("usage: ir inject [@+N] HEX.."); return; }
 if (delay) {
 ir_stubs_inject_rx_at(buf, (size_t)n, st->totalCycles + delay);
 printf("scheduled %d bytes at +%llu cycles\n",
 n, (unsigned long long)delay);
 } else {
 ir_stubs_inject_rx(buf, (size_t)n);
 printf("injected %d bytes\n", n);
 }
 return;
 }
 if (!strcmp(argv[0], "feed")) {
 if (argc < 2) { puts("usage: ir feed FILE [abs]"); return; }
 int abs = (argc >= 3 && !strcmp(argv[2], "abs"));
 long n = ir_stubs_feed_file(argv[1], abs);
 if (n < 0) printf("cannot read %s\n", argv[1]);
 else printf("feed: scheduled %ld bytes from %s%s\n",
 n, argv[1], abs ? " (absolute stamps)" : "");
 return;
 }
 if (!strcmp(argv[0], "latency")) {
 if (argc < 2) { puts("usage: ir latency CYCLES"); return; }
 ir_stubs_set_latency((uint32_t)strtoul(argv[1], NULL, 0));
 printf("ir latency = %s cycles\n", argv[1]);
 return;
 }
 puts("unknown ir subcommand (tap|inject|feed|latency)");
}

static void cmd_ir_recv(int argc, char **argv) {
 int fd = ir_bridge_fd();
 if (fd < 0) { puts("no bridge attached"); return; }
 size_t max = 64;
 if (argc >= 1) {
 uint32_t v;
 if (parse_u32(argv[0], &v) != 0) { puts("bad N"); return; }
 max = v < 256 ? v : 256;
 }
 uint8_t buf[256];
 ssize_t n = recv(fd, buf, max, MSG_DONTWAIT);
 if (n < 0) {
 if (errno == EAGAIN || errno == EWOULDBLOCK) puts("(empty)");
 else perror("recv");
 return;
 }
 if (n == 0) { puts("(peer closed)"); return; }
 printf("got %zd bytes:", n);
 for (int i = 0; i < n; i++) printf(" %02X", buf[i]);
 putchar('\n');
}

/* Block until at least N bytes are pending on the bridge, or TIMEOUT_MS
 * elapse. Useful in scripts that need to rendezvous with a peer. */
static void cmd_ir_wait(int argc, char **argv) {
 if (argc < 1) { puts("usage: ir-wait N [TIMEOUT_MS=5000]"); return; }
 int fd = ir_bridge_fd();
 if (fd < 0) { puts("no bridge attached"); return; }
 uint32_t want;
 if (parse_u32(argv[0], &want) != 0) { puts("bad N"); return; }
 uint32_t timeout_ms = 5000;
 if (argc >= 2 && parse_u32(argv[1], &timeout_ms) != 0) { puts("bad TIMEOUT"); return; }

 size_t accum = 0;
 uint8_t buf[256];
 uint32_t waited = 0;
 while (accum < want && waited < timeout_ms) {
 ssize_t n = recv(fd, buf + accum, sizeof(buf) - accum, MSG_DONTWAIT);
 if (n > 0) accum += (size_t)n;
 else if (n == 0) { puts("(peer closed)"); return; }
 else if (errno != EAGAIN && errno != EWOULDBLOCK) { perror("recv"); return; }
 if (accum >= want) break;
 struct timespec ts = {0, 20 * 1000 * 1000}; /* 20ms */
 nanosleep(&ts, NULL);
 waited += 20;
 }
 if (accum < want) {
 printf("(timeout, got %zu bytes)", accum);
 } else {
 printf("got %zu bytes:", accum);
 }
 for (size_t i = 0; i < accum; i++) printf(" %02X", buf[i]);
 putchar('\n');
}

static void cmd_sleep(int argc, char **argv) {
 if (argc < 1) { puts("usage: sleep MS"); return; }
 uint32_t ms;
 if (parse_u32(argv[0], &ms) != 0) { puts("bad MS"); return; }
 struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000 * 1000 };
 nanosleep(&ts, NULL);
}

static void cmd_ir_send(int argc, char **argv) {
 if (argc < 1) { puts("usage: ir-send HEX [HEX...]"); return; }
 int fd = ir_bridge_fd();
 if (fd < 0) { puts("no bridge attached"); return; }
 uint8_t buf[256];
 if (argc > 256) argc = 256;
 for (int i = 0; i < argc; i++) {
 if (parse_hexbyte(argv[i], &buf[i]) != 0) { puts("bad hex byte"); return; }
 }
 ssize_t n = send(fd, buf, argc, 0);
 if (n < 0) { perror("send"); return; }
 printf("sent %zd bytes to peer over bridge\n", n);
}

static void cmd_ir_sniff(ReplState *st, int argc, char **argv) {
 if (argc < 1) { puts("usage: ir-sniff on|off"); return; }
 if (!strcmp(argv[0], "on")) { st->ir_sniff = true; ir_stubs_set_events(st->ev); puts("ir sniff on"); }
 else if (!strcmp(argv[0], "off")) { st->ir_sniff = false; ir_stubs_set_events(NULL); puts("ir sniff off"); }
 else puts("usage: ir-sniff on|off");
}

static void cmd_peer_patch(int argc, char **argv) {
 if (argc < 1) { puts("usage: peer-patch on|off"); return; }
 if (!strcmp(argv[0], "on")) { walker_set_peer_patch(true); puts("peer-patch on"); }
 else if (!strcmp(argv[0], "off")) { walker_set_peer_patch(false); puts("peer-patch off"); }
 else puts("usage: peer-patch on|off");
}

static void cmd_force_slave(int argc, char **argv) {
 if (argc < 1) { puts("usage: force-slave on|off"); return; }
 if (!strcmp(argv[0], "on")) { walker_set_force_slave(true); puts("force-slave on"); }
 else if (!strcmp(argv[0], "off")) { walker_set_force_slave(false); puts("force-slave off"); }
 else puts("usage: force-slave on|off");
}

static void cmd_stuck(ReplState *st, int argc, char **argv) {
 if (argc < 1) {
 printf("stuck-detect is %s\n", st->stuck_detect ? "on" : "off");
 return;
 }
 if (!strcmp(argv[0], "on")) { st->stuck_detect = true; puts("stuck on"); }
 else if (!strcmp(argv[0], "off")) { st->stuck_detect = false; puts("stuck off"); }
 else puts("usage: stuck on|off");
}

static void cmd_key(int argc, char **argv) {
 if (argc < 1) { puts("usage: key ENTER|LEFT|RIGHT"); return; }
 uint8_t mask = 0;
 if (!strcasecmp(argv[0], "ENTER")) mask = ENTER;
 else if (!strcasecmp(argv[0], "LEFT")) mask = LEFT;
 else if (!strcasecmp(argv[0], "RIGHT")) mask = RIGHT;
 else { puts("key must be ENTER, LEFT, or RIGHT"); return; }
 setKeys(mask);
 printf("queued key: %s (0x%02X)\n", argv[0], mask);
}

static void cmd_rtc(ReplState *st, int argc, char **argv) {
 if (argc >= 1 && !strcmp(argv[0], "auto")) {
 if (argc < 2) { puts("usage: rtc auto on|off"); return; }
 if (!strcmp(argv[1], "on")) { st->rtc_enabled = true; puts("rtc auto on"); }
 else if (!strcmp(argv[1], "off")) { st->rtc_enabled = false; puts("rtc auto off"); }
 else puts("usage: rtc auto on|off");
 return;
 }
 quarterRTCInterrupt();
 ev_emit(st->ev, "rtc", "\"source\":\"manual\"");
 puts("rtc tick fired");
}

static void cmd_snapshot(int argc, char **argv) {
 if (argc < 2) { puts("usage: snapshot save|load PATH"); return; }
 if (!strcmp(argv[0], "save")) {
 void *buf = NULL; size_t len = 0;
 if (walker_save_state(&buf, &len) != 0) { puts("save failed"); return; }
 FILE *f = fopen(argv[1], "wb");
 if (!f) { perror(argv[1]); free(buf); return; }
 if (fwrite(buf, 1, len, f) != len) { perror("write"); fclose(f); free(buf); return; }
 fclose(f); free(buf);
 printf("snapshot saved (%zu bytes)\n", len);
 } else if (!strcmp(argv[0], "load")) {
 FILE *f = fopen(argv[1], "rb");
 if (!f) { perror(argv[1]); return; }
 fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
 void *buf = malloc(len);
 if (fread(buf, 1, len, f) != (size_t)len) { perror("read"); fclose(f); free(buf); return; }
 fclose(f);
 int err = walker_load_state(buf, len);
 free(buf);
 if (err) printf("snapshot load failed (%d)\n", err);
 else puts("snapshot loaded");
 } else puts("usage: snapshot save|load PATH");
}

/* --- dispatch ---------------------------------------------------- */

static bool dispatch(ReplState *st, char *line) {
 char *argv[16];
 int argc = tokenize(line, argv, 16);
 if (argc == 0) return true;

 const char *c = argv[0];
 char **a = argv + 1;
 int ac = argc - 1;

 if (!strcmp(c, "help") || !strcmp(c, "?") || !strcmp(c, "h")) cmd_help();
 else if (!strcmp(c, "info")) cmd_info(st);
 else if (!strcmp(c, "reg")) cmd_reg(ac, a);
 else if (!strcmp(c, "mem")) cmd_mem(ac, a);
 else if (!strcmp(c, "memw")) cmd_memw(ac, a);
 else if (!strcmp(c, "eeprom")) cmd_eeprom(ac, a);
 else if (!strcmp(c, "eepromw")) cmd_eepromw(ac, a);
 else if (!strcmp(c, "break") || !strcmp(c, "b")) cmd_break(st, ac, a);
 else if (!strcmp(c, "watch") || !strcmp(c, "w")) cmd_watch(st, ac, a);
 else if (!strcmp(c, "step") || !strcmp(c, "s")) cmd_step(st, ac, a);
 else if (!strcmp(c, "run") || !strcmp(c, "r")) cmd_run(st, ac, a);
 else if (!strcmp(c, "c") || !strcmp(c, "cont") ||
 !strcmp(c, "continue")) cmd_run(st, 0, NULL);
 else if (!strcmp(c, "draw-trace")) {
 /* draw-trace on|off [pc] — log each lcd_draw_image (debug). */
 int on = (ac >= 1 && !strcmp(a[0], "on"));
 unsigned pcOpt = (ac >= 2) ? (unsigned)strtoul(a[1], NULL, 0) : 0u;
 walkerSetDrawTrace(on, pcOpt);
 printf("draw-trace %s%s\n", on ? "on" : "off",
 pcOpt ? " (custom PC)" : "");
 }
 else if (!strcmp(c, "lcd")) cmd_lcd(ac, a);
 else if (!strcmp(c, "ir")) cmd_ir(st, ac, a);
 else if (!strcmp(c, "ir-inject")) cmd_ir_inject(ac, a);
 else if (!strcmp(c, "ir-send")) cmd_ir_send(ac, a);
 else if (!strcmp(c, "ir-recv")) cmd_ir_recv(ac, a);
 else if (!strcmp(c, "ir-wait")) cmd_ir_wait(ac, a);
 else if (!strcmp(c, "sleep")) cmd_sleep(ac, a);
 else if (!strcmp(c, "ir-sniff")) cmd_ir_sniff(st, ac, a);
 else if (!strcmp(c, "key")) cmd_key(ac, a);
 else if (!strcmp(c, "stuck")) cmd_stuck(st, ac, a);
 else if (!strcmp(c, "peer-patch")) cmd_peer_patch(ac, a);
 else if (!strcmp(c, "force-slave")) cmd_force_slave(ac, a);
 else if (!strcmp(c, "rtc")) cmd_rtc(st, ac, a);
 else if (!strcmp(c, "snapshot")) cmd_snapshot(ac, a);
 else if (!strcmp(c, "quit") || !strcmp(c, "exit") || !strcmp(c, "q")) return false;
 else printf("unknown command '%s' (help for list)\n", c);

 return true;
}

/* --- main loop --------------------------------------------------- */

static int repl_setup_bridge(const ReplOpts *opts) {
 if (!opts->ir_listen && !opts->ir_connect) return 0;
 if (opts->ir_listen && opts->ir_connect) {
 fputs("pwdbg repl: --ir-listen and --ir-connect are mutually exclusive\n", stderr);
 return -1;
 }

 int s = socket(AF_UNIX, SOCK_STREAM, 0);
 if (s < 0) { perror("socket"); return -1; }
 struct sockaddr_un a = {0};
 a.sun_family = AF_UNIX;

 if (opts->ir_listen) {
 strncpy(a.sun_path, opts->ir_listen, sizeof(a.sun_path) - 1);
 unlink(opts->ir_listen); /* best-effort remove stale */
 if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) { perror("bind"); close(s); return -1; }
 if (listen(s, 1) != 0) { perror("listen"); close(s); return -1; }
 fprintf(stderr, "pwdbg repl: waiting for peer on %s ...\n", opts->ir_listen);
 int c = accept(s, NULL, NULL);
 close(s);
 if (c < 0) { perror("accept"); return -1; }
 fprintf(stderr, "pwdbg repl: peer connected\n");
 ir_bridge_attach(c);
 return 0;
 }

 strncpy(a.sun_path, opts->ir_connect, sizeof(a.sun_path) - 1);
 if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) {
 perror("connect"); close(s); return -1;
 }
 fprintf(stderr, "pwdbg repl: connected to peer at %s\n", opts->ir_connect);
 ir_bridge_attach(s);
 return 0;
}

int repl_main(const ReplOpts *opts) {
 char workdir[PATH_MAX];
 bool persist = false;
 const char *rom_dir = opts->rom_dir ? opts->rom_dir : DEFAULT_ROM_DIR;

 /* Capture the launch directory before chdir — the custom ROM symbol table
 * (build/syms.nm) is resolved relative to it. */
 char origcwd[PATH_MAX];
 if (!getcwd(origcwd, sizeof origcwd)) origcwd[0] = '\0';

 if (common_setup_workdir(rom_dir, opts->workdir, workdir, &persist) != 0)
 return 2;
 if (chdir(workdir) != 0) { perror("chdir"); common_cleanup_workdir(workdir, persist); return 2; }

 if (repl_setup_bridge(opts) != 0) {
 common_cleanup_workdir(workdir, persist);
 return 2;
 }

 bool events_owned = false;
 FILE *events_out = common_open_events(opts->events_spec, &events_owned);

 ReplState st = {0};
 st.rtc_enabled = true;
 st.stuck_detect = true;
 st.ir_sniff = opts->ir_sniff;

 if (events_out) {
 ev_init(&st.ev_store, events_out, &st.totalCycles, &st.currentPC,
 opts->instance);
 st.ev = &st.ev_store;
 if (st.ir_sniff) ir_stubs_set_events(st.ev);
 }

 signal(SIGINT, on_sigint);

 initWalker();
 st.currentPC = getPC();
 common_resolve_v2_keypoll_hook(getEntry(), origcwd);
 ir_stubs_set_cycle_ptr(&st.totalCycles);

 ev_emit(st.ev, "start",
 "\"pc\":\"0x%04X\",\"mode\":\"repl\"", st.currentPC);

 fprintf(stderr, "pwdbg repl — entry 0x%04X, workdir %s%s\n",
 st.currentPC, workdir, persist ? " (persistent)" : "");
 fputs("type 'help' for commands, 'quit' to leave.\n", stderr);

 FILE *in = stdin;
 bool is_tty = isatty(fileno(stdin));
 if (opts->script) {
 in = fopen(opts->script, "r");
 if (!in) { perror(opts->script); common_cleanup_workdir(workdir, persist); return 2; }
 is_tty = false;
 }

 char line[1024];
 for (;;) {
 if (is_tty) { fputs("pwdbg> ", stdout); fflush(stdout); }
 if (!fgets(line, sizeof line, in)) break;
 if (!dispatch(&st, line)) break;
 }

 if (in != stdin) fclose(in);
 saveEeprom();
 ev_emit(st.ev, "done",
 "\"pc\":\"0x%04X\",\"instr\":%llu,\"reason\":\"quit\"",
 getPC(), (unsigned long long)st.totalInstrs);

 if (events_out && events_owned) fclose(events_out);
 common_cleanup_workdir(workdir, persist);
 return 0;
}
