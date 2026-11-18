/* GizmondoDroid - kernel call slots, RECT/resource/menu helpers, single-precision soft-float, ctype extras */
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <wctype.h>

/* ------------------------------------------------------------ CE kernel call slots (0xF0010000 - 4*(set<<8|method)) */
void psl_call(uint32_t slot) {
    static int logged[64]; static int nlogged;
    switch (slot) {
    case 0x202:   /* process set, method 2: TerminateProcess(hProc, code) */
        hle_exit_code = (int)cpu.r[1]; hle_running = 0; api_flow_changed = 1; cpu.halted = 1; return;
    default: {
        for (int i = 0; i < nlogged; i++) if (logged[i] == (int)slot) { R(0); return; }
        if (nlogged < 64) logged[nlogged++] = (int)slot;
        hle_log("UNIMPLEMENTED kernel call set %u method %u (r0=%08x r1=%08x) from %08x", slot >> 8, slot & 255, cpu.r[0], cpu.r[1], cpu.r[14]);
        R(0);
    }
    }
}

/* ------------------------------------------------------------ RECT / POINT */
static int32_t rd(uint32_t a, int i) { return (int32_t)r32(a + 4 * i); }
static void wr(uint32_t a, int i, int32_t v) { w32(a + 4 * i, (uint32_t)v); }
static void api_SetRect(void) { wr(A(0), 0, (int32_t)A(1)); wr(A(0), 1, (int32_t)A(2)); wr(A(0), 2, (int32_t)A(3)); wr(A(0), 3, (int32_t)A(4)); R(1); }
static void api_SetRectEmpty(void) { for (int i = 0; i < 4; i++) wr(A(0), i, 0); R(1); }
static void api_CopyRect(void) { for (int i = 0; i < 4; i++) wr(A(0), i, rd(A(1), i)); R(1); }
static void api_EqualRect(void) { R(rd(A(0), 0) == rd(A(1), 0) && rd(A(0), 1) == rd(A(1), 1) && rd(A(0), 2) == rd(A(1), 2) && rd(A(0), 3) == rd(A(1), 3)); }
static void api_IsRectEmpty(void) { R(rd(A(0), 2) <= rd(A(0), 0) || rd(A(0), 3) <= rd(A(0), 1)); }
static void api_InflateRect(void) { int dx = (int)A(1), dy = (int)A(2); uint32_t r = A(0); wr(r, 0, rd(r, 0) - dx); wr(r, 1, rd(r, 1) - dy); wr(r, 2, rd(r, 2) + dx); wr(r, 3, rd(r, 3) + dy); R(1); }
static void api_OffsetRect(void) { int dx = (int)A(1), dy = (int)A(2); uint32_t r = A(0); wr(r, 0, rd(r, 0) + dx); wr(r, 1, rd(r, 1) + dy); wr(r, 2, rd(r, 2) + dx); wr(r, 3, rd(r, 3) + dy); R(1); }
static void api_IntersectRect(void) {
    int32_t l = rd(A(1), 0) > rd(A(2), 0) ? rd(A(1), 0) : rd(A(2), 0), t = rd(A(1), 1) > rd(A(2), 1) ? rd(A(1), 1) : rd(A(2), 1);
    int32_t r = rd(A(1), 2) < rd(A(2), 2) ? rd(A(1), 2) : rd(A(2), 2), b = rd(A(1), 3) < rd(A(2), 3) ? rd(A(1), 3) : rd(A(2), 3);
    if (l >= r || t >= b) { for (int i = 0; i < 4; i++) wr(A(0), i, 0); R(0); return; }
    wr(A(0), 0, l); wr(A(0), 1, t); wr(A(0), 2, r); wr(A(0), 3, b); R(1);
}
static void api_UnionRect(void) {
    int32_t l = rd(A(1), 0) < rd(A(2), 0) ? rd(A(1), 0) : rd(A(2), 0), t = rd(A(1), 1) < rd(A(2), 1) ? rd(A(1), 1) : rd(A(2), 1);
    int32_t r = rd(A(1), 2) > rd(A(2), 2) ? rd(A(1), 2) : rd(A(2), 2), b = rd(A(1), 3) > rd(A(2), 3) ? rd(A(1), 3) : rd(A(2), 3);
    wr(A(0), 0, l); wr(A(0), 1, t); wr(A(0), 2, r); wr(A(0), 3, b); R(1);
}
static void api_PtInRect(void) { int x = (int)A(1), y = (int)A(2); R(x >= rd(A(0), 0) && x < rd(A(0), 2) && y >= rd(A(0), 1) && y < rd(A(0), 3)); }
static void api_MapWindowPoints(void) { R(0); }
static void api_one(void) { R(1); }
static void api_zero(void) { R(0); }

/* ------------------------------------------------------------ resources */
static struct { uint32_t p, size; } res_tab[128]; static int nres;
static void api_FindResourceW(void) {
    Module *m = A(0) ? pe_find(A(0)) : main_module; uint32_t sz = 0;
    if (!m) { R(0); return; }
    uint32_t p = pe_resource(m, A(2), A(1), &sz);
    if (!p) { R(0); kern_set_error(1814); return; }
    for (int i = 0; i < nres; i++) if (res_tab[i].p == p) { R(p); return; }
    if (nres < 128) { res_tab[nres].p = p; res_tab[nres].size = sz; nres++; }
    R(p);
}
static void api_LoadResource(void) { R(A(1)); }
static void api_LockResource(void) { R(A(0)); }
static void api_SizeofResource(void) { for (int i = 0; i < nres; i++) if (res_tab[i].p == A(1)) { R(res_tab[i].size); return; } R(0); }

