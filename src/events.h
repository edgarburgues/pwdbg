#pragma once

#include <stdint.h>
#include <stdio.h>

/* JSON Lines event stream.
 *
 * One JSON object per line, flushed after each emit so external readers
 * (the AI) see events in real time. All events share the shape:
 * {"t":<cycle>,"ev":"<kind>",...extra...}
 *
 * A NULL EventStream* or NULL out drops events silently — call sites never
 * need to check before emitting. */

typedef struct EventStream {
 FILE *out; /* NULL disables emission */
 const uint64_t *cycle; /* live view of the total cycle counter */
 const uint16_t *pc; /* live view of getPC() (optional, may be NULL) */
 int instance; /* 0 = single; 1/2 = duo peer label */
} EventStream;

void ev_init(EventStream *s, FILE *out, const uint64_t *cycle,
 const uint16_t *pc, int instance);

/* Emit an event.
 * kind short identifier, literal ASCII, no quotes needed
 * extra_fmt printf-style format for extra key/value pairs
 * (inserted inside the JSON object, must start with a key,
 * no leading/trailing commas). Pass NULL for no extras. */
void ev_emit(EventStream *s, const char *kind, const char *extra_fmt, ...)
 __attribute__((format(printf, 3, 4)));

/* Convenience helpers used from hot paths where we want zero allocation. */
void ev_emit_pc(EventStream *s, const char *kind, uint16_t pc);
void ev_emit_byte(EventStream *s, const char *kind, uint8_t byte, uint16_t pc);
