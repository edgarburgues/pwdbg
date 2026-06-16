#include "ir.h"
#include "ir_bridge.h"
#include "irtrace.h"
#include "events.h"
#include "walker.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <sys/socket.h>
#include <unistd.h>

/* IR backend for pwdbg.
 *
 * Implements the subset of the SC16IS750 API that walker.c calls under
 * __3DS__. Behaviour depends on two optional hooks:
 *
 * - EventStream (via ir_stubs_set_events): every TX byte / RX edge
 * / RX poll is reported as a structured JSON event.
 *
 * - ir_bridge (via ir_bridge_attach, see ir_bridge.h): every TX byte
 * is written to the bridge fd; ir_recv_poll() reads pending bytes
 * from it. Used by `pwdbg duo` to link two walker instances. */

FILE *logFile = NULL; /* required extern by pokestroller source files */

static EventStream *ev = NULL;

void ir_stubs_set_events(EventStream *s) { ev = s; }

/* Generic hook-event emitter usable from walker_ext.c. Lives here so
 * it shares the same EventStream pointer as the IR events. */
void ev_log_hook(const char *kind, uint16_t pc) {
 ev_emit_pc(ev, kind, pc);
}

void ev_log_ir_dispatch(uint32_t pktSession, uint32_t sessionId,
 uint8_t cmd, uint8_t peerRole) {
 ev_emit(ev, "ir_dispatch",
 "\"cmd\":\"0x%02X\",\"pktSession\":\"0x%08X\","
 "\"ourSession\":\"0x%08X\",\"peerRole\":\"0x%02X\"",
 cmd, pktSession, sessionId, peerRole);
}

void ev_log_regs(const char *kind, uint16_t pc, uint32_t er0, uint32_t er1) {
 ev_emit(ev, kind,
 "\"pc\":\"0x%04X\",\"er0\":\"0x%08X\",\"er1\":\"0x%08X\"",
 pc, er0, er1);
}

/* --- cycle source (for tap stamps and scheduled delivery) ------------- */

static const uint64_t *cycle_ptr = NULL;

void ir_stubs_set_cycle_ptr(const uint64_t *p) { cycle_ptr = p; }

static uint64_t now_cycles(void) {
 if (cycle_ptr) return *cycle_ptr;
 if (ev && ev->cycle) return *ev->cycle;
 return 0;
}

/* --- TX tap ------------------------------------------------------------ *
 * Every byte the firmware shifts out of SCI3 is appended to the tap file
 * as one line: "<cycle> <hexbyte>" (cycle = this instance's total H8
 * cycles at TX-shift time; hexbyte = uppercase, no 0x). The file can be
 * replayed into another instance with `ir feed`, which re-bases the
 * stamps relative to the moment of the feed. */

static FILE *tap_file = NULL;

int ir_stubs_tap_open(const char *path) {
 if (tap_file) { fclose(tap_file); tap_file = NULL; }
 if (!path) return 0; /* "off" */
 tap_file = fopen(path, "w");
 return tap_file ? 0 : -1;
}

void ir_stubs_tap_close(void) {
 if (tap_file) { fclose(tap_file); tap_file = NULL; }
}

static void tap_byte(uint8_t b) {
 if (tap_file) {
 fprintf(tap_file, "%llu %02X\n", (unsigned long long)now_cycles(), b);
 fflush(tap_file);
 }
}

/* --- scheduled RX delivery --------------------------------------------- *
 * RX bytes are queued as CHUNKS (bursts). One chunk = one contiguous
 * packet burst: ir_recv_poll() serves at most one chunk per call, and
 * only once its due-cycle has arrived. This preserves the firmware's
 * packet-boundary detection (a gap between chunks = a gap between
 * polls = the ROM's inter-packet silence check).
 *
 * Sources: `ir inject` (due = now, or now+N with @+N), `ir feed` (due
 * from the tap stamps, re-based), and the ir_bridge socket (due = now +
 * configurable latency; latency 0 keeps today's immediate behavior). */

#define CHUNK_DATA_CAP 160 /* 8-byte header + 128 payload + slack */
#define CHUNK_RING_CAP 1024 /* a full peer session is hundreds of bursts */

typedef struct {
 uint64_t due;
 uint16_t len;
 uint16_t pos; /* partial-consumption cursor */
 uint8_t data[CHUNK_DATA_CAP];
} IrChunk;

static IrChunk chunk_ring[CHUNK_RING_CAP];
static size_t chunk_head = 0; /* next to serve */
static size_t chunk_count = 0;

static uint32_t bridge_latency_cycles = 0;

void ir_stubs_set_latency(uint32_t cycles) { bridge_latency_cycles = cycles; }

