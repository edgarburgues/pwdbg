#pragma once

/* Shim mirroring 03-emulator-3ds/pokestroller/source/irtrace.h.
 * Keeps walker.c's irTracePush() calls valid on the CLI build;
 * pwdbg implements a no-op irTracePush and uses the richer JSON event
 * stream instead. */

#include <stdint.h>

enum IrTraceType {
 IR_TRACE_TX_BYTE = 'T',
 IR_TRACE_RX_BYTE = 'R',
 IR_TRACE_TE_ON = '+',
 IR_TRACE_TE_OFF = '-',
 IR_TRACE_RE_ON = '>',
 IR_TRACE_RE_OFF = '<',
 IR_TRACE_RX_POLL = 'P',
 IR_TRACE_ROLE = 'S',
};

void irTracePush(uint8_t type, uint8_t data, uint16_t pc, uint32_t cycle);
