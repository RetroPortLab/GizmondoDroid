/* GizmondoDroid - waveOut: guest buffers are mixed per frame into a ring the host pulls from */
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

uint32_t audio_rate = 44100;

#define RING 32768
static int16_t ring[RING * 2];
static atomic_uint ring_w, ring_r;      /* frame counters */

typedef struct Buf { uint32_t hdr, data, len, pos; struct Buf *next; } Buf;
typedef struct { int used, playing, paused; uint32_t rate, chans, bits, cb_type, cb, inst, h; Buf *head, *tail; uint64_t played_bytes; double frac; } Stream;
static Stream streams[4];

static Stream *strm(uint32_t h) { for (int i = 0; i < 4; i++) if (streams[i].used && streams[i].h == h) return &streams[i]; return NULL; }
static void api_waveOutGetNumDevs(void) { R(1); }
static void api_waveOutOpen(void) {   /* phwo, dev, pwfx, cb, inst, flags */
    uint32_t flags = A(5), wf = A(2);
    if (flags & 0x20 /* WAVE_FORMAT_QUERY */ ) { R(0); return; }
    for (int i = 0; i < 4; i++) if (!streams[i].used) {
        Stream *s = &streams[i]; memset(s, 0, sizeof *s);
        s->used = 1; s->playing = 1; s->chans = r16(wf + 2); s->rate = r32(wf + 4); s->bits = r16(wf + 14);
        if (!s->chans) s->chans = 2; if (!s->rate) s->rate = 22050; if (!s->bits) s->bits = 16;
        s->cb_type = flags & 0x70000; s->cb = A(3); s->inst = A(4);
        s->h = h_new(H_WAVE, s);
        if (A(0)) w32(A(0), s->h);
        if (s->cb_type == 0x30000 && s->cb) guest_call(s->cb, s->h, 0x3BB, s->inst, 0);
        R(0); return;
    }
    R(4);   /* MMSYSERR_ALLOCATED */
}
static void notify(Stream *s, uint32_t hdr) {
    if (s->cb_type == 0x30000 && s->cb) guest_call(s->cb, s->h, 0x3BD, s->inst, hdr);
    else if (s->cb_type == 0x10000 && s->cb) { extern void win_post_message(uint32_t, uint32_t, uint32_t, uint32_t); win_post_message(s->cb, 0x3BD, s->h, hdr); }
}
static void api_waveOutClose(void) {
    Stream *s = strm(A(0)); if (!s) { R(5); return; }
    if (s->cb_type == 0x30000 && s->cb) guest_call(s->cb, s->h, 0x3BC, s->inst, 0);
    for (Buf *b = s->head; b;) { Buf *n = b->next; free(b); b = n; }
    s->used = 0; h_close(s->h); R(0);
}
static void api_waveOutPrepareHeader(void) { w32(A(1) + 16, r32(A(1) + 16) | 2); R(0); }
static void api_waveOutUnprepareHeader(void) { w32(A(1) + 16, r32(A(1) + 16) & ~2u); R(0); }
static void api_waveOutWrite(void) {
    Stream *s = strm(A(0)); uint32_t hdr = A(1); if (!s) { R(5); return; }
    Buf *b = calloc(1, sizeof *b); b->hdr = hdr; b->data = r32(hdr); b->len = r32(hdr + 4);
    w32(hdr + 16, (r32(hdr + 16) & ~1u) | 0x10 | 2);
    if (s->tail) s->tail->next = b; else s->head = b;
    s->tail = b; R(0);
}
static void drain_all(Stream *s) {
    while (s->head) { Buf *b = s->head; s->head = b->next; if (!s->head) s->tail = NULL; w32(b->hdr + 16, (r32(b->hdr + 16) & ~0x10u) | 1); uint32_t h = b->hdr; free(b); notify(s, h); }
}
static void api_waveOutReset(void) { Stream *s = strm(A(0)); if (!s) { R(5); return; } drain_all(s); R(0); }
static void api_waveOutPause(void) { Stream *s = strm(A(0)); if (s) s->paused = 1; R(0); }
static void api_waveOutRestart(void) { Stream *s = strm(A(0)); if (s) s->paused = 0; R(0); }
static void api_waveOutGetPosition(void) {
    Stream *s = strm(A(0)); if (!s) { R(5); return; }
    uint32_t t = A(1), type = r32(t); uint32_t bpf = s->chans * s->bits / 8;
    if (type == 2) w32(t + 4, (uint32_t)(s->played_bytes / bpf)); else { w32(t, 4); w32(t + 4, (uint32_t)s->played_bytes); }
    R(0);
}
static void api_waveOutSetVolume(void) { R(0); }
static void api_waveOutGetVolume(void) { if (A(1)) w32(A(1), 0xFFFFFFFF); R(0); }
static void api_waveOutGetDevCapsW(void) { for (uint32_t i = 0; i < A(2); i += 4) w32(A(1) + i, 0); R(0); }
static void api_waveOutBreakLoop(void) { R(0); }
static void api_waveOutGetID(void) { if (A(1)) w32(A(1), 0); R(0); }
static void api_waveOutMessage(void) { R(0); }
static void api_zero(void) { R(0); }
static void api_PlaySoundW(void) { R(1); }

