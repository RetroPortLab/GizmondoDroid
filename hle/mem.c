/* GizmondoDroid - guest memory: sparse page table, heap, VirtualAlloc area */
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#define NPAGES (GM_LIMIT >> 12)
static uint8_t *pages[NPAGES];
static uint8_t kdata[0x4000];
int gm_trace_unmapped = 1;
static int unmapped_logs;

#define HEAP_BASE 0x04000000u
#define HEAP_SIZE 0x06000000u
#define VIRT_BASE 0x0A000000u
#define VIRT_END  0x0E800000u

static struct { void *p; size_t n; } chunks[8192]; static int nchunks;
static void *anon(size_t n) {
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return NULL;
    if (nchunks < 8192) { chunks[nchunks].p = p; chunks[nchunks].n = n; nchunks++; }
    return p;
}

int gm_range_free(uint32_t addr, uint32_t size) {
    if (addr >= GM_LIMIT || size > GM_LIMIT - addr) return 0;
    for (uint32_t p = addr >> 12; p < (addr + size + 0xFFF) >> 12; p++) if (pages[p]) return 0;
    return 1;
}

int gm_map(uint32_t addr, uint32_t size) {
    addr &= ~0xFFFu; size = (size + 0xFFF) & ~0xFFFu;
    if (!size || !gm_range_free(addr, size)) return 0;
    uint8_t *h = anon(size);
    if (!h) return 0;
    for (uint32_t i = 0; i < size >> 12; i++) pages[(addr >> 12) + i] = h + (i << 12);
    return 1;
}

uint8_t *gm_ptr(uint32_t a) {
    if (a < GM_LIMIT) { uint8_t *p = pages[a >> 12]; return p ? p + (a & 0xFFF) : NULL; }
    if (a >= KDATA_BASE) return kdata + (a - KDATA_BASE);
    return NULL;
}

/* ---- bus for the CPU core: identity mapped, no MMU */
uint8_t *bus_page(uint32_t pa, int write) {
    (void)write;
    if (pa < GM_LIMIT) return pages[pa >> 12];
    if (pa >= KDATA_BASE) return kdata + ((pa - KDATA_BASE) & ~0xFFFu);
    return NULL;
}
uint32_t bus_io_read(uint32_t pa, int size) {
    if (gm_trace_unmapped && unmapped_logs < 40) { unmapped_logs++; hle_log("unmapped read%d %08x pc=%08x lr=%08x", size, pa, cpu.r[15], cpu.r[14]); }
    return 0;
}
void bus_io_write(uint32_t pa, uint32_t v, int size) {
    if (gm_trace_unmapped && unmapped_logs < 40) { unmapped_logs++; hle_log("unmapped write%d %08x=%08x pc=%08x lr=%08x", size, pa, v, cpu.r[15], cpu.r[14]); }
}

uint32_t r32(uint32_t a) { uint8_t *p = gm_ptr(a); if (!p) return 0; uint32_t v; memcpy(&v, p, 4); return v; }
uint16_t r16(uint32_t a) { uint8_t *p = gm_ptr(a); if (!p) return 0; uint16_t v; memcpy(&v, p, 2); return v; }
uint8_t  r8(uint32_t a)  { uint8_t *p = gm_ptr(a); return p ? *p : 0; }
void w32(uint32_t a, uint32_t v) { uint8_t *p = gm_ptr(a); if (p) memcpy(p, &v, 4); }
void w16(uint32_t a, uint16_t v) { uint8_t *p = gm_ptr(a); if (p) memcpy(p, &v, 2); }
void w8(uint32_t a, uint8_t v)   { uint8_t *p = gm_ptr(a); if (p) *p = v; }

void gm_read(void *dst, uint32_t a, uint32_t n) {
    uint8_t *d = dst;
    while (n) { uint8_t *p = gm_ptr(a); uint32_t k = 0x1000 - (a & 0xFFF); if (k > n) k = n;
        if (p) memcpy(d, p, k); else memset(d, 0, k); d += k; a += k; n -= k; }
}
void gm_write(uint32_t a, const void *src, uint32_t n) {
    const uint8_t *s = src;
    while (n) { uint8_t *p = gm_ptr(a); uint32_t k = 0x1000 - (a & 0xFFF); if (k > n) k = n;
        if (p) memcpy(p, s, k); s += k; a += k; n -= k; }
}

