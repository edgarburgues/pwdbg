#include "duo.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common.h"
#include "ir_bridge.h"
#include "repl.h"
#include "run.h"

/* Run one peer after fork. The ir_bridge is already attached to our
 * end of the socketpair; we just build the RunOpts and delegate to
 * run_main. Runs to completion and exits the child process. */
static int peer_exec(int instance, const DuoOpts *o, int ir_fd) {
 ir_bridge_attach(ir_fd);

 RunOpts r = (RunOpts){
 .rom_dir = o->rom_dir,
 .workdir = instance == 1 ? o->workdir_a : o->workdir_b,
 .events_spec = instance == 1 ? o->events_a : o->events_b,
 .save_eeprom = instance == 1 ? o->save_eeprom_a : o->save_eeprom_b,
 .max_cycles = o->max_cycles,
 .break_count = instance == 1 ? o->break_a_count : o->break_b_count,
 .trace_pc = o->trace_pc,
 .lcd_ascii = false, /* both peers' stderr goes to parent TTY — keep quiet */
 .rtc_enabled = o->rtc_enabled,
 .instance = instance,
 };
 const uint16_t *src = instance == 1 ? o->break_a : o->break_b;
 for (int i = 0; i < r.break_count; i++) r.break_pcs[i] = src[i];

 int rc = run_main(&r);
 close(ir_fd);
 return rc;
}

/* --- pwdbg pair: two scripted REPL peers over a socketpair ------------- */

static int pair_exec(int instance, const PairOpts *o, int ir_fd, int sync_fd) {
 ir_bridge_attach(ir_fd);
 uint32_t q = o->quantum ? o->quantum : 8192;
 if (sync_fd >= 0) {
 repl_set_lockstep(sync_fd, q);
 /* Framed bridge + sender-stamp scheduling: delivery cycle is a
 * function of the sender's deterministic TX cycle, not of host
 * scheduling. Latency must exceed the lockstep skew bound (~2q)
 * so the due-cycle is always still in the receiver's future. */
 ir_stubs_set_bridge_framed(1);
 ir_stubs_set_latency(o->latency > 4 * q ? o->latency : 4 * q);
 } else {
 ir_stubs_set_latency(o->latency);
 }

 /* Per-instance stdout so the two scripts' output doesn't interleave. */
 const char *log = instance == 1
 ? (o->log_a ? o->log_a : "pair_a.log")
 : (o->log_b ? o->log_b : "pair_b.log");
 if (!freopen(log, "w", stdout)) {
 fprintf(stderr, "pwdbg pair: cannot open %s\n", log);
 return 2;
 }
 setvbuf(stdout, NULL, _IOLBF, 0);

 ReplOpts r = (ReplOpts){
 .rom_dir = instance == 1 ? o->rom_a : o->rom_b,
 .workdir = instance == 1 ? o->workdir_a : o->workdir_b,
 .events_spec = instance == 1 ? o->events_a : o->events_b,
 .script = instance == 1 ? o->script_a : o->script_b,
 .instance = instance,
 };
 int rc = repl_main(&r);
 close(ir_fd);
 return rc;
}

int pair_main(const PairOpts *o) {
 if (!o->script_a || !o->script_b) {
 fputs("pwdbg pair: --script-a and --script-b are required\n", stderr);
 return 2;
 }
 int sv[2];
 if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
 perror("pwdbg pair: socketpair");
 return 2;
 }
 /* second pair for the lockstep tokens (kept blocking, unlike the IR fd) */
 int ls[2] = { -1, -1 };
 if (!o->no_lockstep && socketpair(AF_UNIX, SOCK_STREAM, 0, ls) != 0) {
 perror("pwdbg pair: lockstep socketpair");
 return 2;
 }

 pid_t pid_a = fork();
 if (pid_a < 0) { perror("fork"); return 2; }
 if (pid_a == 0) {
 close(sv[1]);
 if (ls[1] >= 0) close(ls[1]);
 _exit(pair_exec(1, o, sv[0], ls[0]) == 0 ? 0 : 1);
 }

 pid_t pid_b = fork();
 if (pid_b < 0) {
 perror("fork");
 kill(pid_a, SIGTERM);
 waitpid(pid_a, NULL, 0);
 return 2;
 }
 if (pid_b == 0) {
 close(sv[0]);
 if (ls[0] >= 0) close(ls[0]);
 _exit(pair_exec(2, o, sv[1], ls[1]) == 0 ? 0 : 1);
 }

 close(sv[0]);
 close(sv[1]);
 if (ls[0] >= 0) { close(ls[0]); close(ls[1]); }

 fprintf(stderr, "pwdbg pair: A pid=%d (%s), B pid=%d (%s)\n",
 (int)pid_a, o->script_a, (int)pid_b, o->script_b);

 int status_a = 0, status_b = 0;
 waitpid(pid_a, &status_a, 0);
 waitpid(pid_b, &status_b, 0);

 int exit_a = WIFEXITED(status_a) ? WEXITSTATUS(status_a) : 128;
 int exit_b = WIFEXITED(status_b) ? WEXITSTATUS(status_b) : 128;
 fprintf(stderr, "pwdbg pair: A exit=%d, B exit=%d\n", exit_a, exit_b);
 return (exit_a || exit_b) ? 1 : 0;
}

int duo_main(const DuoOpts *o) {
 int sv[2];
 if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
 perror("pwdbg duo: socketpair");
 return 2;
 }

 pid_t pid_a = fork();
 if (pid_a < 0) { perror("fork"); return 2; }
 if (pid_a == 0) {
 /* peer A */
 close(sv[1]);
 int rc = peer_exec(1, o, sv[0]);
 _exit(rc == 0 ? 0 : 1);
 }

 pid_t pid_b = fork();
 if (pid_b < 0) {
 perror("fork");
 kill(pid_a, SIGTERM);
 waitpid(pid_a, NULL, 0);
 return 2;
 }
 if (pid_b == 0) {
 /* peer B */
 close(sv[0]);
 int rc = peer_exec(2, o, sv[1]);
 _exit(rc == 0 ? 0 : 1);
 }

 /* parent closes both ends; the children have dup'd references */
 close(sv[0]);
 close(sv[1]);

 fprintf(stderr, "pwdbg duo: peer A pid=%d, peer B pid=%d\n",
 (int)pid_a, (int)pid_b);

 int status_a = 0, status_b = 0;
 waitpid(pid_a, &status_a, 0);
 waitpid(pid_b, &status_b, 0);

 int exit_a = WIFEXITED(status_a) ? WEXITSTATUS(status_a) : 128;
 int exit_b = WIFEXITED(status_b) ? WEXITSTATUS(status_b) : 128;

 fprintf(stderr, "pwdbg duo: peer A exit=%d, peer B exit=%d\n",
 exit_a, exit_b);
 return (exit_a || exit_b) ? 1 : 0;
}
