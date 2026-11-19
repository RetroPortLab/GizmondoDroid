/* GizmondoDroid - software OpenGL ES 1.x (libGLES_CM.dll) + EGL, rendering into the shared RGB565 framebuffer.
 * Fixed-function pipeline: matrices, vertex/color/texcoord arrays, textures (incl. paletted), blending,
 * alpha/depth test, culling, fog. Texture unit 0 only; lighting is not applied (vertex colour is used). */
#include "hle.h"
#include <math.h>
#include <stdlib.h>

typedef struct { int w, h; uint32_t *px; int magl, minl, wraps, wrapt, alpha; } Tex;
typedef struct { float x, y, z, w, r, g, b, a, u, v, fog; int clipflags; } Vtx;
typedef struct { int on, size, type, stride; uint32_t ptr; } Arr;

#define MAXTEX 2048
static Tex *texs[MAXTEX];
static int next_tex = 1;
static int bound[2], cur_unit, cur_cunit;
static float mv[16], pj[16], tm[16], mstack[3][16][16];
static int sp[3], mode_idx;
static Arr va, ca, ta;
static float curcol[4] = { 1, 1, 1, 1 }, clearcol[3];
static int en_depth, en_blend, en_alpha, en_tex, en_fog, en_cull, depth_mask = 1, depth_func = 0x201;
static int blend_s = 1, blend_d = 0, alpha_func = 0x207, front_ccw = 1, cull_mode = 0x405;
static float alpha_ref;
static int vp[4] = { 0, 0, SCREEN_W, SCREEN_H };
static int fog_mode = 0x800, fogcol_set;
static float fog_density = 1, fog_start, fog_end = 1, fog_col[3];
static float zbuf[SCREEN_W * SCREEN_H];
static uint32_t verstr[4];
static int warned_fmt[64];

