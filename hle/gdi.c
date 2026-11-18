/* GizmondoDroid - GDI over RGB565 guest framebuffer and guest-memory DIBs */
#include "hle.h"
#include "gdi.h"
#include <stdio.h>
#include <stdlib.h>
#include "font6x10.inc"

Bmp screen_bmp;
int frame_dirty;
static uint32_t stock[20];
uint32_t gdi_screen_dc_default;

uint16_t *fb_host(void) { return (uint16_t *)gm_ptr(FB_ADDR); }

/* ------------------------------------------------------------ pixels (COLORREF = 0x00BBGGRR) */
static inline uint32_t rgb565_to_cr(uint16_t p) {
    uint32_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
    r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
    return r | (g << 8) | (b << 16);
}
static inline uint16_t cr_to_565(uint32_t c) { return (uint16_t)(((c & 0xF8) << 8) | ((c >> 5) & 0x7E0) | ((c >> 19) & 0x1F)); }
static inline uint32_t rgb555_to_cr(uint16_t p) {
    uint32_t r = (p >> 10) & 31, g = (p >> 5) & 31, b = p & 31;
    r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
    return r | (g << 8) | (b << 16);
}
static inline uint32_t row_addr(const Bmp *b, int y) { return b->bits + (uint32_t)(b->topdown ? y : b->h - 1 - y) * (uint32_t)b->pitch; }

uint32_t bmp_get(const Bmp *b, int x, int y) {
    if (x < 0 || y < 0 || x >= b->w || y >= b->h) return 0;
    uint32_t a = row_addr(b, y);
    switch (b->bpp) {
    case 16: { uint16_t p = r16(a + 2 * x); return b->fmt565 ? rgb565_to_cr(p) : rgb555_to_cr(p); }
    case 32: { uint32_t p = r32(a + 4 * x); return ((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF); }
    case 24: return (uint32_t)r8(a + 3 * x + 2) | ((uint32_t)r8(a + 3 * x + 1) << 8) | ((uint32_t)r8(a + 3 * x) << 16);
    case 8: return b->pal[r8(a + x)];
    case 4: { uint8_t v = r8(a + x / 2); return b->pal[(x & 1) ? v & 15 : v >> 4]; }
    case 1: return b->pal[(r8(a + x / 8) >> (7 - (x & 7))) & 1];
    }
    return 0;
}
int bmp_get_index(const Bmp *b, int x, int y) {
    uint32_t a = row_addr(b, y);
    switch (b->bpp) { case 8: return r8(a + x); case 4: { uint8_t v = r8(a + x / 2); return (x & 1) ? v & 15 : v >> 4; } case 1: return (r8(a + x / 8) >> (7 - (x & 7))) & 1; }
    return -1;
}
static int nearest(const Bmp *b, uint32_t c) {
    int best = 0, bd = 1 << 30, n = b->bpp == 8 ? 256 : 1 << b->bpp;
    for (int i = 0; i < n; i++) {
        int dr = (int)(c & 255) - (int)(b->pal[i] & 255), dg = (int)((c >> 8) & 255) - (int)((b->pal[i] >> 8) & 255), db = (int)((c >> 16) & 255) - (int)((b->pal[i] >> 16) & 255);
        int d = dr * dr + dg * dg + db * db; if (d < bd) { bd = d; best = i; if (!d) break; }
    }
    return best;
}
void bmp_put(Bmp *b, int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= b->w || y >= b->h) return;
    uint32_t a = row_addr(b, y);
    switch (b->bpp) {
    case 16: w16(a + 2 * x, b->fmt565 ? cr_to_565(c) : (uint16_t)(((c & 0xF8) << 7) | ((c >> 6) & 0x3E0) | ((c >> 19) & 0x1F))); break;
    case 32: w32(a + 4 * x, ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF)); break;
    case 24: w8(a + 3 * x, (uint8_t)(c >> 16)); w8(a + 3 * x + 1, (uint8_t)(c >> 8)); w8(a + 3 * x + 2, (uint8_t)c); break;
    case 8: w8(a + x, (uint8_t)nearest(b, c)); break;
    case 4: { uint8_t v = r8(a + x / 2); int i = nearest(b, c); w8(a + x / 2, (x & 1) ? (v & 0xF0) | i : (v & 0x0F) | (i << 4)); break; }
    case 1: { uint8_t v = r8(a + x / 8); int s = 7 - (x & 7); w8(a + x / 8, (uint8_t)((v & ~(1 << s)) | (nearest(b, c) << s))); break; }
    }
}

/* ------------------------------------------------------------ objects */
typedef struct { uint32_t color; int hollow; uint32_t pattern; } Brush;
typedef struct { uint32_t color; int width, style; } Pen;
typedef struct { int height, weight; } Font;

static uint32_t new_bitmap(int w, int h, int bpp, int topdown, uint32_t *bits_ptr_out) {
    Bmp *b = calloc(1, sizeof *b);
    b->w = w; b->h = h < 0 ? -h : h; b->bpp = bpp; b->topdown = topdown; b->owned = 1; b->fmt565 = 1;
    b->pitch = ((w * bpp + 31) / 32) * 4;
    uint32_t n = (uint32_t)b->pitch * (uint32_t)b->h; if (!n) n = 4;
    b->bits = gm_alloc(n);
    if (!b->bits) { free(b); return 0; }
    memset(gm_ptr(b->bits), 0, n);
    if (bits_ptr_out) *bits_ptr_out = b->bits;
    return h_new(H_BITMAP, b);
}
static void default_palette(Bmp *b) {
    if (b->bpp == 1) { b->pal[0] = 0; b->pal[1] = 0xFFFFFF; }
    else if (b->bpp == 4) for (int i = 0; i < 16; i++) { int v = (i & 8) ? 255 : 128; b->pal[i] = (i & 1 ? v : 0) << 16 | (i & 2 ? v : 0) << 8 | (i & 4 ? v : 0); }
    else if (b->bpp == 8) for (int i = 0; i < 256; i++) b->pal[i] = ((i & 0xE0) << 0) | ((i & 0x1C) << 11) | ((i & 3) << 22);
}
/* parse BITMAPINFO at guest address; returns a bitmap whose bits are either a copy of, or point at, data */
static uint32_t dib_from_info(uint32_t bi, uint32_t data, int copy, uint32_t *bitsout) {
    uint32_t hs = r32(bi); int w = (int32_t)r32(bi + 4), h = (int32_t)r32(bi + 8);
    int bpp = r16(bi + 14), comp = r32(bi + 16), used = r32(bi + 32);
    if (hs == 12) { w = r16(bi + 4); h = r16(bi + 6); bpp = r16(bi + 10); used = 0; comp = 0; }
    int topdown = h < 0; if (h < 0) h = -h;
    uint32_t hb = new_bitmap(w, h, bpp, topdown, bitsout);
    Bmp *b = h_get(hb, H_BITMAP); if (!b) return 0;
    uint32_t palp = bi + hs;
    if (comp == 3 && bpp == 16) { b->fmt565 = r32(palp + 4) == 0x7E0; palp += 12; }
    else if (comp == 3) palp += 12;
    else b->fmt565 = 0;           /* BI_RGB 16bpp is 5-5-5 */
    if (bpp <= 8) {
        int n = used ? used : 1 << bpp; if (n > 256) n = 256;
        default_palette(b);
        for (int i = 0; i < n; i++) { uint32_t q = r32(palp + (hs == 12 ? 3 * i : 4 * i)); b->pal[i] = ((q & 0xFF) << 16) | (q & 0xFF00) | ((q >> 16) & 0xFF); }
        if (hs == 12) for (int i = 0; i < n; i++) { uint32_t c = r8(palp + 3 * i) | (r8(palp + 3 * i + 1) << 8) | (r8(palp + 3 * i + 2) << 16); b->pal[i] = ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF); }
        palp += hs == 12 ? 3 * n : 4 * n;
    }
    if (data == 0xFFFFFFFFu) data = palp;
    if (copy && data) { uint32_t n = (uint32_t)b->pitch * (uint32_t)b->h; for (uint32_t i = 0; i < n; i++) w8(b->bits + i, r8(data + i)); }
    return hb;
}

