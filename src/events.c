#include "events.h"

#include <stdarg.h>

void ev_init(EventStream *s, FILE *out, const uint64_t *cycle,
 const uint16_t *pc, int instance) {
 s->out = out;
 s->cycle = cycle;
 s->pc = pc;
 s->instance = instance;
}

static void ev_header(EventStream *s, const char *kind) {
 uint64_t t = s->cycle ? *s->cycle : 0;
 if (s->instance) {
 fprintf(s->out, "{\"i\":%d,\"t\":%llu,\"ev\":\"%s\"",
 s->instance, (unsigned long long)t, kind);
 } else {
 fprintf(s->out, "{\"t\":%llu,\"ev\":\"%s\"",
 (unsigned long long)t, kind);
 }
}

void ev_emit(EventStream *s, const char *kind, const char *extra_fmt, ...) {
 if (!s || !s->out) return;
 ev_header(s, kind);
 if (extra_fmt && *extra_fmt) {
 fputc(',', s->out);
 va_list ap;
 va_start(ap, extra_fmt);
 vfprintf(s->out, extra_fmt, ap);
 va_end(ap);
 }
 fputs("}\n", s->out);
 fflush(s->out);
}

void ev_emit_pc(EventStream *s, const char *kind, uint16_t pc) {
 if (!s || !s->out) return;
 ev_header(s, kind);
 fprintf(s->out, ",\"pc\":\"0x%04X\"}\n", pc);
 fflush(s->out);
}

void ev_emit_byte(EventStream *s, const char *kind, uint8_t byte, uint16_t pc) {
 if (!s || !s->out) return;
 ev_header(s, kind);
 fprintf(s->out, ",\"byte\":\"0x%02X\",\"pc\":\"0x%04X\"}\n", byte, pc);
 fflush(s->out);
}