/* Framed bridge (pwdbg pair): instead of raw bytes, each TX burst goes
 * over the socket as [8B LE sender-cycle][2B LE len][payload]. The
 * receiver schedules the burst at sender_stamp + latency on its OWN
 * clock. Because lockstep bounds the two clocks' skew well below the
 * latency, the delivery cycle is a pure function of the sender's
 * (deterministic) TX cycle — host scheduling can no longer move it.
 * Raw mode (duo / --ir-listen sockets) is unchanged. */
static bool bridge_framed = false;

void ir_stubs_set_bridge_framed(int on) { bridge_framed = on ? true : false; }

/* Bridge debug trace (env PWDBG_BRIDGE_TRACE=1): logs every flush/recv/
 * parse/serve on stderr with the pid, to localise lost/late frames. */
static int bridge_trace = -1;
static int btrace(void) {
 if (bridge_trace < 0) {
 const char *e = getenv("PWDBG_BRIDGE_TRACE");
 bridge_trace = (e && *e && *e != '0') ? 1 : 0;
 }
 return bridge_trace;
}

static IrChunk *chunk_push(uint64_t due) {
 if (chunk_count >= CHUNK_RING_CAP) {
 ev_emit(ev, "ir_chunk_overflow", NULL);
 return NULL;
 }
 IrChunk *c = &chunk_ring[(chunk_head + chunk_count) % CHUNK_RING_CAP];
 chunk_count++;
 c->due = due;
 c->len = 0;
 c->pos = 0;
 return c;
}

/* Append bytes as one chunk due at `due` (splits if > CHUNK_DATA_CAP). */
static void chunks_add(const uint8_t *buf, size_t n, uint64_t due) {
 while (n > 0) {
 IrChunk *c = chunk_push(due);
 if (!c) return;
 size_t take = n < CHUNK_DATA_CAP ? n : CHUNK_DATA_CAP;
 for (size_t i = 0; i < take; i++) c->data[i] = buf[i];
 c->len = (uint16_t)take;
 buf += take;
 n -= take;
 }
}

void ir_stubs_inject_rx(const uint8_t *buf, size_t n) {
 chunks_add(buf, n, now_cycles());
}

void ir_stubs_inject_rx_at(const uint8_t *buf, size_t n, uint64_t due_abs) {
 chunks_add(buf, n, due_abs);
}

/* Load a tap file produced by `ir tap`.
 *
 * relative mode (abs=0): stamps are re-based — byte k is scheduled at
 * now + (stamp_k - stamp_0), i.e. the stream starts immediately.
 *
 * absolute mode (abs=1): byte k is scheduled at stamp_k on THIS
 * instance's own clock. Use when the tap was recorded from a lockstep
 * `pwdbg pair` run (both clocks aligned) and the replaying script is
 * identical to the live peer's — every byte then arrives at the same
 * cycle as it did live, making the replay cycle-deterministic. Feed
 * early (stamps still in the future); past-due chunks deliver at once.
 *
 * Bytes whose stamps are within FEED_GAP cycles of the previous byte
 * join the same chunk (same burst); a larger gap starts a new chunk,
 * preserving the recorded inter-packet silences. Returns the number of
 * bytes scheduled, or -1 on I/O error. */
#define FEED_GAP 1280 /* 4 byte-times at 320 cycles/byte */

long ir_stubs_feed_file(const char *path, int abs) {
 FILE *f = fopen(path, "r");
 if (!f) return -1;

 uint64_t base_now = now_cycles();
 uint64_t stamp0 = 0;
 uint64_t prev_stamp = 0;
 bool first = true;
 long total = 0;
 IrChunk *cur = NULL;

 char line[128];
 while (fgets(line, sizeof line, f)) {
 unsigned long long stamp;
 unsigned byte;
 if (sscanf(line, "%llu %x", &stamp, &byte) != 2) continue;
 if (first) { stamp0 = stamp; prev_stamp = stamp; first = false; }

 bool new_chunk = (cur == NULL) ||
 (stamp - prev_stamp > FEED_GAP) ||
 (cur->len >= CHUNK_DATA_CAP);
 if (new_chunk) {
 /* abs mode adds the configured latency, mirroring the live
 * pair's delivery (due = sender_stamp + latency): set
 * `ir latency` to the pair's effective latency (default
 * 4*quantum = 32768) before feeding for an exact replay. */
 uint64_t due = abs ? (uint64_t)stamp + bridge_latency_cycles
 : base_now + (stamp - stamp0);
 cur = chunk_push(due);
 if (!cur) break;
 }
 cur->data[cur->len++] = (uint8_t)byte;
 prev_stamp = stamp;
 total++;
 }
 fclose(f);
 return total;
}