static float fx(uint32_t v) { return (int32_t)v / 65536.0f; }
static float fb2f(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static void ident(float *m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1; }
static float *curm(void) { return mode_idx == 0 ? mv : mode_idx == 1 ? pj : tm; }
static void mmul(const float *a, const float *b, float *o) {   /* o = a*b, column-major */
    float t[16];
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) {
        float s = 0; for (int k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
        t[c * 4 + r] = s;
    }
    memcpy(o, t, 64);
}

/* ---------------------------------------------------------------- EGL */
static void e_GetDisplay(void) { R(1); }
static void e_Initialize(void) { if (A(1)) w32(A(1), 1); if (A(2)) w32(A(2), 4); R(1); }
static void e_ChooseConfig(void) { if (A(2) && A(3) > 0) w32(A(2), 1); if (A(4)) w32(A(4), 1); R(1); }
static void e_CreateWindowSurface(void) { R(2); }
static void e_CreateContext(void) { R(3); }
static void e_MakeCurrent(void) { R(1); }
extern void kern_vsync(void);
/* Frame skipping: when the host is behind real time it asks for the next frame(s) not to be rasterised (game logic and audio keep running). */
int gles_skip_request, gles_skip_max = 2, gles_opt_filter, gles_opt_fog = 1, gles_opt_chunk = 8;
static uint16_t backbuf[320 * 240];   /* games render here; copied to the screen on eglSwapBuffers so half-drawn frames are never shown (no flicker) */
static int skipping, skip_run;
static void e_SwapBuffers(void) {
    if (!skipping) { uint16_t *fb = fb_host(); if (fb) memcpy(fb, backbuf, sizeof backbuf); frame_dirty = 1; }
    skipping = gles_skip_request && skip_run < gles_skip_max;
    skip_run = skipping ? skip_run + 1 : 0;
    R(1); kern_vsync();
}

/* ---------------------------------------------------------------- state */
static void g_MatrixMode(void) { uint32_t m = A(0); mode_idx = m == 0x1701 ? 1 : m == 0x1702 ? 2 : 0; }
static void g_LoadIdentity(void) { ident(curm()); }
static void g_LoadMatrixx(void) { float *m = curm(); for (int i = 0; i < 16; i++) m[i] = fx(r32(A(0) + 4 * i)); }
static void g_PushMatrix(void) { if (sp[mode_idx] < 16) memcpy(mstack[mode_idx][sp[mode_idx]++], curm(), 64); }
static void g_PopMatrix(void) { if (sp[mode_idx] > 0) memcpy(curm(), mstack[mode_idx][--sp[mode_idx]], 64); }
static void g_Scalef(void) {
    float s[3] = { fb2f(A(0)), fb2f(A(1)), fb2f(A(2)) }, *m = curm();
    for (int c = 0; c < 3; c++) for (int r = 0; r < 4; r++) m[c * 4 + r] *= s[c];
}
static void g_Viewport(void) { vp[0] = (int)A(0); vp[1] = (int)A(1); vp[2] = (int)A(2); vp[3] = (int)A(3); }
static void cap(uint32_t c, int on) {
    switch (c) {
    case 0x0B71: en_depth = on; break; case 0x0BE2: en_blend = on; break; case 0x0BC0: en_alpha = on; break;
    case 0x0DE1: if (cur_unit == 0) en_tex = on; break; case 0x0B60: en_fog = on; break; case 0x0B44: en_cull = on; break;
    default: break;
    }
}
static void g_Enable(void) { cap(A(0), 1); }
static void g_Disable(void) { cap(A(0), 0); }
static Arr *arr_for(uint32_t c) { return c == 0x8074 ? &va : c == 0x8076 ? &ca : (c == 0x8078 && cur_cunit == 0) ? &ta : NULL; }
static void g_EnableClientState(void) { Arr *a = arr_for(A(0)); if (a) a->on = 1; }
static void g_DisableClientState(void) { Arr *a = arr_for(A(0)); if (a) a->on = 0; }
static void setptr(Arr *a) { a->size = (int)A(0); a->type = (int)A(1); a->stride = (int)A(2); a->ptr = A(3); }
static void g_VertexPointer(void) { setptr(&va); }
static void g_ColorPointer(void) { setptr(&ca); }
static void g_TexCoordPointer(void) { if (cur_cunit == 0) setptr(&ta); }
static void g_ActiveTexture(void) { cur_unit = A(0) == 0x84C1 ? 1 : 0; }
static void g_ClientActiveTexture(void) { cur_cunit = A(0) == 0x84C1 ? 1 : 0; }
static void g_Color4f(void) { for (int i = 0; i < 4; i++) curcol[i] = fb2f(A(i)); }
static void g_Color4x(void) { for (int i = 0; i < 4; i++) curcol[i] = fx(A(i)); }
static void g_ClearColorx(void) { for (int i = 0; i < 3; i++) clearcol[i] = fx(A(i)); }
static void g_DepthFunc(void) { depth_func = (int)A(0); }
static void g_DepthMask(void) { depth_mask = A(0) != 0; }
static void g_BlendFunc(void) { blend_s = (int)A(0); blend_d = (int)A(1); }
static void g_AlphaFuncx(void) { alpha_func = (int)A(0); alpha_ref = fx(A(1)); if (getenv("GIZ_GLLOG")) hle_log("AlphaFuncx %x %f", alpha_func, alpha_ref); }
static void g_FrontFace(void) { front_ccw = A(0) == 0x0901; }
static void g_CullFace(void) { cull_mode = (int)A(0); }
static void g_Finish(void) { }
static void g_Fogxv(void) {
    uint32_t p = A(0), a = A(1);
    switch (p) {
    case 0x0B62: fog_density = fx(r32(a)); break; case 0x0B63: fog_start = fx(r32(a)); break;
    case 0x0B64: fog_end = fx(r32(a)); break; case 0x0B65: fog_mode = (int)r32(a); break;
    case 0x0B66: for (int i = 0; i < 3; i++) fog_col[i] = fx(r32(a + 4 * i)); break;
    }
}
static void g_Lightxv(void) { }
static void g_GetString(void) {
    uint32_t n = A(0) - 0x1F00;
    if (n > 3) { R(0); return; }
    if (!verstr[n]) { static const char *s[4] = { "GizmondoDroid", "GizmondoDroid software GLES", "OpenGL ES-CM 1.1", "" }; verstr[n] = gs_new(s[n]); }
    R(verstr[n]);
}

/* ---------------------------------------------------------------- textures */
static Tex *tex_get(int id, int create) {
    if (id <= 0 || id >= MAXTEX) return NULL;
    if (!texs[id] && create) { texs[id] = calloc(1, sizeof(Tex)); texs[id]->magl = texs[id]->minl = 0; texs[id]->wraps = texs[id]->wrapt = 0x2901; }
    return texs[id];
}
static void g_GenTextures(void) { for (int i = 0; i < (int)A(0); i++) { int id = next_tex++ % MAXTEX; if (!id) id = next_tex++; tex_get(id, 1); w32(A(1) + 4 * i, (uint32_t)id); } }
static void g_DeleteTextures(void) {
    for (int i = 0; i < (int)A(0); i++) { int id = (int)r32(A(1) + 4 * i); if (id > 0 && id < MAXTEX && texs[id]) { free(texs[id]->px); free(texs[id]); texs[id] = NULL; } }
}
static void g_BindTexture(void) { bound[cur_unit] = (int)A(1); tex_get(bound[cur_unit], 1); }
static void g_TexParameterx(void) {
    Tex *t = tex_get(bound[cur_unit], 0); if (!t) return;
    uint32_t p = A(1), v = A(2);
    if (p == 0x2800) t->magl = v == 0x2601; else if (p == 0x2801) t->minl = (v == 0x2601 || v == 0x2701 || v == 0x2703);
    else if (p == 0x2802) t->wraps = (int)v; else if (p == 0x2803) t->wrapt = (int)v;
}
static uint32_t pack(int r, int g, int b, int a) { return (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b; }
static uint32_t px16(uint16_t p, int fmt) {
    int r, g, b, a = 255;
    if (fmt == 0x8363) { r = (p >> 11) * 255 / 31; g = ((p >> 5) & 63) * 255 / 63; b = (p & 31) * 255 / 31; }
    else if (fmt == 0x8033) { r = (p >> 12) * 17; g = ((p >> 8) & 15) * 17; b = ((p >> 4) & 15) * 17; a = (p & 15) * 17; }
    else { r = (p >> 11) * 255 / 31; g = ((p >> 6) & 31) * 255 / 31; b = ((p >> 1) & 31) * 255 / 31; a = (p & 1) * 255; }
    return pack(r, g, b, a);
}

/* S3TC / DXT decode (level 0 only) */
static void dxt_decode(uint32_t src, int w, int h, int kind, uint32_t *out, int *alpha) {
    int bw = (w + 3) / 4, bh = (h + 3) / 4, bsz = (kind == 1 || kind == 0) ? 8 : 16;
    *alpha = kind != 0;
    for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
        uint32_t b = src + (uint32_t)((by * bw + bx) * bsz);
        uint32_t cb = (bsz == 16) ? b + 8 : b;
        uint16_t c0 = r16(cb), c1 = r16(cb + 2); uint32_t bits = r32(cb + 4);
        int col[4][4];
        int r0 = (c0 >> 11) * 255 / 31, g0 = ((c0 >> 5) & 63) * 255 / 63, b0 = (c0 & 31) * 255 / 31;
        int r1 = (c1 >> 11) * 255 / 31, g1 = ((c1 >> 5) & 63) * 255 / 63, b1 = (c1 & 31) * 255 / 31;
        col[0][0] = r0; col[0][1] = g0; col[0][2] = b0; col[0][3] = 255;
        col[1][0] = r1; col[1][1] = g1; col[1][2] = b1; col[1][3] = 255;
        if (c0 > c1 || bsz == 16) {
            for (int k = 0; k < 3; k++) { col[2][k] = (2 * col[0][k] + col[1][k]) / 3; col[3][k] = (col[0][k] + 2 * col[1][k]) / 3; }
            col[2][3] = col[3][3] = 255;
        } else {
            for (int k = 0; k < 3; k++) { col[2][k] = (col[0][k] + col[1][k]) / 2; col[3][k] = 0; }
            col[2][3] = 255; col[3][3] = (kind == 1) ? 0 : 255;
        }
        uint8_t al[16];
        for (int i = 0; i < 16; i++) al[i] = 255;
        if (kind == 2) for (int i = 0; i < 16; i++) al[i] = (uint8_t)(((r8(b + i / 2) >> ((i & 1) * 4)) & 15) * 17);
        else if (kind == 3) {
            int a0 = r8(b), a1 = r8(b + 1); uint64_t ab = 0;
            for (int i = 0; i < 6; i++) ab |= (uint64_t)r8(b + 2 + i) << (8 * i);
            int pal[8]; pal[0] = a0; pal[1] = a1;
            if (a0 > a1) for (int i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
            else { for (int i = 1; i < 5; i++) pal[i + 1] = ((5 - i) * a0 + i * a1) / 5; pal[6] = 0; pal[7] = 255; }
            for (int i = 0; i < 16; i++) al[i] = (uint8_t)pal[(ab >> (3 * i)) & 7];
        }
        for (int py = 0; py < 4; py++) for (int pxl = 0; pxl < 4; pxl++) {
            int x = bx * 4 + pxl, y = by * 4 + py; if (x >= w || y >= h) continue;
            int idx = (bits >> (2 * (py * 4 + pxl))) & 3;
            int a = bsz == 16 ? al[py * 4 + pxl] : col[idx][3];
            out[y * w + x] = pack(col[idx][0], col[idx][1], col[idx][2], a);
        }
    }
}
static void tex_store(Tex *t, int w, int h, uint32_t *px, int alpha) {
    if (getenv("GIZ_GLDUMP")) { int blk = 0; for (int i = 0; i < w * h; i++) if (px[i] == 0xFF000000u) blk++; if (blk > w * h / 2) hle_log("texture %d %dx%d is %d%% opaque black", (int)(t - (Tex *)0), w, h, blk * 100 / (w * h)); } free(t->px); t->px = px; t->w = w; t->h = h; t->alpha = alpha; }
static void g_TexImage2D(void) {
    if (getenv("GIZ_GLLOG")) hle_log("glTexImage2D lvl=%d ifmt=%x %dx%d fmt=%x type=%x tex=%d", (int)A(1), A(2), (int)A(3), (int)A(4), A(6), A(7), bound[cur_unit]);
    int level = (int)A(1), w = (int)A(3), h = (int)A(4); uint32_t fmt = A(6), ty = A(7), src = A(8);
    Tex *t = tex_get(bound[cur_unit], 0);
    if (!t || level != 0 || w <= 0 || h <= 0 || w > 2048 || h > 2048) return;
    uint32_t *px = calloc((size_t)w * h, 4);
    int alpha = fmt == 0x1908 || fmt == 0x80E1 || fmt == 0x1906 || fmt == 0x190A;
    if (src) for (int i = 0; i < w * h; i++) {
        uint32_t c = 0xFF000000;
        if (ty == 0x1401) {
            switch (fmt) {
            case 0x1908: c = pack(r8(src + 4 * i), r8(src + 4 * i + 1), r8(src + 4 * i + 2), r8(src + 4 * i + 3)); break;
            case 0x80E1: c = pack(r8(src + 4 * i + 2), r8(src + 4 * i + 1), r8(src + 4 * i), r8(src + 4 * i + 3)); break;
            case 0x1907: c = pack(r8(src + 3 * i), r8(src + 3 * i + 1), r8(src + 3 * i + 2), 255); break;
            case 0x1909: { int l = r8(src + i); c = pack(l, l, l, 255); } break;
            case 0x190A: { int l = r8(src + 2 * i); c = pack(l, l, l, r8(src + 2 * i + 1)); } break;
            case 0x1906: c = pack(255, 255, 255, r8(src + i)); break;
            }
        } else if (ty == 0x8363 || ty == 0x8033 || ty == 0x8034) c = px16(r16(src + 2 * i), (int)ty);
        px[i] = c;
    } else for (int i = 0; i < w * h; i++) px[i] = 0xFFFFFFFF;
    tex_store(t, w, h, px, alpha);
}
static void g_CompressedTexImage2D(void) {
    if (getenv("GIZ_GLLOG")) hle_log("glCompressedTexImage2D lvl=%d fmt=%x %dx%d size=%u tex=%d", (int)A(1), A(2), (int)A(3), (int)A(4), A(6), bound[cur_unit]);
    int level = (int)A(1), w = (int)A(3), h = (int)A(4); uint32_t fmt = A(2), src = A(7);
    Tex *t = tex_get(bound[cur_unit], 0);
    if (!t || level > 0 || w <= 0 || h <= 0 || w > 2048 || h > 2048) return;
    uint32_t *px = malloc((size_t)w * h * 4);
    int alpha = 0;
    if (fmt >= 0x8B90 && fmt <= 0x8B99) {
        int eight = fmt >= 0x8B95, kind = (fmt - 0x8B90) % 5;   /* RGB8, RGBA8, 565, 4444, 5551 */
        int n = eight ? 256 : 16, psz = kind == 0 ? 3 : kind == 1 ? 4 : 2;
        uint32_t pal[256];
        for (int i = 0; i < n; i++) {
            uint32_t a = src + i * psz;
            switch (kind) {
            case 0: pal[i] = pack(r8(a), r8(a + 1), r8(a + 2), 255); break;
            case 1: pal[i] = pack(r8(a), r8(a + 1), r8(a + 2), r8(a + 3)); break;
            case 2: pal[i] = px16(r16(a), 0x8363); break;
            case 3: pal[i] = px16(r16(a), 0x8033); break;
            default: pal[i] = px16(r16(a), 0x8034); break;
            }
        }
        alpha = kind == 1 || kind == 3 || kind == 4;
        if (getenv("GIZ_GLDUMP") && w == 16) { hle_log("pal4 tex%d level=%d size=%u: pal0=%08x pal1=%08x pal2=%08x pal15=%08x idx0..7=%02x%02x%02x%02x%02x%02x%02x%02x", bound[cur_unit], (int)A(1), A(6), pal[0], pal[1], pal[2], pal[15], r8(src+n*psz), r8(src+n*psz+1), r8(src+n*psz+2), r8(src+n*psz+3), r8(src+n*psz+4), r8(src+n*psz+5), r8(src+n*psz+6), r8(src+n*psz+7)); }
        if (getenv("GIZ_GLLOG")) { int lo = 0; for (int i = 0; i < n; i++) if ((pal[i] >> 24) < 255) lo++; hle_log("pal tex%d %dx%d kind=%d palette entries with alpha<255: %d (pal0=%08x)", bound[cur_unit], w, h, kind, lo, pal[0]); }
        uint32_t idx = src + n * psz;
        for (int i = 0; i < w * h; i++) {
            int v = eight ? r8(idx + i) : ((r8(idx + i / 2) >> ((i & 1) ? 0 : 4)) & 15);
            px[i] = pal[v];
        }
    } else if (fmt >= 0x83F0 && fmt <= 0x83F3) {
        dxt_decode(src, w, h, fmt == 0x83F0 ? 0 : fmt == 0x83F1 ? 1 : fmt == 0x83F2 ? 2 : 3, px, &alpha);
    } else {
        if (fmt < 64 || 1) { int k = (int)(fmt & 63); if (!warned_fmt[k]) { warned_fmt[k] = 1; hle_log("GLES: unsupported compressed texture format 0x%x (%dx%d)", fmt, w, h); } }
        for (int i = 0; i < w * h; i++) px[i] = 0xFF808080;
    }
    tex_store(t, w, h, px, alpha);
}

/* ---------------------------------------------------------------- vertex fetch */
static float comp(const Arr *a, uint32_t addr, int i) {
    switch (a->type) {
    case 0x1406: return fb2f(r32(addr + 4 * i));
    case 0x140C: return fx(r32(addr + 4 * i));
    case 0x1402: return (int16_t)r16(addr + 2 * i);
    case 0x1403: return r16(addr + 2 * i);
    case 0x1400: return (int8_t)r8(addr + i);
    default: return r8(addr + i);
    }
}
static int tsize(int ty) { return ty == 0x1406 || ty == 0x140C ? 4 : (ty == 0x1402 || ty == 0x1403) ? 2 : 1; }
static float fogf(float d) {
    float f = fog_mode == 0x2601 ? (fog_end - d) / (fog_end - fog_start) : fog_mode == 0x800 ? expf(-fog_density * d) : expf(-(fog_density * d) * (fog_density * d));
    return f < 0 ? 0 : f > 1 ? 1 : f;
}
static void fetch(int i, Vtx *o) {
    float p[4] = { 0, 0, 0, 1 };
    uint32_t a = va.ptr + (uint32_t)i * (va.stride ? (uint32_t)va.stride : (uint32_t)(va.size * tsize(va.type)));
    for (int k = 0; k < va.size && k < 4; k++) p[k] = comp(&va, a, k);
    float e[4], c[4];
    for (int r = 0; r < 4; r++) e[r] = mv[r] * p[0] + mv[4 + r] * p[1] + mv[8 + r] * p[2] + mv[12 + r] * p[3];
    for (int r = 0; r < 4; r++) c[r] = pj[r] * e[0] + pj[4 + r] * e[1] + pj[8 + r] * e[2] + pj[12 + r] * e[3];
    o->x = c[0]; o->y = c[1]; o->z = c[2]; o->w = c[3];
    if (ca.on) {
        uint32_t ca_a = ca.ptr + (uint32_t)i * (ca.stride ? (uint32_t)ca.stride : (uint32_t)(ca.size * tsize(ca.type)));
        float s = (ca.type == 0x1401) ? 1 / 255.0f : 1.0f;
        o->r = comp(&ca, ca_a, 0) * s; o->g = comp(&ca, ca_a, 1) * s; o->b = comp(&ca, ca_a, 2) * s; o->a = ca.size > 3 ? comp(&ca, ca_a, 3) * s : 1;
    } else { o->r = curcol[0]; o->g = curcol[1]; o->b = curcol[2]; o->a = curcol[3]; }
    o->u = o->v = 0;
    if (ta.on) {
        uint32_t t = ta.ptr + (uint32_t)i * (ta.stride ? (uint32_t)ta.stride : (uint32_t)(ta.size * tsize(ta.type)));
        float s = comp(&ta, t, 0), tt = ta.size > 1 ? comp(&ta, t, 1) : 0;
        o->u = tm[0] * s + tm[4] * tt + tm[12]; o->v = tm[1] * s + tm[5] * tt + tm[13];
    }
    o->fog = en_fog ? fogf(fabsf(c[3])) : 1;
}

/* ---------------------------------------------------------------- rasteriser */
typedef struct { float x, y, z, iw, r, g, b, a, uw, vw, fog; } SV;   /* screen vertex */

static void to_screen(const Vtx *v, SV *s) {
    float iw = 1.0f / v->w;
    s->x = vp[0] + (v->x * iw * 0.5f + 0.5f) * vp[2];
    s->y = (SCREEN_H - vp[1] - vp[3]) + (1 - (v->y * iw * 0.5f + 0.5f)) * vp[3];
    s->z = v->z * iw * 0.5f + 0.5f; s->iw = iw;
    s->r = v->r; s->g = v->g; s->b = v->b; s->a = v->a; s->uw = v->u * iw; s->vw = v->v * iw; s->fog = v->fog;
}

static inline uint32_t texel(const Tex *t, int x, int y) {
    if (t->wraps == 0x812F) x = x < 0 ? 0 : x >= t->w ? t->w - 1 : x; else { x %= t->w; if (x < 0) x += t->w; }
    if (t->wrapt == 0x812F) y = y < 0 ? 0 : y >= t->h ? t->h - 1 : y; else { y %= t->h; if (y < 0) y += t->h; }
    return t->px[y * t->w + x];
}

static int getenv_noalpha;
static int zpass(float z, float old) {
    switch (depth_func) {
    case 0x200: return 0; case 0x201: return z < old; case 0x202: return z == old; case 0x203: return z <= old;
    case 0x204: return z > old; case 0x205: return z != old; case 0x206: return z >= old; default: return 1;
    }
}
static float bfac(int f, float sc, float sa, float dc, float da, int sat) {
    switch (f) {
    case 0: return 0; case 1: return 1; case 0x300: return sc; case 0x301: return 1 - sc; case 0x302: return sa; case 0x303: return 1 - sa;
    case 0x304: return da; case 0x305: return 1 - da; case 0x306: return dc; case 0x307: return 1 - dc;
    default: return sat ? (sa < 1 - da ? sa : 1 - da) : 1;
    }
}

static void raster(const SV *a, const SV *b, const SV *c) {
    if (skipping) return;
    float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);   /* screen y is down */
    if (area == 0 || area != area) return;
    int front = (area < 0) == (front_ccw != 0);          /* CCW in GL (y up) is negative area with y down */
    if (en_cull) {
        if (cull_mode == 0x0408) return;
        if ((cull_mode == 0x0405 && !front) || (cull_mode == 0x0404 && front)) return;
    }
    float minx = fminf(a->x, fminf(b->x, c->x)), maxx = fmaxf(a->x, fmaxf(b->x, c->x));
    float miny = fminf(a->y, fminf(b->y, c->y)), maxy = fmaxf(a->y, fmaxf(b->y, c->y));
    int x0 = (int)fmaxf(floorf(minx), 0), x1 = (int)fminf(ceilf(maxx), SCREEN_W - 1);
    int y0 = (int)fmaxf(floorf(miny), 0), y1 = (int)fminf(ceilf(maxy), SCREEN_H - 1);
    if (x0 > x1 || y0 > y1) return;
    float inv = 1.0f / area;
    const Tex *tex = (en_tex && ta.on) ? tex_get(bound[0], 0) : NULL;
    if (tex && !tex->px) tex = NULL;
    uint16_t *fb = backbuf;
    if (!fb) return;
    /* attribute planes: f(x,y) = f0 + gx*(x-a.x) + gy*(y-a.y) */
    enum { N_Z, N_IW, N_U, N_V, N_R, N_G, N_B, N_A, N_F, NATT };
    float f0[NATT] = { a->z, a->iw, a->uw, a->vw, a->r, a->g, a->b, a->a, a->fog };
    float f1[NATT] = { b->z, b->iw, b->uw, b->vw, b->r, b->g, b->b, b->a, b->fog };
    float f2[NATT] = { c->z, c->iw, c->uw, c->vw, c->r, c->g, c->b, c->a, c->fog };
    float gx[NATT], gy[NATT];
    float bax = b->x - a->x, bay = b->y - a->y, cax = c->x - a->x, cay = c->y - a->y;
    for (int k = 0; k < NATT; k++) {
        float d1 = f1[k] - f0[k], d2 = f2[k] - f0[k];
        gx[k] = (d1 * cay - d2 * bay) * inv;
        gy[k] = (d2 * bax - d1 * cax) * inv;
    }
    /* barycentric l_i(px,py) = li0 + lix*px + liy*py, used only for the coverage span */
    float L[3][3];   /* {const, dx, dy} */
    {
        const SV *v[3] = { a, b, c };
        for (int i = 0; i < 3; i++) {
            const SV *p = v[(i + 1) % 3], *q = v[(i + 2) % 3];   /* edge opposite vertex i */
            float dx = (p->y - q->y) * inv, dy = (q->x - p->x) * inv;
            L[i][1] = dx; L[i][2] = dy; L[i][0] = (p->x * q->y - q->x * p->y) * inv;
        }
    }
    int pm_s = tex ? tex->w : 0, pm_t = tex ? tex->h : 0;
    int pow2 = tex && !(pm_s & (pm_s - 1)) && !(pm_t & (pm_t - 1));
    for (int y = y0; y <= y1; y++) {
        float py = y + 0.5f;
        float lo = (float)x0 + 0.5f, hi = (float)x1 + 0.5f;
        int empty = 0;
        for (int i = 0; i < 3; i++) {
            float A0 = L[i][0] + L[i][2] * py, B = L[i][1];
            if (B > 1e-12f) { float t = -A0 / B; if (t > lo) lo = t; }
            else if (B < -1e-12f) { float t = -A0 / B; if (t < hi) hi = t; }
            else if (A0 < 0) { empty = 1; break; }
        }
        if (empty) continue;
        int xs = (int)ceilf(lo - 0.5f - 0.004f), xe = (int)floorf(hi - 0.5f + 0.004f);   /* tiny conservative bias: shared edges must never leave gaps */
        if (xs < x0) xs = x0; if (xe > x1) xe = x1;
        if (xs > xe) continue;
        float dy_ = py - a->y, dx_ = (xs + 0.5f) - a->x, v[NATT];
        for (int k = 0; k < NATT; k++) v[k] = f0[k] + gx[k] * dx_ + gy[k] * dy_;
        int pi = y * SCREEN_W + xs, chunk = 0; float cu = 0, cv = 0, dcu = 0, dcv = 0;
        for (int x = xs; x <= xe; x++, pi++) {
            float z = v[N_Z];
            if (z >= 0 && z <= 1 && (!en_depth || zpass(z, zbuf[pi]))) {
                float r = v[N_R], g = v[N_G], bl = v[N_B], al = v[N_A];
                int keep = 1;
                if (tex) {
                    if (chunk == 0) {   /* exact perspective division every 8 pixels, linear in between */
                        int n = xe - x + 1; if (n > gles_opt_chunk) n = gles_opt_chunk; chunk = n;
                        float w0 = 1.0f / v[N_IW];
                        cu = v[N_U] * w0 * tex->w; cv = v[N_V] * w0 * tex->h;
                        float iw1 = v[N_IW] + gx[N_IW] * n, w1 = iw1 > 1e-9f ? 1.0f / iw1 : w0;
                        dcu = ((v[N_U] + gx[N_U] * n) * w1 * tex->w - cu) / n; dcv = ((v[N_V] + gx[N_V] * n) * w1 * tex->h - cv) / n;
                    }
                    float fu = cu, fv = cv;
                    float tr, tg, tb, ta_;
                    if (tex->magl && gles_opt_filter != 1 || gles_opt_filter == 2) {
                        fu -= 0.5f; fv -= 0.5f;
                        int ix = (int)floorf(fu), iy = (int)floorf(fv); float fx_ = fu - ix, fy = fv - iy;
                        uint32_t p00 = texel(tex, ix, iy), p10 = texel(tex, ix + 1, iy), p01 = texel(tex, ix, iy + 1), p11 = texel(tex, ix + 1, iy + 1);
                        float w00 = (1 - fx_) * (1 - fy), w10 = fx_ * (1 - fy), w01 = (1 - fx_) * fy, w11 = fx_ * fy;
#define CH(sh) ((((p00 >> sh) & 255) * w00 + ((p10 >> sh) & 255) * w10 + ((p01 >> sh) & 255) * w01 + ((p11 >> sh) & 255) * w11) * (1.0f / 255.0f))
                        tr = CH(16); tg = CH(8); tb = CH(0); ta_ = CH(24);
#undef CH
                    } else {
                        int ix = (int)fu, iy = (int)fv; if (fu < 0) ix = (int)floorf(fu); if (fv < 0) iy = (int)floorf(fv);
                        uint32_t p;
                        if (pow2 && tex->wraps == 0x2901 && tex->wrapt == 0x2901) p = tex->px[(iy & (pm_t - 1)) * pm_s + (ix & (pm_s - 1))];
                        else p = texel(tex, ix, iy);
                        tr = ((p >> 16) & 255) * (1.0f / 255.0f); tg = ((p >> 8) & 255) * (1.0f / 255.0f); tb = (p & 255) * (1.0f / 255.0f); ta_ = (p >> 24) * (1.0f / 255.0f);
                    }
                    r *= tr; g *= tg; bl *= tb; al *= ta_;
                }
                if (en_alpha && !getenv_noalpha) {
                    float al8 = floorf(al * 255.0f + 0.5f) * (1.0f / 255.0f), ar8 = floorf(alpha_ref * 255.0f + 0.5f) * (1.0f / 255.0f);   /* compare as 8-bit like real hardware */
                    switch (alpha_func) {
                    case 0x200: keep = 0; break; case 0x201: keep = al8 < ar8; break; case 0x202: keep = al8 == ar8; break;
                    case 0x203: keep = al8 <= ar8; break; case 0x204: keep = al8 > ar8; break; case 0x205: keep = al8 != ar8; break;
                    case 0x206: keep = al8 >= ar8; break; default: keep = 1;
                    }
                }
                if (keep) {
                    if (en_fog && gles_opt_fog) { float f = v[N_F]; r = r * f + fog_col[0] * (1 - f); g = g * f + fog_col[1] * (1 - f); bl = bl * f + fog_col[2] * (1 - f); }
                    if (en_blend && !(blend_s == 1 && blend_d == 0)) {
                        uint16_t d = fb[pi];
                        float dr = (d >> 11) * (1.0f / 31.0f), dg = ((d >> 5) & 63) * (1.0f / 63.0f), db = (d & 31) * (1.0f / 31.0f);
                        float sr = bfac(blend_s, r, al, dr, 1, 1), sg = bfac(blend_s, g, al, dg, 1, 1), sb = bfac(blend_s, bl, al, db, 1, 1);
                        float tr2 = bfac(blend_d, r, al, dr, 1, 0), tg2 = bfac(blend_d, g, al, dg, 1, 0), tb2 = bfac(blend_d, bl, al, db, 1, 0);
                        r = r * sr + dr * tr2; g = g * sg + dg * tg2; bl = bl * sb + db * tb2;
                    }
                    int ir = (int)(r * 31.0f + 0.5f), ig = (int)(g * 63.0f + 0.5f), ib = (int)(bl * 31.0f + 0.5f);
                    ir = ir < 0 ? 0 : ir > 31 ? 31 : ir; ig = ig < 0 ? 0 : ig > 63 ? 63 : ig; ib = ib < 0 ? 0 : ib > 31 ? 31 : ib;
                    fb[pi] = (uint16_t)(ir << 11 | ig << 5 | ib);
                    if (depth_mask) zbuf[pi] = z;
                }
            }
            for (int k = 0; k < NATT; k++) v[k] += gx[k];
            if (tex) { chunk--; cu += dcu; cv += dcv; }
        }
    }
}

/* clip a triangle against the near plane (z >= -w) and rasterise */
static Vtx lerpv(const Vtx *a, const Vtx *b, float t) {
    Vtx o; float *pa = (float *)a, *pb = (float *)b, *po = (float *)&o;
    for (int i = 0; i < 11; i++) po[i] = pa[i] + (pb[i] - pa[i]) * t;
    return o;
}
static void tri(const Vtx *v0, const Vtx *v1, const Vtx *v2) {
    const Vtx *in[3] = { v0, v1, v2 };
    Vtx out[4]; int n = 0;
    for (int i = 0; i < 3; i++) {
        const Vtx *p = in[i], *q = in[(i + 1) % 3];
        float dp = p->z + p->w, dq = q->z + q->w;
        if (dp >= 0 && p->w > 1e-6f) out[n++] = *p;
        if ((dp >= 0) != (dq >= 0)) { float t = dp / (dp - dq); if (n < 4) out[n++] = lerpv(p, q, t); }
    }
    if (n < 3) return;
    SV s[4];
    for (int i = 0; i < n; i++) { if (out[i].w < 1e-6f) return; to_screen(&out[i], &s[i]); }
    raster(&s[0], &s[1], &s[2]);
    if (n == 4) raster(&s[0], &s[2], &s[3]);
}

static Vtx *vcache; static uint32_t *vstamp; static uint32_t vgen;
static void draw(int mode, int count, uint32_t idx, int ity, int first) {
    if (!va.on || count <= 0 || mode < 4) return;
    if (!vcache) { vcache = malloc(65536 * sizeof(Vtx)); vstamp = calloc(65536, 4); }
    vgen++;
    #define IDX(k) ((idx ? (ity == 0x1401 ? r8(idx + (k)) : r16(idx + 2 * (k))) : (first + (k))) & 0xFFFF)
    #define V(k) ({ int _i = IDX(k); if (vstamp[_i] != vgen) { fetch(_i, &vcache[_i]); vstamp[_i] = vgen; } &vcache[_i]; })
    if (mode == 4) for (int k = 0; k + 2 < count; k += 3) tri(V(k), V(k + 1), V(k + 2));
    else if (mode == 5) for (int k = 0; k + 2 < count; k++) { if (k & 1) tri(V(k + 1), V(k), V(k + 2)); else tri(V(k), V(k + 1), V(k + 2)); }
    else if (mode == 6) for (int k = 1; k + 1 < count; k++) tri(V(0), V(k), V(k + 1));
}
static void g_DrawElements(void) { draw((int)A(0), (int)A(1), A(3), (int)A(2), 0); }
static void g_DrawArrays(void) { draw((int)A(0), (int)A(2), 0, 0, (int)A(1)); }
static void g_Clear(void) {
    uint32_t m = A(0);
    uint16_t *fb = backbuf;
    if ((m & 0x4000) && fb && !skipping) {
        int r = (int)(clearcol[0] * 31 + 0.5f), g = (int)(clearcol[1] * 63 + 0.5f), b = (int)(clearcol[2] * 31 + 0.5f);
        uint16_t c = (uint16_t)(r << 11 | g << 5 | b);
        for (int i = 0; i < SCREEN_W * SCREEN_H; i++) fb[i] = c;
    }
    if (m & 0x100) for (int i = 0; i < SCREEN_W * SCREEN_H; i++) zbuf[i] = 1.0f;
}

#define E(n, f) { n, f }
static const ApiDef gdefs[] = {
    E("eglGetDisplay", e_GetDisplay), E("eglInitialize", e_Initialize), E("eglChooseConfig", e_ChooseConfig),
    E("eglCreateWindowSurface", e_CreateWindowSurface), E("eglCreateContext", e_CreateContext), E("eglMakeCurrent", e_MakeCurrent), E("eglSwapBuffers", e_SwapBuffers),
    E("glMatrixMode", g_MatrixMode), E("glLoadIdentity", g_LoadIdentity), E("glLoadMatrixx", g_LoadMatrixx), E("glPushMatrix", g_PushMatrix), E("glPopMatrix", g_PopMatrix),
    E("glScalef", g_Scalef), E("glViewport", g_Viewport), E("glEnable", g_Enable), E("glDisable", g_Disable),
    E("glEnableClientState", g_EnableClientState), E("glDisableClientState", g_DisableClientState),
    E("glVertexPointer", g_VertexPointer), E("glColorPointer", g_ColorPointer), E("glTexCoordPointer", g_TexCoordPointer),
    E("glActiveTexture", g_ActiveTexture), E("glClientActiveTexture", g_ClientActiveTexture),
    E("glColor4f", g_Color4f), E("glColor4x", g_Color4x), E("glClearColorx", g_ClearColorx), E("glClear", g_Clear),
    E("glDepthFunc", g_DepthFunc), E("glDepthMask", g_DepthMask), E("glBlendFunc", g_BlendFunc), E("glAlphaFuncx", g_AlphaFuncx),
    E("glFrontFace", g_FrontFace), E("glCullFace", g_CullFace), E("glFinish", g_Finish), E("glFogxv", g_Fogxv), E("glLightxv", g_Lightxv), E("glGetString", g_GetString),
    E("glGenTextures", g_GenTextures), E("glDeleteTextures", g_DeleteTextures), E("glBindTexture", g_BindTexture),
    E("glTexParameterx", g_TexParameterx), E("glTexImage2D", g_TexImage2D), E("glCompressedTexImage2D", g_CompressedTexImage2D),
    E("glDrawElements", g_DrawElements), E("glDrawArrays", g_DrawArrays),
};
void gles_init_api(void) {
    getenv_noalpha = getenv("GIZ_NOALPHA") != NULL;
    for (int i = 0; i < MAXTEX; i++) if (texs[i]) { free(texs[i]->px); free(texs[i]); texs[i] = NULL; }
    next_tex = 1; bound[0] = bound[1] = 0; cur_unit = cur_cunit = 0; mode_idx = 0;
    ident(mv); ident(pj); ident(tm); sp[0] = sp[1] = sp[2] = 0;
    memset(&va, 0, sizeof va); memset(&ca, 0, sizeof ca); memset(&ta, 0, sizeof ta);
    for (int i = 0; i < 4; i++) curcol[i] = 1;
    en_depth = en_blend = en_alpha = en_tex = en_fog = en_cull = 0; depth_mask = 1; depth_func = 0x201;
    blend_s = 1; blend_d = 0; alpha_func = 0x207; front_ccw = 1; cull_mode = 0x405;
    vp[0] = vp[1] = 0; vp[2] = SCREEN_W; vp[3] = SCREEN_H; fog_mode = 0x800; fog_density = 1; fog_start = 0; fog_end = 1;
    memset(verstr, 0, sizeof verstr);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) zbuf[i] = 1.0f;
    api_register(gdefs, sizeof gdefs / sizeof gdefs[0]);
}
