#pragma once

/* Shim header seen by walker.c when compiling pwdbg.
 * walker.c does `#include "ir.h"` under __3DS__; this provides only the
 * subset of the SC16IS750 driver API that walker.c actually calls, so the
 * real 3DS implementation in 03-emulator-3ds/pokestroller/source/ir.c
 * does not need to link. pwdbg provides its own implementations in
 * ir_stubs.c and bridges to the JSON event stream / peer socket. */

#include <stdint.h>
#include <stddef.h>

void ir_tx_start(void);
void ir_tx_byte(uint8_t b);
void ir_tx_end(void);
void ir_tx_flush_to_rx(void);
void ir_recv_start(void);
void ir_recv_pump(void); /* drain peer socket into the scheduled chunk
 * queue NOW (eager parse) — keeps frame dues
 * meaningful even while rxBuf is mid-drain */
size_t ir_recv_poll(uint8_t *buf, size_t maxlen);
void ir_recv_stop(void);
uint64_t ir_get_blocked_ticks(void);