int gs_read(uint32_t a, char *out, int max) {
    int i = 0;
    while (i < max - 1) { uint8_t c = r8(a + i); if (!c) break; out[i++] = (char)c; }
    out[i] = 0; return i;
}
int gw_len(uint32_t a) { int n = 0; while (r16(a + 2 * n)) n++; return n; }
int gw_read(uint32_t a, char *out, int max) {
    int o = 0;
    for (;;) {
        uint32_t c = r16(a); a += 2;
        if (!c) break;
        if (c >= 0xD800 && c < 0xDC00) { uint32_t d = r16(a); a += 2; c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); }
        if (c < 0x80) { if (o + 1 >= max) break; out[o++] = (char)c; }
        else if (c < 0x800) { if (o + 2 >= max) break; out[o++] = 0xC0 | (c >> 6); out[o++] = 0x80 | (c & 63); }
        else if (c < 0x10000) { if (o + 3 >= max) break; out[o++] = 0xE0 | (c >> 12); out[o++] = 0x80 | ((c >> 6) & 63); out[o++] = 0x80 | (c & 63); }
        else { if (o + 4 >= max) break; out[o++] = 0xF0 | (c >> 18); out[o++] = 0x80 | ((c >> 12) & 63); out[o++] = 0x80 | ((c >> 6) & 63); out[o++] = 0x80 | (c & 63); }
    }
    out[o] = 0; return o;
}
void gw_write(uint32_t a, const char *s, int maxchars) {
    int n = 0;
    while (*s && n < maxchars - 1) {
        uint32_t c = (uint8_t)*s++;
        if (c >= 0xF0 && s[0] && s[1] && s[2]) { c = ((c & 7) << 18) | ((s[0] & 63) << 12) | ((s[1] & 63) << 6) | (s[2] & 63); s += 3; }
        else if (c >= 0xE0 && s[0] && s[1]) { c = ((c & 15) << 12) | ((s[0] & 63) << 6) | (s[1] & 63); s += 2; }
        else if (c >= 0xC0 && s[0]) { c = ((c & 31) << 6) | (s[0] & 63); s += 1; }
        if (c >= 0x10000) { c -= 0x10000; w16(a + 2 * n++, 0xD800 + (c >> 10)); if (n >= maxchars - 1) break; w16(a + 2 * n++, 0xDC00 + (c & 1023)); }
        else w16(a + 2 * n++, (uint16_t)c);
    }
    w16(a + 2 * n, 0);
}
uint32_t gs_new(const char *s) { uint32_t a = gm_alloc((uint32_t)strlen(s) + 1); if (a) gm_write(a, s, (uint32_t)strlen(s) + 1); return a; }
uint32_t gw_new(const char *s) { uint32_t n = (uint32_t)strlen(s) + 1; uint32_t a = gm_alloc(n * 2); if (a) gw_write(a, s, (int)n); return a; }

/* ---- heap: boundary-tag allocator over a fixed arena, metadata kept in guest memory.
 * block: [size|used][prev_size][payload...]; free blocks keep next/prev links in the payload. */
static uint8_t *hp;                /* host pointer to arena base */
static uint32_t bins[32];          /* free list heads (guest addresses of block starts, 0 = empty) */
#define HB(a)   ((uint32_t *)(hp + ((a) - HEAP_BASE)))
#define BSZ(a)  (HB(a)[0] & ~7u)
#define BUSED(a) (HB(a)[0] & 1u)
#define BPREV(a) (HB(a)[1])
#define FNEXT(a) (HB(a)[2])
#define FPREV(a) (HB(a)[3])

static int bin_of(uint32_t sz) { return 31 - __builtin_clz(sz); }
static void fl_add(uint32_t a) {
    int b = bin_of(BSZ(a)); FNEXT(a) = bins[b]; FPREV(a) = 0;
    if (bins[b]) FPREV(bins[b]) = a;
    bins[b] = a;
}
static void fl_del(uint32_t a) {
    int b = bin_of(BSZ(a));
    if (FPREV(a)) FNEXT(FPREV(a)) = FNEXT(a); else bins[b] = FNEXT(a);
    if (FNEXT(a)) FPREV(FNEXT(a)) = FPREV(a);
}
static int in_heap(uint32_t a) { return a >= HEAP_BASE && a < HEAP_BASE + HEAP_SIZE; }

static void heap_init(void) {
    gm_map(HEAP_BASE, HEAP_SIZE);
    hp = gm_ptr(HEAP_BASE);
    memset(bins, 0, sizeof bins);
    /* one free block spanning the arena, with a used sentinel word at the end */
    uint32_t sz = HEAP_SIZE - 16;
    HB(HEAP_BASE)[0] = sz; HB(HEAP_BASE)[1] = 0;
    uint32_t end = HEAP_BASE + sz;
    HB(end)[0] = 8 | 1; HB(end)[1] = sz;
    fl_add(HEAP_BASE);
}