uint32_t gdi_make_dc(Bmp *target, uint32_t hbmp) {
    DC *d = calloc(1, sizeof *d);
    d->bmp = target; d->hbmp = hbmp; d->tc = 0; d->bk = 0xFFFFFF; d->bkmode = 2; d->brush = stock[0]; d->pen = stock[7]; d->font = stock[13];
    d->cl_l = 0; d->cl_t = 0; d->cl_r = target->w; d->cl_b = target->h; d->rop2 = 13;
    return h_new(H_DC, d);
}
DC *dc_get(uint32_t h) { return h_get(h, H_DC); }

static void api_CreateCompatibleDC(void) {
    uint32_t mono = new_bitmap(1, 1, 1, 0, NULL);
    Bmp *b = h_get(mono, H_BITMAP); default_palette(b);
    R(gdi_make_dc(b, mono));
    DC *d = dc_get(cpu.r[0]); if (d) d->mono_default = 1;
}
static void api_CreateCompatibleBitmap(void) {
    int w = (int)A(1), h = (int)A(2);
    DC *d = dc_get(A(0)); int bpp = 16; if (d && !d->mono_default && d->bmp) bpp = d->bmp->bpp;
    if (w <= 0 || h <= 0) { w = 1; h = 1; }
    uint32_t hb = new_bitmap(w, h, bpp, 0, NULL); Bmp *b = h_get(hb, H_BITMAP);
    if (b && d && d->bmp) { b->fmt565 = d->bmp->fmt565; memcpy(b->pal, d->bmp->pal, sizeof b->pal); }
    R(hb);
}
static void api_CreateBitmap(void) {
    int w = (int)A(0), h = (int)A(1), bpp = (int)A(3); if (w <= 0) w = 1; if (h <= 0) h = 1; if (bpp <= 0) bpp = 1;
    uint32_t hb = new_bitmap(w, h, bpp, 1, NULL); Bmp *b = h_get(hb, H_BITMAP); default_palette(b);
    if (b && A(4)) { /* WORD-aligned rows, top-down */ int srcp = ((w * bpp + 15) / 16) * 2; for (int y = 0; y < h; y++) for (int x = 0; x < b->pitch && x < srcp; x++) w8(b->bits + (uint32_t)y * b->pitch + x, r8(A(4) + (uint32_t)y * srcp + x)); }
    R(hb);
}
static void api_CreateDIBSection(void) {
    uint32_t bits; uint32_t hb = dib_from_info(A(1), 0, 0, &bits);
    if (A(3)) w32(A(3), bits);
    R(hb);
}
static void api_CreateDIBitmap(void) {   /* hdc, pbmih, fdwInit, pbInit, pbmi, usage */
    if (!A(1)) { R(0); return; }
    uint32_t hs = r32(A(1)); int w = (int32_t)r32(A(1) + 4), h = (int32_t)r32(A(1) + 8); if (hs == 12) { w = r16(A(1) + 4); h = r16(A(1) + 6); }
    DC *d = dc_get(A(0)); int bpp = d && d->bmp && !d->mono_default ? d->bmp->bpp : 16;
    uint32_t hb = new_bitmap(w, h, bpp, 0, NULL); Bmp *b = h_get(hb, H_BITMAP);
    if (b && d && d->bmp) b->fmt565 = d->bmp->fmt565;
    if (b && (A(2) & 4) && A(3) && A(4)) {
        uint32_t src; uint32_t sb = dib_from_info(A(4), A(3), 1, &src); Bmp *s = h_get(sb, H_BITMAP);
        for (int y = 0; y < b->h; y++) for (int x = 0; x < b->w; x++) bmp_put(b, x, y, bmp_get(s, x, y));
        gm_free(s->bits); free(s); h_close(sb);
    }
    R(hb);
}

