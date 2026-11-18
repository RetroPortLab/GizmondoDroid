#pragma once
#include "hle.h"
#include <math.h>

typedef struct Bmp {
    int w, h, bpp, pitch, topdown, dib, owned, fmt565;
    uint32_t bits;               /* guest address of pixel data */
    uint32_t pal[256];           /* COLORREF palette for <= 8 bpp */
} Bmp;

typedef struct DC {
    Bmp *bmp; uint32_t hbmp;
    uint32_t brush, pen, font, tc, bk;
    int bkmode, rop2, mono_default, screen;
    int ox, oy, vx, vy;            /* origin of this DC inside the bitmap, viewport origin */
    int cl_l, cl_t, cl_r, cl_b;    /* clip rectangle in bitmap coordinates */
    int cx, cy;                    /* current position */
    uint32_t hwnd;
} DC;

extern Bmp screen_bmp;
void     gdi_init(void);
DC      *dc_get(uint32_t h);
uint32_t gdi_make_dc(Bmp *target, uint32_t hbmp);
uint32_t gdi_screen_dc(int ox, int oy, int cl, int ct, int cr, int cb);
void     dc_fill(DC *d, int l, int t, int r, int b, uint32_t colorref);
void     dc_fill_brush(DC *d, uint32_t hbr, int l, int t, int r, int b);
uint32_t bmp_get(const Bmp *b, int x, int y);
void     bmp_put(Bmp *b, int x, int y, uint32_t colorref);
uint32_t gdi_syscolor(uint32_t index);
uint32_t gdi_syscolor_brush(uint32_t index);