uint32_t gm_alloc(uint32_t n) {
    if (n > HEAP_SIZE / 2) return 0;
    uint32_t need = (n + 8 + 7) & ~7u; if (need < 24) need = 24;
    uint32_t blk = 0;
    int b = bin_of(need);
    for (uint32_t a = bins[b]; a; a = FNEXT(a)) if (BSZ(a) >= need) { blk = a; break; }
    for (int i = b + 1; !blk && i < 32; i++) if (bins[i]) blk = bins[i];
    if (!blk) return 0;
    fl_del(blk);
    uint32_t sz = BSZ(blk);
    if (sz - need >= 24) {
        uint32_t rest = blk + need, rsz = sz - need;
        HB(blk)[0] = need | 1;
        HB(rest)[0] = rsz; HB(rest)[1] = need;
        uint32_t nx = rest + rsz; HB(nx)[1] = rsz;
        fl_add(rest);
    } else HB(blk)[0] = sz | 1;
    return blk + 8;
}

void gm_free(uint32_t p) {
    if (!p || !in_heap(p)) return;
    uint32_t a = p - 8;
    if (!BUSED(a)) return;
    uint32_t sz = BSZ(a);
    uint32_t nx = a + sz;
    if (!BUSED(nx)) { fl_del(nx); sz += BSZ(nx); }
    if (BPREV(a) && !BUSED(a - BPREV(a))) { uint32_t pv = a - BPREV(a); fl_del(pv); sz += BSZ(pv); a = pv; }
    HB(a)[0] = sz;
    HB(a + sz)[1] = sz;
    fl_add(a);
}

uint32_t gm_size(uint32_t p) { return (p && in_heap(p)) ? BSZ(p - 8) - 8 : 0; }

uint32_t gm_realloc(uint32_t p, uint32_t n) {
    if (!p) return gm_alloc(n);
    if (!in_heap(p)) return 0;
    uint32_t old = gm_size(p);
    if (n <= old) return p;
    uint32_t q = gm_alloc(n);
    if (!q) return 0;
    memcpy(gm_ptr(q), gm_ptr(p), old);
    gm_free(p);
    return q;
}

/* ---- VirtualAlloc area: page-granular extents */
typedef struct { uint32_t a, n; } Ext;
static Ext ext[4096]; static int next_;
static int ext_cmp(const void *x, const void *y) { uint32_t a = ((const Ext *)x)->a, b = ((const Ext *)y)->a; return a < b ? -1 : a > b; }

uint32_t gm_valloc(uint32_t addr, uint32_t size) {
    size = (size + 0xFFF) & ~0xFFFu; if (!size) size = 0x1000;
    if (addr) {
        for (int i = 0; i < next_; i++) if (addr >= ext[i].a && addr < ext[i].a + ext[i].n) return addr;   /* commit inside a reservation */
        if (in_heap(addr)) return addr;
        if (addr >= VIRT_BASE && addr + size <= VIRT_END) {
            for (int i = 0; i < next_; i++) if (addr < ext[i].a + ext[i].n && addr + size > ext[i].a) return 0;
            if (next_ >= 4096) return 0;
            ext[next_].a = addr; ext[next_].n = size; next_++; qsort(ext, next_, sizeof ext[0], ext_cmp);
            return addr;
        }
        return 0;
    }
    static int mapped;
    if (!mapped) { gm_map(VIRT_BASE, VIRT_END - VIRT_BASE); mapped = 1; }
    uint32_t cur = VIRT_BASE;
    for (int i = 0; i <= next_; i++) {
        uint32_t lim = i < next_ ? ext[i].a : VIRT_END;
        if (lim - cur >= size) {
            if (next_ >= 4096) return 0;
            ext[next_].a = cur; ext[next_].n = size; next_++; qsort(ext, next_, sizeof ext[0], ext_cmp);
            memset(gm_ptr(cur), 0, size);
            return cur;
        }
        if (i < next_) cur = ext[i].a + ext[i].n;
    }
    return 0;
}

int gm_vfree(uint32_t addr) {
    for (int i = 0; i < next_; i++) if (ext[i].a == addr) {
        memmove(&ext[i], &ext[i + 1], (next_ - i - 1) * sizeof ext[0]); next_--; return 1;
    }
    return 0;
}

void gm_init(void) {
    for (int i = 0; i < nchunks; i++) munmap(chunks[i].p, chunks[i].n);
    nchunks = 0; unmapped_logs = 0;
    memset(pages, 0, sizeof pages); memset(kdata, 0, sizeof kdata);
    next_ = 0;
    heap_init();
    gm_map(FB_ADDR, SCREEN_W * SCREEN_H * 2 + 0x1000);
}

/* Debug: walk the heap; returns the address of the first inconsistent block, or 0. */
uint32_t gm_heap_check(void) {
    uint32_t a = HEAP_BASE, prev = 0;
    while (1) {
        if (!in_heap(a) && a != HEAP_BASE + HEAP_SIZE - 16) return a;
        uint32_t sz = BSZ(a);
        if (a == HEAP_BASE + HEAP_SIZE - 16) return 0;
        if (sz < 24 || (sz & 7) || a + sz > HEAP_BASE + HEAP_SIZE) return a;
        if (BPREV(a) != prev) return a;
        prev = sz; a += sz;
    }
}