/* ------------------------------------------------------------ select / delete / stock */
static void api_SelectObject(void) {
    DC *d = dc_get(A(0)); uint32_t o = A(1);
    if (!d) { R(0); return; }
    switch (h_type(o)) {
    case H_BITMAP: { uint32_t old = d->hbmp; Bmp *b = h_get(o, H_BITMAP); d->bmp = b; d->hbmp = o; d->mono_default = 0; d->cl_l = d->cl_t = 0; d->cl_r = b->w; d->cl_b = b->h; R(old); break; }
    case H_BRUSH: { uint32_t old = d->brush; d->brush = o; R(old); break; }
    case H_PEN: { uint32_t old = d->pen; d->pen = o; R(old); break; }
    case H_FONT: { uint32_t old = d->font; d->font = o; R(old); break; }
    default: R(0);
    }
}
static void api_DeleteObject(void) {
    uint32_t h = A(0); int t = h_type(h);
    for (int i = 0; i < 20; i++) if (stock[i] == h) { R(1); return; }
    if (t == H_BITMAP) { Bmp *b = h_get(h, H_BITMAP); if (b != &screen_bmp) { if (b->owned) gm_free(b->bits); free(b); } h_close(h); }
    else if (t == H_BRUSH || t == H_PEN || t == H_FONT || t == H_PALETTE || t == H_REGION) { free(h_get(h, t)); h_close(h); }
    R(t != H_NONE);
}
static void api_DeleteDC(void) { DC *d = dc_get(A(0)); if (d) { if (d->mono_default && d->hbmp) { Bmp *b = d->bmp; if (b && b->owned && b != &screen_bmp) { gm_free(b->bits); free(b); } h_close(d->hbmp); } free(d); h_close(A(0)); } R(1); }
static uint32_t new_brush(uint32_t c, int hollow) { Brush *b = calloc(1, sizeof *b); b->color = c; b->hollow = hollow; return h_new(H_BRUSH, b); }
static void api_CreateSolidBrush(void) { R(new_brush(A(0) & 0xFFFFFF, 0)); }
static void api_CreatePatternBrush(void) { Bmp *b = h_get(A(0), H_BITMAP); R(new_brush(b ? bmp_get(b, 0, 0) : 0, 0)); }
static void api_CreateDIBPatternBrushPt(void) { R(new_brush(0x808080, 0)); }
static void api_CreateBrushIndirect(void) { uint32_t a = A(0); R(new_brush(r32(a + 4), r32(a) == 1)); }
static void api_CreatePen(void) { Pen *p = calloc(1, sizeof *p); p->style = (int)A(0); p->width = (int)A(1); p->color = A(2) & 0xFFFFFF; R(h_new(H_PEN, p)); }
static void api_CreatePenIndirect(void) { Pen *p = calloc(1, sizeof *p); p->style = (int)r32(A(0)); p->width = (int)r32(A(0) + 4); p->color = r32(A(0) + 12) & 0xFFFFFF; R(h_new(H_PEN, p)); }
static void api_CreateFontIndirectW(void) { Font *f = calloc(1, sizeof *f); int h = (int)r32(A(0)); f->height = h < 0 ? -h : h; f->weight = (int)r32(A(0) + 16); R(h_new(H_FONT, f)); }
static void api_GetStockObject(void) { uint32_t i = A(0); R(i < 20 ? stock[i] : 0); }
static void api_GetObjectW(void) {
    uint32_t h = A(0), cb = A(1), p = A(2);
    if (h_type(h) == H_BITMAP) {
        Bmp *b = h_get(h, H_BITMAP);
        if (cb < 24) { R(0); return; }
        w32(p, 0); w32(p + 4, (uint32_t)b->w); w32(p + 8, (uint32_t)b->h); w32(p + 12, (uint32_t)b->pitch); w16(p + 16, 1); w16(p + 18, (uint16_t)b->bpp); w32(p + 20, b->dib ? b->bits : 0);
        R(24);
    } else if (h_type(h) == H_BRUSH) { Brush *b = h_get(h, H_BRUSH); if (cb >= 12) { w32(p, 0); w32(p + 4, b->color); w32(p + 8, 0); } R(12); }
    else R(0);
}
static void api_GetObjectType(void) { switch (h_type(A(0))) { case H_BITMAP: R(7); break; case H_BRUSH: R(2); break; case H_PEN: R(1); break; case H_FONT: R(6); break; case H_DC: R(3); break; default: R(0); } }

/* ------------------------------------------------------------ state */
static void api_SetBkColor(void) { DC *d = dc_get(A(0)); if (!d) { R(0xFFFFFFFF); return; } R(d->bk); d->bk = A(1) & 0xFFFFFF; }
static void api_SetTextColor(void) { DC *d = dc_get(A(0)); if (!d) { R(0xFFFFFFFF); return; } R(d->tc); d->tc = A(1) & 0xFFFFFF; }
static void api_GetBkColor(void) { DC *d = dc_get(A(0)); R(d ? d->bk : 0); }
static void api_GetTextColor(void) { DC *d = dc_get(A(0)); R(d ? d->tc : 0); }
static void api_SetBkMode(void) { DC *d = dc_get(A(0)); if (!d) { R(0); return; } R((uint32_t)d->bkmode); d->bkmode = (int)A(1); }
static void api_SetROP2(void) { DC *d = dc_get(A(0)); if (!d) { R(0); return; } R((uint32_t)d->rop2); d->rop2 = (int)A(1); }
static void api_GetDeviceCaps(void) {
    switch (A(1)) { case 2: R(1); break; case 8: R(320); break; case 10: R(240); break; case 4: R(60); break; case 6: R(45); break;
    case 12: R(16); break; case 14: R(1); break; case 24: R(0xFFFFFFFF); break; case 38: R(0x100); break;
    case 88: case 90: R(96); break; case 104: R(0); break; case 108: R(0); break; default: R(0); }
}
static void api_NoOp1(void) { R(1); }
static void api_NoOp0(void) { R(0); }
static void api_SelectPalette(void) { R(0); }
static void api_CreatePalette(void) { R(h_new(H_PALETTE, calloc(1, 16))); }
static void api_IntersectClipRect(void) {
    DC *d = dc_get(A(0)); if (!d) { R(0); return; }
    int l = (int)A(1) + d->ox, t = (int)A(2) + d->oy, r = (int)A(3) + d->ox, b = (int)A(4) + d->oy;
    if (l > d->cl_l) d->cl_l = l; if (t > d->cl_t) d->cl_t = t; if (r < d->cl_r) d->cl_r = r; if (b < d->cl_b) d->cl_b = b;
    R(2);
}
static void api_SetViewportOrgEx(void) { DC *d = dc_get(A(0)); if (d) { if (A(3)) { w32(A(3), (uint32_t)d->vx); w32(A(3) + 4, (uint32_t)d->vy); } d->vx = (int)A(1); d->vy = (int)A(2); } R(1); }
static void api_SetWindowOrgEx(void) { R(1); }

/* ------------------------------------------------------------ drawing */
static inline void clip_rect(DC *d, int *l, int *t, int *r, int *b) {
    if (*l < d->cl_l) *l = d->cl_l; if (*t < d->cl_t) *t = d->cl_t;
    if (*r > d->cl_r) *r = d->cl_r; if (*b > d->cl_b) *b = d->cl_b;
    if (*l < 0) *l = 0; if (*t < 0) *t = 0; if (*r > d->bmp->w) *r = d->bmp->w; if (*b > d->bmp->h) *b = d->bmp->h;
}
static inline void dpix(DC *d, int x, int y, uint32_t c) {
    x += d->ox + d->vx; y += d->oy + d->vy;
    if (x < d->cl_l || y < d->cl_t || x >= d->cl_r || y >= d->cl_b) return;
    bmp_put(d->bmp, x, y, c);
}
void dc_fill(DC *d, int l, int t, int r, int b, uint32_t c) {
    l += d->ox + d->vx; r += d->ox + d->vx; t += d->oy + d->vy; b += d->oy + d->vy;
    clip_rect(d, &l, &t, &r, &b);
    if (l >= r || t >= b) return;
    Bmp *bm = d->bmp;
    if (bm->bpp == 16) {
        uint16_t p = bm->fmt565 ? cr_to_565(c) : (uint16_t)(((c & 0xF8) << 7) | ((c >> 6) & 0x3E0) | ((c >> 19) & 0x1F));
        for (int y = t; y < b; y++) { uint32_t a = row_addr(bm, y); uint16_t *hp = (uint16_t *)gm_ptr(a + 2 * l); if (hp) for (int x = 0; x < r - l; x++) hp[x] = p; else for (int x = l; x < r; x++) w16(a + 2 * x, p); }
    } else for (int y = t; y < b; y++) for (int x = l; x < r; x++) bmp_put(bm, x, y, c);
    if (bm == &screen_bmp) frame_dirty = 1;
}
static uint32_t brush_color(DC *d) { Brush *b = h_get(d->brush, H_BRUSH); return b ? b->color : 0xFFFFFF; }
static int brush_hollow(DC *d) { Brush *b = h_get(d->brush, H_BRUSH); return !b || b->hollow; }
static uint32_t pen_color(DC *d) { Pen *p = h_get(d->pen, H_PEN); return p ? p->color : 0; }
static int pen_null(DC *d) { Pen *p = h_get(d->pen, H_PEN); return !p || p->style == 5; }
void dc_fill_brush(DC *d, uint32_t hbr, int l, int t, int r, int b) {
    Brush *br = h_get(hbr, H_BRUSH); if (!br || br->hollow) return;
    dc_fill(d, l, t, r, b, br->color);
}

