/* GizmondoDroid - save-state byte stream (same call saves and loads, by s->loading) */
#include "state.h"
#include <stdlib.h>
#include <string.h>

void st_bytes(StateIO *s, void *p, size_t n) {
    if (s->err) return;
    if (s->loading) {
        if (s->pos + n > s->len) { s->err = 1; return; }
        memcpy(p, s->buf + s->pos, n); s->pos += n;
    } else {
        if (s->pos + n > s->cap) {
            size_t nc = s->cap ? s->cap * 2 : 65536; while (nc < s->pos + n) nc *= 2;
            uint8_t *nb = realloc(s->buf, nc); if (!nb) { s->err = 1; return; }
            s->buf = nb; s->cap = nc;
        }
        memcpy(s->buf + s->pos, p, n); s->pos += n; if (s->pos > s->len) s->len = s->pos;
    }
}