/* TX packet accumulator.
 *
 * The ROM drives SCI3 in bursts: multiple bytes within one "packet",
 * then ~320 H8 cycles of silence that triggers ir_tx_flush_to_rx in
 * walker.c. Writing each byte to the socket individually destroys
 * the receiver's packet-boundary detection (the peer polls at its
 * own pace and may split a single burst across multiple polls, which
 * makes the ROM's 4-Timer-W-tick gap check see false boundaries
 * mid-packet, CRC fails, connection never settles).
 *
 * We buffer the burst locally and flush to the socket as one write()
 * on ir_tx_flush_to_rx / ir_tx_end — the receiver then reads the whole
 * packet atomically on a single recv(), exactly matching the real
 * IR bursts-with-silence-between-them model. */
#define TX_BATCH_CAP 144 /* 8-byte header + up to 128-byte payload + slack */
static uint8_t tx_batch[TX_BATCH_CAP];
static size_t tx_batch_len = 0;

static void tx_batch_flush(void) {
 if (tx_batch_len == 0) return;
 int fd = ir_bridge_fd();
 if (fd >= 0) {
 ssize_t n;
 size_t want;
 if (bridge_framed) {
 uint64_t stamp = now_cycles();
 uint8_t hdr[10];
 for (int i = 0; i < 8; i++) hdr[i] = (uint8_t)(stamp >> (8 * i));
 hdr[8] = (uint8_t)(tx_batch_len & 0xFF);
 hdr[9] = (uint8_t)((tx_batch_len >> 8) & 0xFF);
 uint8_t frame[10 + TX_BATCH_CAP];
 for (int i = 0; i < 10; i++) frame[i] = hdr[i];
 for (size_t i = 0; i < tx_batch_len; i++) frame[10 + i] = tx_batch[i];
 want = 10 + tx_batch_len;
 n = write(fd, frame, want);
 } else {
 want = tx_batch_len;
 n = write(fd, tx_batch, want);
 }
 if (btrace())
 fprintf(stderr, "[BR %d] flush want=%zu wrote=%zd t=%llu\n",
 (int)getpid(), want, n,
 (unsigned long long)now_cycles());
 if (n >= 0 && (size_t)n != want) {
 ev_emit(ev, "ir_bridge_err",
 "\"stage\":\"tx_flush_short\",\"want\":%zu,\"wrote\":%zd",
 want, n);
 if (btrace())
 fprintf(stderr, "[BR %d] SHORT WRITE %zd/%zu\n",
 (int)getpid(), n, want);
 }
 if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
 ev_emit(ev, "ir_bridge_err",
 "\"stage\":\"tx_flush\",\"errno\":%d", errno);
 }
 }
 tx_batch_len = 0;
}

/* --- SCI3 <-> IR surface called from walker.c ------------------------- */

void ir_tx_start(void) {
 ev_emit_pc(ev, "ir_tx_start", getPC());
}

void ir_tx_byte(uint8_t b) {
 ev_emit_byte(ev, "ir_tx", b, getPC());
 tap_byte(b);
 if (tx_batch_len < TX_BATCH_CAP) {
 tx_batch[tx_batch_len++] = b;
 } else {
 /* Overflow — flush what we have and drop the extra byte.
 * Shouldn't hit unless the ROM emits a packet > 144 bytes. */
 tx_batch_flush();
 ev_emit(ev, "ir_tx_overflow", NULL);
 }
}

void ir_tx_end(void) {
 tx_batch_flush();
 ev_emit_pc(ev, "ir_tx_end", getPC());
}

void ir_tx_flush_to_rx(void) {
 tx_batch_flush();
 ev_emit_pc(ev, "ir_tx_flush", getPC());
}

void ir_recv_start(void) {
 ev_emit_pc(ev, "ir_rx_start", getPC());
}

/* Drain the bridge into the chunk queue. Raw mode: one recv() = one
 * TX burst from the peer (the sender flushes whole bursts), so one
 * chunk = one packet, servable after the configured latency. Framed
 * mode (pwdbg pair): parse [stamp][len][payload] frames from a
 * staging buffer and schedule each at sender_stamp + latency.
 *
 * Called EAGERLY from the emulator's poll tick (every poll, not just
 * when rxBuf is empty) so frames are queued with their dues still in
 * the future — the serve time is then a pure function of the sender
 * stamp, independent of how busy the receiver's rxBuf was or of host
 * scheduling. (Late parse used to turn dues stale, making delivery
 * timing follow host I/O — the pair-stall bug.) */