static void line(DC *d, int x0, int y0, int x1, int y1, uint32_t c) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
    for (int n = 0; n < 4096; n++) {
        dpix(d, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * e; if (e2 >= dy) { e += dy; x0 += sx; } if (e2 <= dx) { e += dx; y0 += sy; }
    }
    if (d->bmp == &screen_bmp) frame_dirty = 1;
}
static void api_MoveToEx(void) { DC *d = dc_get(A(0)); if (!d) { R(0); return; } if (A(3)) { w32(A(3), (uint32_t)d->cx); w32(A(3) + 4, (uint32_t)d->cy); } d->cx = (int)A(1); d->cy = (int)A(2); R(1); }
static void api_LineTo(void) { DC *d = dc_get(A(0)); if (!d) { R(0); return; } if (!pen_null(d)) line(d, d->cx, d->cy, (int)A(1), (int)A(2), pen_color(d)); d->cx = (int)A(1); d->cy = (int)A(2); R(1); }
static void api_Rectangle(void) {
    DC *d = dc_get(A(0)); if (!d) { R(0); return; }
    int l = (int)A(1), t = (int)A(2), r = (int)A(3), b = (int)A(4);
    if (!brush_hollow(d)) dc_fill(d, l + 1, t + 1, r - 1, b - 1, brush_color(d));
    if (!pen_null(d)) { uint32_t c = pen_color(d); line(d, l, t, r - 1, t, c); line(d, r - 1, t, r - 1, b - 1, c); line(d, r - 1, b - 1, l, b - 1, c); line(d, l, b - 1, l, t, c); }
    R(1);
}
static void api_RoundRect(void) { api_Rectangle(); }
static void api_Ellipse(void) {
    DC *d = dc_get(A(0)); if (!d) { R(0); return; }
    int l = (int)A(1), t = (int)A(2), r = (int)A(3), b = (int)A(4);
    double cx = (l + r - 1) / 2.0, cy = (t + b - 1) / 2.0, rx = (r - l) / 2.0, ry = (b - t) / 2.0;
    if (rx <= 0 || ry <= 0) { R(1); return; }
    uint32_t fc = brush_color(d), pc = pen_color(d); int fill = !brush_hollow(d), pen = !pen_null(d);
    for (int y = t; y < b; y++) {
        double dy = (y - cy) / ry; double w = 1 - dy * dy; if (w < 0) continue;
        double hx = rx * sqrt(w); int xl = (int)floor(cx - hx + 0.5), xr = (int)floor(cx + hx + 0.5);
        if (fill) dc_fill(d, xl, y, xr + 1, y + 1, fc);
        if (pen) { dpix(d, xl, y, pc); dpix(d, xr, y, pc); }
    }
    if (pen) for (int x = l; x < r; x++) {
        double dx = (x - cx) / rx; double w = 1 - dx * dx; if (w < 0) continue; double hy = ry * sqrt(w);
        dpix(d, x, (int)floor(cy - hy + 0.5), pc); dpix(d, x, (int)floor(cy + hy + 0.5), pc);
    }
    R(1);
}
static void api_Polyline(void) {
    DC *d = dc_get(A(0)); int n = (int)A(2); if (!d || n < 2) { R(0); return; }
    for (int i = 0; i + 1 < n; i++) line(d, (int)r32(A(1) + 8 * i), (int)r32(A(1) + 8 * i + 4), (int)r32(A(1) + 8 * i + 8), (int)r32(A(1) + 8 * i + 12), pen_color(d));
    R(1);
}
static void api_Polygon(void) {
    DC *d = dc_get(A(0)); int n = (int)A(2); if (!d || n < 2 || n > 256) { R(0); return; }
    int px[256], py[256]; int ymin = 1 << 30, ymax = -(1 << 30);
    for (int i = 0; i < n; i++) { px[i] = (int)r32(A(1) + 8 * i); py[i] = (int)r32(A(1) + 8 * i + 4); if (py[i] < ymin) ymin = py[i]; if (py[i] > ymax) ymax = py[i]; }
    if (!brush_hollow(d)) for (int y = ymin; y <= ymax; y++) {
        int xs[256], k = 0;
        for (int i = 0; i < n; i++) { int j = (i + 1) % n; if ((py[i] <= y && py[j] > y) || (py[j] <= y && py[i] > y)) xs[k++] = px[i] + (y - py[i]) * (px[j] - px[i]) / (py[j] - py[i]); }
        for (int a = 1; a < k; a++) for (int b = a; b > 0 && xs[b - 1] > xs[b]; b--) { int t = xs[b]; xs[b] = xs[b - 1]; xs[b - 1] = t; }
        for (int a = 0; a + 1 < k; a += 2) dc_fill(d, xs[a], y, xs[a + 1], y + 1, brush_color(d));
    }
    if (!pen_null(d)) for (int i = 0; i < n; i++) line(d, px[i], py[i], px[(i + 1) % n], py[(i + 1) % n], pen_color(d));
    R(1);
}
static void api_SetPixel(void) { DC *d = dc_get(A(0)); if (!d) { R(0xFFFFFFFF); return; } dpix(d, (int)A(1), (int)A(2), A(3) & 0xFFFFFF); R(A(3)); }
static void api_GetPixel(void) { DC *d = dc_get(A(0)); if (!d) { R(0xFFFFFFFF); return; } R(bmp_get(d->bmp, (int)A(1) + d->ox, (int)A(2) + d->oy)); }