/* next stereo frame of one stream (resampled to audio_rate); returns 0 when out of data */
static int stream_frame(Stream *s, int *l, int *r) {
    while (s->head && s->head->pos >= s->head->len) {
        Buf *b = s->head; s->head = b->next; if (!s->head) s->tail = NULL;
        w32(b->hdr + 16, (r32(b->hdr + 16) & ~0x10u) | 1);
        uint32_t h = b->hdr; free(b); notify(s, h);
    }
    if (!s->head) return 0;
    Buf *b = s->head; uint32_t bpf = s->chans * s->bits / 8;
    uint32_t a = b->data + b->pos;
    int v[2] = { 0, 0 };
    for (uint32_t c = 0; c < s->chans && c < 2; c++) {
        if (s->bits == 16) v[c] = (int16_t)r16(a + 2 * c); else v[c] = ((int)r8(a + c) - 128) << 8;
    }
    if (s->chans == 1) v[1] = v[0];
    *l = v[0]; *r = v[1];
    s->frac += (double)s->rate / audio_rate;
    while (s->frac >= 1.0) { s->frac -= 1.0; b->pos += bpf; s->played_bytes += bpf; }
    return 1;
}

void audio_frame(int frames) {
    unsigned w = atomic_load(&ring_w), r = atomic_load(&ring_r);
    if (w - r > RING - (unsigned)frames - 1) return;   /* host is not draining: drop */
    for (int i = 0; i < frames; i++) {
        int L = 0, R_ = 0;
        for (int k = 0; k < 4; k++) {
            Stream *s = &streams[k]; int l, rr;
            if (s->used && !s->paused && stream_frame(s, &l, &rr)) { L += l; R_ += rr; }
        }
        if (L > 32767) L = 32767; if (L < -32768) L = -32768; if (R_ > 32767) R_ = 32767; if (R_ < -32768) R_ = -32768;
        ring[2 * ((w + i) % RING)] = (int16_t)L; ring[2 * ((w + i) % RING) + 1] = (int16_t)R_;
    }
    atomic_store(&ring_w, w + frames);
}
int audio_read(int16_t *out, int frames) {
    static int primed;
    unsigned w = atomic_load(&ring_w), r = atomic_load(&ring_r); int got = 0;
    /* after an underrun, wait until ~2 emulated frames are buffered again so playback resumes smoothly instead of crackling */
    if (!primed) { if (w - r < (unsigned)(audio_rate / 30)) { for (int i = 0; i < frames * 2; i++) out[i] = 0; return 0; } primed = 1; }
    if (w - r < (unsigned)frames) primed = 0;
    while (got < frames && r != w) { out[2 * got] = ring[2 * (r % RING)]; out[2 * got + 1] = ring[2 * (r % RING) + 1]; r++; got++; }
    atomic_store(&ring_r, r);
    for (int i = got; i < frames; i++) { out[2 * i] = 0; out[2 * i + 1] = 0; }
    return got;
}

#define E(n, f) { n, f }
static const ApiDef adefs[] = {
    E("waveOutGetNumDevs", api_waveOutGetNumDevs), E("waveOutOpen", api_waveOutOpen), E("waveOutClose", api_waveOutClose), E("waveOutPrepareHeader", api_waveOutPrepareHeader),
    E("waveOutUnprepareHeader", api_waveOutUnprepareHeader), E("waveOutWrite", api_waveOutWrite), E("waveOutReset", api_waveOutReset), E("waveOutPause", api_waveOutPause),
    E("waveOutRestart", api_waveOutRestart), E("waveOutGetPosition", api_waveOutGetPosition), E("waveOutSetVolume", api_waveOutSetVolume), E("waveOutGetVolume", api_waveOutGetVolume),
    E("waveOutGetDevCapsW", api_waveOutGetDevCapsW), E("waveOutBreakLoop", api_waveOutBreakLoop), E("waveOutGetID", api_waveOutGetID), E("waveOutMessage", api_waveOutMessage),
    E("waveInGetNumDevs", api_zero), E("midiOutGetNumDevs", api_zero), E("PlaySoundW", api_PlaySoundW), E("sndPlaySoundW", api_PlaySoundW),
};
void audio_init_api(void) {
    memset(streams, 0, sizeof streams); atomic_store(&ring_w, 0); atomic_store(&ring_r, 0);
    api_register(adefs, sizeof adefs / sizeof adefs[0]);
}