void ir_recv_pump(void) {
 uint64_t now = now_cycles();
 int fd = ir_bridge_fd();
 if (fd >= 0 && !bridge_framed) {
 uint8_t tmp[CHUNK_DATA_CAP];
 for (;;) {
 ssize_t got = recv(fd, tmp, sizeof tmp, MSG_DONTWAIT);
 if (got > 0) {
 chunks_add(tmp, (size_t)got, now + bridge_latency_cycles);
 if ((size_t)got < sizeof tmp) break; /* burst drained */
 } else {
 if (got == 0) ev_emit(ev, "ir_bridge_eof", NULL);
 break; /* EAGAIN/EWOULDBLOCK: nothing pending — normal */
 }
 }
 } else if (fd >= 0) {
 static uint8_t fr_buf[4096];
 static size_t fr_len = 0;
 for (;;) {
 ssize_t got = recv(fd, fr_buf + fr_len, sizeof fr_buf - fr_len,
 MSG_DONTWAIT);
 if (got > 0) fr_len += (size_t)got;
 else {
 if (got == 0) ev_emit(ev, "ir_bridge_eof", NULL);
 break;
 }
 if (fr_len >= sizeof fr_buf) break;
 }
 /* parse complete frames */
 size_t off = 0;
 while (fr_len - off >= 10) {
 uint64_t stamp = 0;
 for (int i = 0; i < 8; i++)
 stamp |= (uint64_t)fr_buf[off + i] << (8 * i);
 size_t plen = (size_t)fr_buf[off + 8] |
 ((size_t)fr_buf[off + 9] << 8);
 if (fr_len - off - 10 < plen) break; /* incomplete payload */
 if (btrace())
 fprintf(stderr, "[BR %d] frame stamp=%llu len=%zu due=%llu now=%llu\n",
 (int)getpid(), (unsigned long long)stamp, plen,
 (unsigned long long)(stamp + bridge_latency_cycles),
 (unsigned long long)now);
 chunks_add(fr_buf + off + 10, plen,
 stamp + bridge_latency_cycles);
 off += 10 + plen;
 }
 if (off > 0) {
 for (size_t i = off; i < fr_len; i++) fr_buf[i - off] = fr_buf[i];
 fr_len -= off;
 }
 }
}

size_t ir_recv_poll(uint8_t *buf, size_t maxlen) {
 size_t n = 0;
 uint64_t now = now_cycles();

 ir_recv_pump();

 /* Serve at most ONE due chunk per poll (a poll boundary = the
 * firmware's inter-packet gap detector sees a silence). */
 if (chunk_count > 0) {
 IrChunk *c = &chunk_ring[chunk_head];
 if (c->due <= now) {
 size_t avail = (size_t)(c->len - c->pos);
 n = avail < maxlen ? avail : maxlen;
 for (size_t i = 0; i < n; i++) buf[i] = c->data[c->pos + i];
 c->pos += (uint16_t)n;
 if (btrace())
 fprintf(stderr, "[BR %d] serve len=%zu (chunk %u/%u) due=%llu now=%llu queued=%zu\n",
 (int)getpid(), n, c->pos, c->len,
 (unsigned long long)c->due, (unsigned long long)now,
 chunk_count);
 if (c->pos >= c->len) {
 chunk_head = (chunk_head + 1) % CHUNK_RING_CAP;
 chunk_count--;
 }
 }
 }

 if (n == 0) return 0;

 if (ev && ev->out) {
 fputs("", ev->out);
 /* serialise as a JSON array of hex bytes */
 uint64_t t = ev->cycle ? *ev->cycle : 0;
 if (ev->instance)
 fprintf(ev->out, "{\"i\":%d,\"t\":%llu,\"ev\":\"ir_rx\",\"bytes\":[",
 ev->instance, (unsigned long long)t);
 else
 fprintf(ev->out, "{\"t\":%llu,\"ev\":\"ir_rx\",\"bytes\":[",
 (unsigned long long)t);
 for (size_t i = 0; i < n; i++)
 fprintf(ev->out, "%s\"0x%02X\"", i ? "," : "", buf[i]);
 fprintf(ev->out, "],\"pc\":\"0x%04X\"}\n", getPC());
 fflush(ev->out);
 }
 return n;
}

void ir_recv_stop(void) {
 ev_emit_pc(ev, "ir_rx_stop", getPC());
}

uint64_t ir_get_blocked_ticks(void) { return 0; }

/* walker.c calls this on various SCI3 edges. pwdbg surfaces the richer
 * info via the JSON stream already, so the trace ring is a no-op. */
void irTracePush(uint8_t type, uint8_t data, uint16_t pc, uint32_t cycle) {
 (void)type; (void)data; (void)pc; (void)cycle;
}