static uint32_t apply_rop(uint32_t rop, uint32_t s, uint32_t dst, uint32_t pat) {
    switch ((rop >> 16) & 0xFF) {
    case 0xCC: return s;                  /* SRCCOPY */
    case 0xEE: return s | dst;            /* SRCPAINT */
    case 0x88: return s & dst;            /* SRCAND */
    case 0x66: return (s ^ dst) & 0xFFFFFF;  /* SRCINVERT */
    case 0x44: return s & ~dst & 0xFFFFFF;   /* SRCERASE */
    case 0x33: return ~s & 0xFFFFFF;      /* NOTSRCCOPY */
    case 0x11: return ~(s | dst) & 0xFFFFFF; /* NOTSRCERASE */
    case 0x00: return 0;                  /* BLACKNESS */
    case 0xFF: return 0xFFFFFF;           /* WHITENESS */
    case 0x55: return ~dst & 0xFFFFFF;    /* DSTINVERT */
    case 0xF0: return pat;                /* PATCOPY */
    case 0x5A: return (pat ^ dst) & 0xFFFFFF; /* PATINVERT */
    case 0xFA: return pat | dst;          /* PATPAINT-ish */
    case 0xB8: case 0xE2: return s;       /* mask ops: approximate */
    default: return s;
    }
}
void gdi_blit(DC *dd, int dx, int dy, int dw, int dh, DC *sd, const Bmp *sb, int sx, int sy, int sw, int sh, uint32_t rop, int use_key, uint32_t key) {
    int need_src = ((rop >> 16) & 0xFF) != 0x00 && ((rop >> 16) & 0xFF) != 0xFF && ((rop >> 16) & 0xFF) != 0x55 && ((rop >> 16) & 0xFF) != 0xF0 && ((rop >> 16) & 0xFF) != 0x5A;
    if (need_src && !sb) return;
    int flipx = 0, flipy = 0;
    if (dw < 0) { dw = -dw; dx -= dw; flipx = 1; } if (dh < 0) { dh = -dh; dy -= dh; flipy = 1; }
    if (sw == 0 || sh == 0) { sw = dw; sh = dh; }
    int ox = dd->ox + dd->vx, oy = dd->oy + dd->vy;
    int sox = sd ? sd->ox + sd->vx : 0, soy = sd ? sd->oy + sd->vy : 0;
    int l = dx + ox, t = dy + oy, r = dx + dw + ox, b = dy + dh + oy;
    int cl = l, ct = t, cr = r, cb = b; clip_rect(dd, &cl, &ct, &cr, &cb);
    if (cl >= cr || ct >= cb) return;
    uint32_t pat = brush_color(dd);
    Bmp *db = dd->bmp;
    int same = need_src && sb && sw == dw && sh == dh && !use_key && ((rop >> 16) & 0xFF) == 0xCC && sb->bpp == db->bpp && (db->bpp != 16 || sb->fmt565 == db->fmt565) && db->bpp >= 8 && !flipx && !flipy;
    if (same && db->bpp >= 8 && (db->bpp != 8 || !memcmp(sb->pal, db->pal, 1024))) {
        int bytes = (cr - cl) * db->bpp / 8;
        for (int y = ct; y < cb; y++) {
            int ssy = sy + soy + (y - t), ssx = sx + sox + (cl - l);
            if (ssy < 0 || ssy >= sb->h || ssx < 0 || ssx + (cr - cl) > sb->w) goto slow;
            uint8_t *dp = gm_ptr(row_addr(db, y) + (uint32_t)cl * db->bpp / 8), *sp = gm_ptr(row_addr(sb, ssy) + (uint32_t)ssx * sb->bpp / 8);
            if (!dp || !sp) goto slow;
            memmove(dp, sp, (size_t)bytes);
        }
        if (db == &screen_bmp) frame_dirty = 1;
        return;
    }
slow:
    for (int y = ct; y < cb; y++) {
        int fy = y - t; if (flipy) fy = dh - 1 - fy;
        int ssy = sh == dh ? sy + fy : sy + fy * sh / dh;
        for (int x = cl; x < cr; x++) {
            int fx = x - l; if (flipx) fx = dw - 1 - fx;
            int ssx = sw == dw ? sx + fx : sx + fx * sw / dw;
            uint32_t s = 0;
            if (need_src) {
                int xx = ssx + sox, yy = ssy + soy;
                if (xx < 0 || yy < 0 || xx >= sb->w || yy >= sb->h) continue;
                s = bmp_get(sb, xx, yy);
                if (use_key && (s & 0xFFFFFF) == (key & 0xFFFFFF)) continue;
            }
            uint32_t d0 = (((rop >> 16) & 0xFF) == 0xCC || ((rop >> 16) & 0xFF) == 0xF0 || ((rop >> 16) & 0xFF) == 0x33 || ((rop >> 16) & 0xFF) == 0x00 || ((rop >> 16) & 0xFF) == 0xFF) ? 0 : bmp_get(db, x, y);
            bmp_put(db, x, y, apply_rop(rop, s, d0, pat));
        }
    }
    if (db == &screen_bmp) frame_dirty = 1;
}
static void api_BitBlt(void) {
    DC *dd = dc_get(A(0)); if (!dd) { R(0); return; }
    DC *sd = dc_get(A(5));
    gdi_blit(dd, (int)A(1), (int)A(2), (int)A(3), (int)A(4), sd, sd ? sd->bmp : NULL, (int)A(6), (int)A(7), (int)A(3), (int)A(4), A(8), 0, 0);
    R(1);
}
static void api_StretchBlt(void) {
    DC *dd = dc_get(A(0)); if (!dd) { R(0); return; }
    DC *sd = dc_get(A(5));
    gdi_blit(dd, (int)A(1), (int)A(2), (int)A(3), (int)A(4), sd, sd ? sd->bmp : NULL, (int)A(6), (int)A(7), (int)A(8), (int)A(9), A(10), 0, 0);
    R(1);
}
static void api_MaskBlt(void) { /* hdcDest,x,y,w,h,hdcSrc,xs,ys,hbmMask,xm,ym,rop */
    DC *dd = dc_get(A(0)); DC *sd = dc_get(A(5)); Bmp *mk = h_get(A(8), H_BITMAP);
    if (!dd || !sd) { R(0); return; }
    int x = (int)A(1), y = (int)A(2), w = (int)A(3), h = (int)A(4), sx = (int)A(6), sy = (int)A(7), mx = (int)A(9), my = (int)A(10);
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
        if (mk && (bmp_get(mk, mx + i, my + j) & 0xFF) == 0) continue;     /* mask bit 1 = copy */
        dpix(dd, x + i, y + j, bmp_get(sd->bmp, sx + i + sd->ox, sy + j + sd->oy));
    }
    if (dd->bmp == &screen_bmp) frame_dirty = 1;
    R(1);
}
static void api_PatBlt(void) {
    DC *dd = dc_get(A(0)); if (!dd) { R(0); return; }
    int x = (int)A(1), y = (int)A(2), w = (int)A(3), h = (int)A(4); uint32_t rop = A(5);
    uint32_t op = (rop >> 16) & 0xFF;
    if (op == 0xF0) dc_fill(dd, x, y, x + w, y + h, brush_color(dd));
    else if (op == 0x00) dc_fill(dd, x, y, x + w, y + h, 0);
    else if (op == 0xFF) dc_fill(dd, x, y, x + w, y + h, 0xFFFFFF);
    else gdi_blit(dd, x, y, w, h, NULL, NULL, 0, 0, w, h, rop, 0, 0);
    R(1);
}
static void api_TransparentImage(void) {   /* hdc,x,y,w,h, hsrc,sx,sy,sw,sh, color */
    DC *dd = dc_get(A(0)); if (!dd) { R(0); return; }
    DC *sd = dc_get(A(5)); const Bmp *sb = sd ? sd->bmp : h_get(A(5), H_BITMAP);
    if (!sb) { R(0); return; }
    gdi_blit(dd, (int)A(1), (int)A(2), (int)A(3), (int)A(4), sd, sb, (int)A(6), (int)A(7), (int)A(8), (int)A(9), 0xCC0020, 1, A(10));
    R(1);
}
static void api_StretchDIBits(void) {   /* hdc,xd,yd,wd,hd,xs,ys,ws,hs,bits,bmi,usage,rop */
    DC *dd = dc_get(A(0)); if (!dd) { R(0); return; }
    uint32_t bits; uint32_t hb = dib_from_info(A(10), A(9), 1, &bits); Bmp *s = h_get(hb, H_BITMAP); if (!s) { R(0); return; }
    int ys = (int)A(6), hs = (int)A(8);
    if (!s->topdown) ys = s->h - ys - hs;    /* source Y is bottom-up */
    int ws = (int)A(7);
    Bmp tmp = *s; tmp.topdown = 1; tmp.bits = s->bits;
    if (!s->topdown) { tmp.topdown = 0; ys = (int)A(6); }
    gdi_blit(dd, (int)A(1), (int)A(2), (int)A(3), (int)A(4), NULL, &tmp, (int)A(5), s->topdown ? ys : (s->h - (int)A(6) - hs), ws, hs, A(12), 0, 0);
    gm_free(s->bits); free(s); h_close(hb); R((uint32_t)hs);
}
static void api_SetDIBitsToDevice(void) {   /* hdc,xd,yd,w,h,xs,ys,start,lines,bits,bmi,usage */
    DC *dd = dc_get(A(0)); if (!dd) { R(0); return; }
    uint32_t bits; uint32_t hb = dib_from_info(A(10), A(9), 1, &bits); Bmp *s = h_get(hb, H_BITMAP); if (!s) { R(0); return; }
    int w = (int)A(3), h = (int)A(4), xs = (int)A(5), ys = (int)A(6);
    gdi_blit(dd, (int)A(1), (int)A(2), w, h, NULL, s, xs, s->topdown ? ys : s->h - ys - h, w, h, 0xCC0020, 0, 0);
    gm_free(s->bits); free(s); h_close(hb); R((uint32_t)h);
}
static void api_SetDIBits(void) { R(0); }
static void api_GetDIBits(void) { R(0); }