/* ------------------------------------------------------------ misc CRT */
static FILE *std_files[3];
static uint32_t std_handles[3];
static void api_getstdfilex(void) {
    uint32_t i = A(0); if (i > 2) { R(0); return; }
    if (!std_handles[i] || h_type(std_handles[i]) != H_MISC) { std_files[0] = stdin; std_files[1] = stdout; std_files[2] = stderr; std_handles[i] = h_new(H_MISC, std_files[i]); }
    R(std_handles[i]);
}
static int ctype_match(int c, uint32_t m) {
    if (c < 0 || c > 255) return 0;
    return ((m & 1) && isupper(c)) || ((m & 2) && islower(c)) || ((m & 4) && isdigit(c)) || ((m & 8) && isspace(c)) || ((m & 0x10) && ispunct(c)) || ((m & 0x20) && iscntrl(c)) ||
           ((m & 0x40) && (c == ' ' || c == '\t')) || ((m & 0x80) && isxdigit(c)) || ((m & 0x100) && isalpha(c));
}
static void api_isctype(void) { R(ctype_match((int)A(0), A(1))); }
static void api_ExtEscape(void) {   /* hdc, esc, cbIn, in, cbOut, out */
    uint32_t esc = A(1);
    if (esc == 8 /* QUERYESCSUPPORT */) { R(A(3) && (r32(A(3)) == 0x20001 || r32(A(3)) == 1)); return; }
    if (esc == 0x20001 /* GETRAWFRAMEBUFFER */ && A(5)) {
        uint32_t o = A(5);
        w16(o, 1); w16(o + 2, 16); w32(o + 4, FB_ADDR); w32(o + 8, 2); w32(o + 12, SCREEN_W * 2); w32(o + 16, SCREEN_W); w32(o + 20, SCREEN_H);
        R(1); return;
    }
    R(0);
}

/* ------------------------------------------------------------ single precision soft-float */
#define FBIN(nm, op) static void api___##nm(void) { R_float(F_arg(0) op F_arg(1)); }
FBIN(adds, +) FBIN(subs, -) FBIN(muls, *) FBIN(divs, /)
#define FCMP(nm, op) static void api___##nm(void) { R(F_arg(0) op F_arg(1)); }
FCMP(eqs, ==) FCMP(nes, !=) FCMP(lts, <) FCMP(les, <=) FCMP(gts, >) FCMP(ges, >=)
static void api___negs(void) { R_float(-F_arg(0)); }
static void api___stoi(void) { float f = F_arg(0); R(f >= 2147483647.0f ? 0x7FFFFFFFu : f <= -2147483648.0f ? 0x80000000u : (uint32_t)(int32_t)f); }
static void api___stou(void) { float f = F_arg(0); R(f <= 0 ? 0 : (uint32_t)f); }
static void api___itos(void) { R_float((float)(int32_t)A(0)); }
static void api___utos(void) { R_float((float)A(0)); }
static void api___dtos(void) { R_float((float)D_arg(0)); }
static void api___stod(void) { R_double((double)F_arg(0)); }
static void api___stoi64(void) { R64((uint64_t)(int64_t)F_arg(0)); }
static void api___stou64(void) { R64((uint64_t)F_arg(0)); }
static void api___i64tos(void) { R_float((float)(int64_t)(cpu.r[0] | ((uint64_t)cpu.r[1] << 32))); }

#define E(n, f) { n, f }
static const ApiDef udefs[] = {
    E("SetRect", api_SetRect), E("SetRectEmpty", api_SetRectEmpty), E("CopyRect", api_CopyRect), E("EqualRect", api_EqualRect), E("IsRectEmpty", api_IsRectEmpty),
    E("InflateRect", api_InflateRect), E("OffsetRect", api_OffsetRect), E("IntersectRect", api_IntersectRect), E("UnionRect", api_UnionRect), E("PtInRect", api_PtInRect),
    E("MapWindowPoints", api_MapWindowPoints), E("ClipCursor", api_one),
    E("FindResourceW", api_FindResourceW), E("LoadResource", api_LoadResource), E("LockResource", api_LockResource), E("SizeofResource", api_SizeofResource),
    E("TranslateAcceleratorW", api_zero), E("LoadAcceleratorsW", api_one), E("CheckMenuItem", api_zero), E("EnableMenuItem", api_zero), E("SetMenuItemInfoW", api_one), E("GetMenuItemInfoW", api_zero),
    E("DialogBoxIndirectParamW", api_zero), E("DialogBoxW", api_zero), E("CreateDialogIndirectParamW", api_zero), E("EndDialog", api_one), E("FlushInstructionCache", api_one),
    E("RegisterHotKey", api_one), E("UnregisterHotKey", api_one), E("UnregisterFunc1", api_one),
    E("_getstdfilex", api_getstdfilex), E("_isctype", api_isctype), E("iswctype", api_isctype), E("_wfreopen", api_zero), E("ExtEscape", api_ExtEscape),
    E("__adds", api___adds), E("__subs", api___subs), E("__muls", api___muls), E("__divs", api___divs), E("__negs", api___negs),
    E("__eqs", api___eqs), E("__nes", api___nes), E("__lts", api___lts), E("__les", api___les), E("__gts", api___gts), E("__ges", api___ges),
    E("__stoi", api___stoi), E("__stou", api___stou), E("__itos", api___itos), E("__utos", api___utos), E("__dtos", api___dtos), E("__stod", api___stod), E("__stoi64", api___stoi64), E("__stou64", api___stou64), E("__i64tos", api___i64tos),
};
void api_user_init(void) { nres = 0; memset(std_handles, 0, sizeof std_handles); api_register(udefs, sizeof udefs / sizeof udefs[0]); }