/* ------------------------------------------------------------ text */
static int font_scale(DC *d) { Font *f = h_get(d->font, H_FONT); int h = f ? f->height : 0; return h >= 20 ? 2 : 1; }
static int text_run(DC *d, int x, int y, const char *s, int n, int draw) {
    int sc = font_scale(d), cw = 6 * sc;
    if (draw) {
        if (d->bkmode == 2) dc_fill(d, x, y, x + n * cw, y + 10 * sc, d->bk);
        for (int i = 0; i < n; i++) {
            unsigned char c = (unsigned char)s[i]; if (c < 32 || c > 126) { if (c < 32) continue; c = '?'; }
            const uint8_t *g = font6x10[c - 32];
            for (int gy = 0; gy < 10; gy++) for (int gx = 0; gx < 6; gx++) if (g[gy] & (0x20 >> gx))
                for (int a = 0; a < sc; a++) for (int b = 0; b < sc; b++) dpix(d, x + i * cw + gx * sc + a, y + gy * sc + b, d->tc);
        }
        if (d->bmp == &screen_bmp) frame_dirty = 1;
    }
    return n * cw;
}
static void api_ExtTextOutW(void) {   /* hdc,x,y,options,lprect,str,count,dx */
    DC *d = dc_get(A(0)); if (!d) { R(0); return; }
    int n = (int)A(6); char s[512]; if (n > 511) n = 511; if (n < 0) n = 0;
    for (int i = 0; i < n; i++) { uint16_t c = r16(A(5) + 2 * i); s[i] = c < 128 ? (char)c : '?'; }
    s[n] = 0;
    int x = (int)A(1), y = (int)A(2);
    if (A(3) & 2 && A(4)) dc_fill(d, (int)r32(A(4)), (int)r32(A(4) + 4), (int)r32(A(4) + 8), (int)r32(A(4) + 12), d->bk);   /* ETO_OPAQUE */
    if ((A(3) & 2) && A(4)) { int bm = d->bkmode; d->bkmode = 1; text_run(d, x, y, s, n, 1); d->bkmode = bm; }
    else text_run(d, x, y, s, n, 1);
    R(1);
}
static void api_TextOutW(void) {
    DC *d = dc_get(A(0)); if (!d) { R(0); return; }
    int n = (int)A(4); char s[512]; if (n > 511) n = 511; if (n < 0) n = 0;
    for (int i = 0; i < n; i++) { uint16_t c = r16(A(3) + 2 * i); s[i] = c < 128 ? (char)c : '?'; }
    text_run(d, (int)A(1), (int)A(2), s, n, 1); R(1);
}
static void api_GetTextExtentPoint32W(void) {
    DC *d = dc_get(A(0)); int sc = d ? font_scale(d) : 1; int n = (int)A(2);
    if (A(3)) { w32(A(3), (uint32_t)(n * 6 * sc)); w32(A(3) + 4, (uint32_t)(10 * sc)); }
    R(1);
}
static void api_GetTextExtentExPointW(void) { R(1); }
static void api_GetTextMetricsW(void) {
    DC *d = dc_get(A(0)); int sc = d ? font_scale(d) : 1; uint32_t t = A(1);
    for (int i = 0; i < 15; i++) w32(t + 4 * i, 0);
    w32(t, (uint32_t)(10 * sc)); w32(t + 4, (uint32_t)(8 * sc)); w32(t + 8, (uint32_t)(2 * sc)); w32(t + 20, (uint32_t)(6 * sc)); w32(t + 24, (uint32_t)(6 * sc));
    R(1);
}
static void api_DrawTextW(void) {   /* hdc, str, count, rect, format */
    DC *d = dc_get(A(0)); if (!d) { R(0); return; }
    int n = (int)A(2); uint32_t fm = A(4), rc = A(3);
    char s[1024]; if (n < 0) n = gw_len(A(1)); if (n > 1023) n = 1023;
    for (int i = 0; i < n; i++) { uint16_t c = r16(A(1) + 2 * i); s[i] = c < 128 ? (char)c : '?'; } s[n] = 0;
    int sc = font_scale(d), cw = 6 * sc, lh = 10 * sc + 1;
    int l = (int)r32(rc), t = (int)r32(rc + 4), r = (int)r32(rc + 8), b = (int)r32(rc + 12), width = r - l;
    /* split into lines */
    struct { int off, len; } lines[64]; int nl = 0, i = 0;
    while (i <= n && nl < 64) {
        int st = i, ln = 0, lastsp = -1;
        while (i < n && s[i] != '\n') {
            if (s[i] == '\r') { i++; continue; }
            if ((fm & 0x10) && !(fm & 0x20) && (ln + 1) * cw > width && width > 0) { if (lastsp >= 0) { i = st + lastsp + 1; ln = lastsp; } break; }
            if (s[i] == ' ') lastsp = ln;
            ln++; i++;
        }
        lines[nl].off = st; lines[nl].len = ln; nl++;
        if (i < n && s[i] == '\n') i++;
        if ((fm & 0x20)) break;
        if (i >= n) break;
        while (i < n && s[i] == ' ' && (fm & 0x10)) i++;
    }
    int total = nl * lh;
    if (fm & 0x400) {   /* DT_CALCRECT */
        int mw = 0; for (int k = 0; k < nl; k++) if (lines[k].len * cw > mw) mw = lines[k].len * cw;
        w32(rc + 8, (uint32_t)(l + (fm & 0x10 ? width : mw))); w32(rc + 12, (uint32_t)(t + total)); R((uint32_t)total); return;
    }
    int y = t; if (fm & 4) y = t + (b - t - total) / 2; else if (fm & 8) y = b - total;
    for (int k = 0; k < nl; k++) {
        int w = lines[k].len * cw, x = l;
        if (fm & 1) x = l + (width - w) / 2; else if (fm & 2) x = r - w;
        text_run(d, x, y, s + lines[k].off, lines[k].len, 1); y += lh;
    }
    R((uint32_t)total);
}
static void api_FillRect(void) {
    DC *d = dc_get(A(0)); if (!d) { R(0); return; }
    uint32_t rc = A(1), br = A(2);
    if (br < 32) { extern uint32_t gdi_syscolor_brush(uint32_t i); br = gdi_syscolor_brush(br - 1); }
    dc_fill_brush(d, br, (int)r32(rc), (int)r32(rc + 4), (int)r32(rc + 8), (int)r32(rc + 12)); R(1);
}
static void api_FrameRect(void) {
    DC *d = dc_get(A(0)); if (!d) { R(0); return; } uint32_t rc = A(1); Brush *b = h_get(A(2), H_BRUSH); if (!b) { R(0); return; }
    int l = (int)r32(rc), t = (int)r32(rc + 4), r = (int)r32(rc + 8), bt = (int)r32(rc + 12);
    dc_fill(d, l, t, r, t + 1, b->color); dc_fill(d, l, bt - 1, r, bt, b->color); dc_fill(d, l, t, l + 1, bt, b->color); dc_fill(d, r - 1, t, r, bt, b->color); R(1);
}
static void api_InvertRect(void) { DC *d = dc_get(A(0)); if (!d) { R(0); return; } uint32_t rc = A(1); for (int y = (int)r32(rc + 4); y < (int)r32(rc + 12); y++) for (int x = (int)r32(rc); x < (int)r32(rc + 8); x++) dpix(d, x, y, bmp_get(d->bmp, x + d->ox, y + d->oy) ^ 0xFFFFFF); R(1); }
static void api_DrawEdge(void) { R(1); }

/* ------------------------------------------------------------ resources / images */
static Module *mod_of(uint32_t h) { Module *m = h ? pe_find(h) : NULL; return m ? m : main_module; }
static uint32_t load_bitmap_res(uint32_t hinst, uint32_t name) {
    Module *m = mod_of(hinst); uint32_t sz; if (!m) return 0;
    uint32_t p = pe_resource(m, 2, name, &sz);
    if (!p) { for (int i = 0; i < 0 ; i++) {} return 0; }
    uint32_t bits; uint32_t hb = dib_from_info(p, 0xFFFFFFFFu, 1, &bits);
    Bmp *b = h_get(hb, H_BITMAP); if (b) b->dib = 0;
    return hb;
}
static uint32_t load_bmp_file(const char *gpath) {
    char h[600]; if (!fs_map(gpath, h, sizeof h, 0)) return 0;
    FILE *f = fopen(h, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n); if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return 0; } fclose(f);
    if (n < 54 || buf[0] != 'B' || buf[1] != 'M') { free(buf); return 0; }
    uint32_t off = buf[10] | buf[11] << 8 | buf[12] << 16 | (uint32_t)buf[13] << 24;
    uint32_t g = gm_alloc((uint32_t)n); gm_write(g, buf, (uint32_t)n); free(buf);
    uint32_t bits; uint32_t hb = dib_from_info(g + 14, g + off, 1, &bits); gm_free(g); return hb;
}
static void api_LoadBitmapW(void) { R(load_bitmap_res(A(0), A(1))); }
static void api_LoadImageW(void) {   /* hinst, name, type, cx, cy, flags */
    if (A(2) == 0) {
        if (A(5) & 0x10) { char g[600]; gw_read(A(1), g, sizeof g); R(load_bmp_file(g)); }
        else R(load_bitmap_res(A(0), A(1)));
    } else R(h_new(H_MISC, calloc(1, 8)));
}
static void api_LoadIconW(void) { R(h_new(H_MISC, calloc(1, 8))); }
static void api_LoadStringW(void) {   /* hinst, id, buf, cch */
    Module *m = mod_of(A(0)); uint32_t sz; if (!m) { R(0); return; }
    uint32_t blk = pe_resource(m, 6, (A(1) >> 4) + 1, &sz); if (!blk) { R(0); return; }
    uint32_t p = blk; for (uint32_t i = 0; i < (A(1) & 15); i++) p += 2 + 2 * r16(p);
    uint32_t len = r16(p); p += 2;
    if (!A(3)) { R(0); return; }
    if (A(3) == 0) { R(len); return; }
    uint32_t n = len < A(3) - 1 ? len : A(3) - 1; for (uint32_t i = 0; i < n; i++) w16(A(2) + 2 * i, r16(p + 2 * i)); w16(A(2) + 2 * n, 0);
    R(n);
}
static void api_DrawIcon(void) { R(1); }

uint32_t gdi_syscolor_brush(uint32_t i) { return i < 32 ? stock[16 + (i & 3)] : stock[0]; }
uint32_t gdi_syscolor(uint32_t i) {
    static const uint32_t cols[] = { 0xC0C0C0, 0x808000, 0x800000, 0xC0C0C0, 0xFFFFFF, 0xFFFFFF, 0x000000, 0x000000, 0x000000, 0xFFFFFF, 0xC0C0C0, 0xC0C0C0, 0x808080, 0x800000, 0xFFFFFF, 0xC0C0C0, 0x808080, 0x808080, 0x000000, 0xC0C0C0, 0xFFFFFF, 0x000000, 0xC0C0C0, 0x000000, 0xFFFFE1 };
    return i < sizeof cols / sizeof *cols ? cols[i] : 0;
}
static void api_GetSysColor(void) { R(gdi_syscolor(A(0))); }
static void api_GetSysColorBrush(void) { R(stock[16]); }

uint32_t gdi_screen_dc(int ox, int oy, int cl, int ct, int cr, int cb) {
    uint32_t h = gdi_make_dc(&screen_bmp, 0); DC *d = dc_get(h);
    d->ox = ox; d->oy = oy; d->cl_l = cl; d->cl_t = ct; d->cl_r = cr; d->cl_b = cb; d->screen = 1; return h;
}

void gdi_init(void) {
    screen_bmp.w = SCREEN_W; screen_bmp.h = SCREEN_H; screen_bmp.bpp = 16; screen_bmp.pitch = SCREEN_W * 2; screen_bmp.bits = FB_ADDR; screen_bmp.topdown = 1; screen_bmp.fmt565 = 1;
    memset(stock, 0, sizeof stock);
    uint32_t grays[5] = { 0xFFFFFF, 0xC0C0C0, 0x808080, 0x404040, 0x000000 };
    for (int i = 0; i < 5; i++) stock[i] = new_brush(grays[i], 0);
    stock[5] = new_brush(0, 1);
    for (int i = 6; i < 9; i++) { Pen *p = calloc(1, sizeof *p); p->color = i == 6 ? 0xFFFFFF : 0; p->width = 1; p->style = i == 8 ? 5 : 0; stock[i] = h_new(H_PEN, p); }
    stock[13] = h_new(H_FONT, calloc(1, sizeof(Font)));
    stock[10] = stock[11] = stock[12] = stock[14] = stock[13];
    stock[15] = h_new(H_PALETTE, calloc(1, 16));
    stock[16] = new_brush(0xC0C0C0, 0); stock[17] = new_brush(0xC0C0C0, 0); stock[18] = new_brush(0xFFFFFF, 0); stock[19] = new_brush(0x000000, 0);
}

#define E(n, f) { n, f }
static const ApiDef gdefs[] = {
    E("CreateCompatibleDC", api_CreateCompatibleDC), E("CreateCompatibleBitmap", api_CreateCompatibleBitmap), E("CreateBitmap", api_CreateBitmap),
    E("CreateDIBSection", api_CreateDIBSection), E("CreateDIBitmap", api_CreateDIBitmap), E("SelectObject", api_SelectObject), E("DeleteObject", api_DeleteObject),
    E("DeleteDC", api_DeleteDC), E("CreateSolidBrush", api_CreateSolidBrush), E("CreatePatternBrush", api_CreatePatternBrush), E("CreateDIBPatternBrushPt", api_CreateDIBPatternBrushPt),
    E("CreateBrushIndirect", api_CreateBrushIndirect), E("CreatePen", api_CreatePen), E("CreatePenIndirect", api_CreatePenIndirect), E("CreateFontIndirectW", api_CreateFontIndirectW),
    E("GetStockObject", api_GetStockObject), E("GetObjectW", api_GetObjectW), E("GetObjectType", api_GetObjectType),
    E("SetBkColor", api_SetBkColor), E("SetTextColor", api_SetTextColor), E("GetBkColor", api_GetBkColor), E("GetTextColor", api_GetTextColor), E("SetBkMode", api_SetBkMode), E("SetROP2", api_SetROP2),
    E("GetDeviceCaps", api_GetDeviceCaps), E("SelectPalette", api_SelectPalette), E("RealizePalette", api_NoOp0), E("CreatePalette", api_CreatePalette), E("CreateHalftonePalette", api_CreatePalette),
    E("GetSystemPaletteEntries", api_NoOp0), E("SetPaletteEntries", api_NoOp0), E("GetPaletteEntries", api_NoOp0), E("GetNearestPaletteIndex", api_NoOp0),
    E("IntersectClipRect", api_IntersectClipRect), E("SelectClipRgn", api_NoOp1), E("ExcludeClipRect", api_NoOp1), E("SetViewportOrgEx", api_SetViewportOrgEx), E("SetWindowOrgEx", api_SetWindowOrgEx),
    E("MoveToEx", api_MoveToEx), E("LineTo", api_LineTo), E("Rectangle", api_Rectangle), E("RoundRect", api_RoundRect), E("Ellipse", api_Ellipse), E("Polyline", api_Polyline), E("Polygon", api_Polygon),
    E("SetPixel", api_SetPixel), E("GetPixel", api_GetPixel), E("BitBlt", api_BitBlt), E("StretchBlt", api_StretchBlt), E("MaskBlt", api_MaskBlt), E("PatBlt", api_PatBlt),
    E("TransparentImage", api_TransparentImage), E("StretchDIBits", api_StretchDIBits), E("SetDIBitsToDevice", api_SetDIBitsToDevice), E("SetDIBits", api_SetDIBits), E("GetDIBits", api_GetDIBits),
    E("ExtTextOutW", api_ExtTextOutW), E("TextOutW", api_TextOutW), E("GetTextExtentPoint32W", api_GetTextExtentPoint32W), E("GetTextExtentExPointW", api_GetTextExtentExPointW),
    E("GetTextMetricsW", api_GetTextMetricsW), E("DrawTextW", api_DrawTextW), E("FillRect", api_FillRect), E("FrameRect", api_FrameRect), E("InvertRect", api_InvertRect), E("DrawEdge", api_DrawEdge),
    E("LoadBitmapW", api_LoadBitmapW), E("LoadImageW", api_LoadImageW), E("LoadIconW", api_LoadIconW), E("LoadStringW", api_LoadStringW), E("DrawIcon", api_DrawIcon), E("DrawIconEx", api_DrawIcon),
    E("GetSysColor", api_GetSysColor), E("GetSysColorBrush", api_GetSysColorBrush),
};
void gdi_init_api(void) { gdi_init(); api_register(gdefs, sizeof gdefs / sizeof gdefs[0]); (void)api_NoOp1; }
