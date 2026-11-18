/* GizmondoDroid - C runtime pieces of coredll: printf engine, string/memory, math, soft-float, setjmp */
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>
#include <wctype.h>
#include <time.h>

/* ------------------------------------------------------------ printf engine */
static void put_utf8(char *o, int *n, int max, uint32_t c) {
    char t[4]; int k = 0;
    if (c < 0x80) t[k++] = (char)c;
    else if (c < 0x800) { t[k++] = 0xC0 | (c >> 6); t[k++] = 0x80 | (c & 63); }
    else { t[k++] = 0xE0 | (c >> 12); t[k++] = 0x80 | ((c >> 6) & 63); t[k++] = 0x80 | (c & 63); }
    for (int i = 0; i < k; i++) { if (*n < max - 1) o[*n] = t[i]; (*n)++; }
}

int hle_format(char *out, int outmax, const char *fmt, int wide_fmt, uint32_t va, int va_is_ptr, int first_arg) {
    int idx = first_arg, o = 0;
    uint32_t vp = va;
#define N32() (va_is_ptr ? (vp += 4, r32(vp - 4)) : A(idx++))
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') { if (o < outmax - 1) out[o] = *f; o++; continue; }
        f++;
        if (*f == '%') { if (o < outmax - 1) out[o] = '%'; o++; continue; }
        char spec[32]; int sl = 0, ll = 0, hh = 0, width = -1, prec = -1;
        spec[sl++] = '%';
        while (*f && strchr("-+ #0", *f)) { if (sl < 12) spec[sl++] = *f; f++; }
        if (*f == '*') { width = (int)N32(); f++; if (width < 0) { spec[sl++] = '-'; width = -width; } }
        else { width = 0; while (isdigit((unsigned char)*f)) { width = width * 10 + *f - '0'; f++; } }
        if (*f == '.') { f++; prec = 0; if (*f == '*') { prec = (int)N32(); f++; } else while (isdigit((unsigned char)*f)) { prec = prec * 10 + *f - '0'; f++; } }
        if (!strncmp(f, "I64", 3)) { ll = 1; f += 3; }
        else if (*f == 'l' && f[1] == 'l') { ll = 1; f += 2; }
        else if (*f == 'l' || *f == 'w' || *f == 'L') f++;
        else if (*f == 'h') { hh = 1; f++; }
        char conv = *f; if (!conv) break;
        char tmp[1100]; int tl = 0;
        if (width > 1000) width = 1000;
        snprintf(spec + sl, sizeof spec - sl, "%s", "");   /* width/prec appended below */
        char w[40]; snprintf(w, sizeof w, "%d", width < 0 ? 0 : width);
        char pr[40]; if (prec >= 0) snprintf(pr, sizeof pr, ".%d", prec > 1000 ? 1000 : prec); else pr[0] = 0;
        char hs[96];
        switch (conv) {
        case 'd': case 'i': {
            long long v;
            if (ll) {
                if (va_is_ptr) { vp = (vp + 7) & ~7u; uint32_t lo = r32(vp), hi = r32(vp + 4); vp += 8; v = (long long)(((uint64_t)hi << 32) | lo); }
                else { if (idx & 1) idx++; uint32_t lo = A(idx), hi = A(idx + 1); idx += 2; v = (long long)(((uint64_t)hi << 32) | lo); }
            } else { v = (int32_t)N32(); if (hh) v = (int16_t)v; }
            snprintf(hs, sizeof hs, "%s%s%slld", spec, w, pr);
            tl = snprintf(tmp, sizeof tmp, hs, v); break; }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long v;
            if (ll) {
                if (va_is_ptr) { vp = (vp + 7) & ~7u; uint32_t lo = r32(vp), hi = r32(vp + 4); vp += 8; v = ((uint64_t)hi << 32) | lo; }
                else { if (idx & 1) idx++; uint32_t lo = A(idx), hi = A(idx + 1); idx += 2; v = ((uint64_t)hi << 32) | lo; }
            } else { v = N32(); if (hh) v &= 0xFFFF; }
            snprintf(hs, sizeof hs, "%s%s%sll%c", spec, w, pr, conv);
            tl = snprintf(tmp, sizeof tmp, hs, v); break; }
        case 'p': { uint32_t v = N32(); tl = snprintf(tmp, sizeof tmp, "%08X", v); break; }
        case 'c': case 'C': {
            uint32_t v = N32(); char cs[8]; int cn = 0;
            if ((conv == 'c') == (wide_fmt != 0)) put_utf8(cs, &cn, 8, v & 0xFFFF); else cs[cn++] = (char)v;
            cs[cn] = 0;
            snprintf(hs, sizeof hs, "%s%ss", spec, w);
            tl = snprintf(tmp, sizeof tmp, hs, cs); break; }
        case 's': case 'S': {
            uint32_t p = N32(); char sb[1024];
            int wide = (conv == 's') == (wide_fmt != 0);
            if (!p) snprintf(sb, sizeof sb, "(null)");
            else if (wide) gw_read(p, sb, sizeof sb); else gs_read(p, sb, sizeof sb);
            snprintf(hs, sizeof hs, "%s%s%ss", spec, w, pr);
            tl = snprintf(tmp, sizeof tmp, hs, sb); break; }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            uint64_t u;
            if (va_is_ptr) { vp = (vp + 7) & ~7u; u = r32(vp) | ((uint64_t)r32(vp + 4) << 32); vp += 8; }
            else { if (idx & 1) idx++; u = A(idx) | ((uint64_t)A(idx + 1) << 32); idx += 2; }
            double d; memcpy(&d, &u, 8);
            snprintf(hs, sizeof hs, "%s%s%s%c", spec, w, pr, conv);
            tl = snprintf(tmp, sizeof tmp, hs, d); break; }
        default: tmp[0] = '%'; tmp[1] = conv; tl = 2; break;
        }
        if (tl > (int)sizeof tmp - 1) tl = sizeof tmp - 1;
        for (int i = 0; i < tl; i++) { if (o < outmax - 1) out[o] = tmp[i]; o++; }
    }
    if (outmax > 0) out[o < outmax ? o : outmax - 1] = 0;
    return o;
}

static char fbuf[8192];
static void fmt_out(uint32_t dst, int wide_out, int count, int n) {
    if (n >= (int)sizeof fbuf) n = sizeof fbuf - 1;
    if (wide_out) gw_write(dst, fbuf, count > 0 ? count : (int)sizeof fbuf);
    else { int l = (int)strlen(fbuf); if (count > 0 && l >= count) l = count - 1; gm_write(dst, fbuf, l); w8(dst + l, 0); }
}
static void sf_common(uint32_t buf, int fmt_reg, int wide, int count_reg, int first, int isva) {
    char f[2048]; uint32_t fa = A(fmt_reg);
    if (wide) gw_read(fa, f, sizeof f); else gs_read(fa, f, sizeof f);
    int count = count_reg >= 0 ? (int)A(count_reg) : 0;
    int n = hle_format(fbuf, sizeof fbuf, f, wide, isva ? A(first) : 0, isva, isva ? 0 : first);
    if (count_reg >= 0 && count <= 0) { R(0); return; }
    fmt_out(buf, wide, count, n);
    R(n < 0 ? 0 : n);
}
static void api_sprintf(void) { sf_common(A(0), 1, 0, -1, 2, 0); }
static void api_snprintf(void) { sf_common(A(0), 2, 0, 1, 3, 0); }
static void api_swprintf(void) { sf_common(A(0), 1, 1, -1, 2, 0); }
static void api_snwprintf(void) { sf_common(A(0), 2, 1, 1, 3, 0); }
static void api_vsprintf(void) { sf_common(A(0), 1, 0, -1, 2, 1); }
static void api_vsnprintf(void) { sf_common(A(0), 2, 0, 1, 3, 1); }
static void api_vswprintf(void) { sf_common(A(0), 1, 1, -1, 2, 1); }
static void api_vsnwprintf(void) { sf_common(A(0), 2, 1, 1, 3, 1); }
static void api_printf(void) { char f[2048]; gs_read(A(0), f, sizeof f); int n = hle_format(fbuf, sizeof fbuf, f, 0, 0, 0, 1); hle_log("printf: %s", fbuf); R(n); }
static void api_wprintf(void) { char f[2048]; gw_read(A(0), f, sizeof f); int n = hle_format(fbuf, sizeof fbuf, f, 1, 0, 0, 1); hle_log("wprintf: %s", fbuf); R(n); }

/* ------------------------------------------------------------ memory */
static void gcopy(uint32_t d, uint32_t s, uint32_t n) {
    while (n) {
        uint32_t c = n, ds = 0x1000 - (d & 0xFFF), ss = 0x1000 - (s & 0xFFF);
        if (c > ds) c = ds; if (c > ss) c = ss;
        uint8_t *dp = gm_ptr(d), *sp = gm_ptr(s);
        if (!dp || !sp) { for (uint32_t i = 0; i < c; i++) w8(d + i, r8(s + i)); }
        else memcpy(dp, sp, c);
        d += c; s += c; n -= c;
    }
}
static void api_memcpy(void) { uint32_t d = A(0); gcopy(d, A(1), A(2)); R(d); }
static void api_memmove(void) {
    uint32_t d = A(0), s = A(1), n = A(2);
    if (d > s && d < s + n) { for (uint32_t i = n; i > 0; i--) w8(d + i - 1, r8(s + i - 1)); }
    else gcopy(d, s, n);
    R(d);
}
static void api_memset(void) {
    uint32_t d = A(0), n = A(2); uint8_t v = (uint8_t)A(1), *p;
    uint32_t a = d;
    while (n) { uint32_t c = 0x1000 - (a & 0xFFF); if (c > n) c = n; p = gm_ptr(a); if (p) memset(p, v, c); else for (uint32_t i = 0; i < c; i++) w8(a + i, v); a += c; n -= c; }
    R(d);
}
static void api_memcmp(void) {
    uint32_t a = A(0), b = A(1), n = A(2);
    for (uint32_t i = 0; i < n; i++) { int d = (int)r8(a + i) - (int)r8(b + i); if (d) { R(d < 0 ? 0xFFFFFFFFu : 1); return; } }
    R(0);
}
static void api_memchr(void) { uint32_t a = A(0), n = A(2); uint8_t c = (uint8_t)A(1); for (uint32_t i = 0; i < n; i++) if (r8(a + i) == c) { R(a + i); return; } R(0); }
static void api_wmemset(void) { uint32_t d = A(0); for (uint32_t i = 0; i < A(2); i++) w16(d + 2 * i, (uint16_t)A(1)); R(d); }
static void api_wmemcpy(void) { uint32_t d = A(0); gcopy(d, A(1), A(2) * 2); R(d); }

/* ------------------------------------------------------------ ASCII strings */
static uint32_t slen(uint32_t a) { uint32_t n = 0; while (r8(a + n)) n++; return n; }
static uint32_t wlen(uint32_t a) { uint32_t n = 0; while (r16(a + 2 * n)) n++; return n; }
static void api_strlen(void) { R(slen(A(0))); }
static void api_strcpy(void) { uint32_t d = A(0), s = A(1); uint32_t n = slen(s) + 1; gcopy(d, s, n); R(d); }
static void api_strncpy(void) { uint32_t d = A(0), s = A(1), n = A(2); uint32_t i = 0; for (; i < n && r8(s + i); i++) w8(d + i, r8(s + i)); for (; i < n; i++) w8(d + i, 0); R(d); }
static void api_strcat(void) { uint32_t d = A(0); uint32_t e = d + slen(d); gcopy(e, A(1), slen(A(1)) + 1); R(d); }
static void api_strncat(void) { uint32_t d = A(0), s = A(1), n = A(2); uint32_t e = d + slen(d), i = 0; for (; i < n && r8(s + i); i++) w8(e + i, r8(s + i)); w8(e + i, 0); R(d); }
static int scmp(uint32_t a, uint32_t b, int64_t n, int ci) {
    for (int64_t i = 0; n < 0 || i < n; i++) {
        int x = r8(a + i), y = r8(b + i);
        if (ci) { x = tolower(x); y = tolower(y); }
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
    return 0;
}
static void api_strcmp(void) { R((uint32_t)scmp(A(0), A(1), -1, 0)); }
static void api_strncmp(void) { R((uint32_t)scmp(A(0), A(1), A(2), 0)); }
static void api_stricmp(void) { R((uint32_t)scmp(A(0), A(1), -1, 1)); }
static void api_strnicmp(void) { R((uint32_t)scmp(A(0), A(1), A(2), 1)); }
static void api_strchr(void) { uint32_t a = A(0); uint8_t c = (uint8_t)A(1); for (;; a++) { if (r8(a) == c) { R(a); return; } if (!r8(a)) { R(0); return; } } }
static void api_strrchr(void) { uint32_t a = A(0), r = 0; uint8_t c = (uint8_t)A(1); for (;; a++) { if (r8(a) == c) r = a; if (!r8(a)) break; } R(r); }
static void api_strstr(void) {
    uint32_t h = A(0), n = A(1); uint32_t nl = slen(n);
    if (!nl) { R(h); return; }
    for (; r8(h); h++) { uint32_t i = 0; while (i < nl && r8(h + i) == r8(n + i)) i++; if (i == nl) { R(h); return; } }
    R(0);
}
static void api_strdup(void) { uint32_t n = slen(A(0)) + 1, d = gm_alloc(n); if (d) gcopy(d, A(0), n); R(d); }
static void api_atoi(void) { char s[64]; gs_read(A(0), s, sizeof s); R((uint32_t)atoi(s)); }
static void api_atof(void) { char s[64]; gs_read(A(0), s, sizeof s); R_double(atof(s)); }
static void api_strtol(void) {
    char s[128]; gs_read(A(0), s, sizeof s); char *e; long v = strtol(s, &e, (int)A(2));
    if (A(1)) w32(A(1), A(0) + (uint32_t)(e - s)); R((uint32_t)v);
}
static void api_strtoul(void) {
    char s[128]; gs_read(A(0), s, sizeof s); char *e; unsigned long v = strtoul(s, &e, (int)A(2));
    if (A(1)) w32(A(1), A(0) + (uint32_t)(e - s)); R((uint32_t)v);
}
static void api_strtod(void) {
    char s[128]; gs_read(A(0), s, sizeof s); char *e; double v = strtod(s, &e);
    if (A(1)) w32(A(1), A(0) + (uint32_t)(e - s)); R_double(v);
}
static void api_itoa(void) { char s[40]; int r = (int)A(2); uint32_t v = A(0); if (r == 10) snprintf(s, sizeof s, "%d", (int)v); else if (r == 16) snprintf(s, sizeof s, "%x", v); else if (r == 8) snprintf(s, sizeof s, "%o", v); else { int n = 0; char t[40]; if (!v) t[n++] = '0'; while (v) { t[n++] = "0123456789abcdefghijklmnopqrstuvwxyz"[v % r]; v /= r; } for (int i = 0; i < n; i++) s[i] = t[n - 1 - i]; s[n] = 0; } gm_write(A(1), s, (uint32_t)strlen(s) + 1); R(A(1)); }
static void api_toupper(void) { R((uint32_t)toupper((int)A(0))); }
static void api_tolower(void) { R((uint32_t)tolower((int)A(0))); }
static void api_isalpha(void) { R(isalpha((int)A(0) & 0xFF) != 0); }
static void api_isdigit(void) { R(isdigit((int)A(0) & 0xFF) != 0); }
static void api_isalnum(void) { R(isalnum((int)A(0) & 0xFF) != 0); }
static void api_isspace(void) { R(isspace((int)A(0) & 0xFF) != 0); }
static void api_isupper(void) { R(isupper((int)A(0) & 0xFF) != 0); }
static void api_islower(void) { R(islower((int)A(0) & 0xFF) != 0); }
static void api_isxdigit(void) { R(isxdigit((int)A(0) & 0xFF) != 0); }
static void api_ispunct(void) { R(ispunct((int)A(0) & 0xFF) != 0); }
static void api_isprint(void) { R(isprint((int)A(0) & 0xFF) != 0); }
static void api_iscntrl(void) { R(iscntrl((int)A(0) & 0xFF) != 0); }

/* ------------------------------------------------------------ wide strings */
static void api_wcslen(void) { R(wlen(A(0))); }
static void api_wcscpy(void) { uint32_t d = A(0), s = A(1); gcopy(d, s, (wlen(s) + 1) * 2); R(d); }
static void api_wcsncpy(void) { uint32_t d = A(0), s = A(1), n = A(2), i = 0; for (; i < n && r16(s + 2 * i); i++) w16(d + 2 * i, r16(s + 2 * i)); for (; i < n; i++) w16(d + 2 * i, 0); R(d); }
static void api_wcscat(void) { uint32_t d = A(0); gcopy(d + wlen(d) * 2, A(1), (wlen(A(1)) + 1) * 2); R(d); }
static void api_wcsncat(void) { uint32_t d = A(0), s = A(1), n = A(2), e = d + wlen(d) * 2, i = 0; for (; i < n && r16(s + 2 * i); i++) w16(e + 2 * i, r16(s + 2 * i)); w16(e + 2 * i, 0); R(d); }
static int wcmp(uint32_t a, uint32_t b, int64_t n, int ci) {
    for (int64_t i = 0; n < 0 || i < n; i++) {
        int x = r16(a + 2 * i), y = r16(b + 2 * i);
        if (ci) { x = towlower(x); y = towlower(y); }
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
    return 0;
}
static void api_wcscmp(void) { R((uint32_t)wcmp(A(0), A(1), -1, 0)); }
static void api_wcsncmp(void) { R((uint32_t)wcmp(A(0), A(1), A(2), 0)); }
static void api_wcsicmp(void) { R((uint32_t)wcmp(A(0), A(1), -1, 1)); }
static void api_wcsnicmp(void) { R((uint32_t)wcmp(A(0), A(1), A(2), 1)); }
static void api_wcschr(void) { uint32_t a = A(0); uint16_t c = (uint16_t)A(1); for (;; a += 2) { if (r16(a) == c) { R(a); return; } if (!r16(a)) { R(0); return; } } }
static void api_wcsrchr(void) { uint32_t a = A(0), r = 0; uint16_t c = (uint16_t)A(1); for (;; a += 2) { if (r16(a) == c) r = a; if (!r16(a)) break; } R(r); }
static void api_wcsstr(void) {
    uint32_t h = A(0), n = A(1), nl = wlen(n);
    if (!nl) { R(h); return; }
    for (; r16(h); h += 2) { uint32_t i = 0; while (i < nl && r16(h + 2 * i) == r16(n + 2 * i)) i++; if (i == nl) { R(h); return; } }
    R(0);
}
static void api_wcsdup(void) { uint32_t n = (wlen(A(0)) + 1) * 2, d = gm_alloc(n); if (d) gcopy(d, A(0), n); R(d); }
static void api_wtoi(void) { char s[64]; gw_read(A(0), s, sizeof s); R((uint32_t)atoi(s)); }
static void api_wtof(void) { char s[64]; gw_read(A(0), s, sizeof s); R_double(atof(s)); }
static void api_wcstol(void) {
    char s[128]; gw_read(A(0), s, sizeof s); char *e; long v = strtol(s, &e, (int)A(2));
    if (A(1)) w32(A(1), A(0) + 2 * (uint32_t)(e - s)); R((uint32_t)v);
}
static void api_wcstoul(void) {
    char s[128]; gw_read(A(0), s, sizeof s); char *e; unsigned long v = strtoul(s, &e, (int)A(2));
    if (A(1)) w32(A(1), A(0) + 2 * (uint32_t)(e - s)); R((uint32_t)v);
}
static void api_itow(void) { char s[40]; snprintf(s, sizeof s, (int)A(2) == 16 ? "%x" : "%d", A(0)); gw_write(A(1), s, 40); R(A(1)); }
static void api_towupper(void) { R((uint32_t)towupper((wint_t)A(0))); }
static void api_towlower(void) { R((uint32_t)towlower((wint_t)A(0))); }
static void api_iswspace(void) { R(iswspace((wint_t)A(0)) != 0); }
static void api_iswdigit(void) { R(iswdigit((wint_t)A(0)) != 0); }
static void api_iswalpha(void) { R(iswalpha((wint_t)A(0)) != 0); }
static void api_lstrlenW(void) { R(A(0) ? wlen(A(0)) : 0); }
static void api_lstrcpynW(void) { uint32_t d = A(0), s = A(1), n = A(2), i = 0; if (!n) { R(0); return; } for (; i + 1 < n && r16(s + 2 * i); i++) w16(d + 2 * i, r16(s + 2 * i)); w16(d + 2 * i, 0); R(d); }
static void api_mbstowcs(void) {
    uint32_t d = A(0), s = A(1), n = A(2), i = 0;
    for (; i < n && r8(s + i); i++) if (d) w16(d + 2 * i, r8(s + i));
    if (d && i < n) w16(d + 2 * i, 0); R(i);
}
static void api_wcstombs(void) {
    uint32_t d = A(0), s = A(1), n = A(2), i = 0;
    for (; i < n && r16(s + 2 * i); i++) if (d) w8(d + i, (uint8_t)r16(s + 2 * i));
    if (d && i < n) w8(d + i, 0); R(i);
}
static void api_MultiByteToWideChar(void) {
    uint32_t s = A(2); int sl = (int)A(3); uint32_t d = A(4); int dl = (int)A(5);
    if (sl < 0) sl = (int)slen(s) + 1;
    if (dl == 0) { R((uint32_t)sl); return; }
    int i = 0; for (; i < sl && i < dl; i++) w16(d + 2 * i, r8(s + i));
    R((uint32_t)i);
}
static void api_WideCharToMultiByte(void) {
    uint32_t s = A(2); int sl = (int)A(3); uint32_t d = A(4); int dl = (int)A(5);
    if (sl < 0) sl = (int)wlen(s) + 1;
    if (dl == 0) { R((uint32_t)sl); return; }
    int i = 0; for (; i < sl && i < dl; i++) { uint16_t c = r16(s + 2 * i); w8(d + i, c < 256 ? (uint8_t)c : '?'); }
    R((uint32_t)i);
}

/* ------------------------------------------------------------ misc CRT */
static uint32_t rand_seed = 1;
static void api_rand(void) { rand_seed = rand_seed * 214013u + 2531011u; R((rand_seed >> 16) & 0x7FFF); }
static void api_srand(void) { rand_seed = A(0); }
static void api_abs(void) { int v = (int)A(0); R((uint32_t)(v < 0 ? -v : v)); }
static void api_time(void) { uint32_t t = (uint32_t)time(NULL); if (A(0)) w32(A(0), t); R(t); }
static void api_clock(void) { R(tick_ms()); }
static void api_NoOp0(void) { R(0); }

static void sort_swap(uint32_t a, uint32_t b, uint32_t sz) {
    for (uint32_t i = 0; i < sz; i++) { uint8_t t = r8(a + i); w8(a + i, r8(b + i)); w8(b + i, t); }
}
static void api_qsort(void) {
    uint32_t base = A(0), n = A(1), sz = A(2), fn = A(3);
    if (n < 2 || !sz) return;
    /* heapsort, comparisons through the guest callback */
#define EL(i) (base + (uint32_t)(i) * sz)
#define CMP(i, j) ((int32_t)guest_call(fn, EL(i), EL(j), 0, 0))
    for (int64_t s = (int64_t)n / 2 - 1; s >= 0; s--) {
        int64_t r = s;
        for (;;) { int64_t c = 2 * r + 1; if (c >= (int64_t)n) break; if (c + 1 < (int64_t)n && CMP(c, c + 1) < 0) c++; if (CMP(r, c) >= 0) break; sort_swap(EL(r), EL(c), sz); r = c; }
    }
    for (int64_t e = (int64_t)n - 1; e > 0; e--) {
        sort_swap(EL(0), EL(e), sz);
        int64_t r = 0;
        for (;;) { int64_t c = 2 * r + 1; if (c >= e) break; if (c + 1 < e && CMP(c, c + 1) < 0) c++; if (CMP(r, c) >= 0) break; sort_swap(EL(r), EL(c), sz); r = c; }
    }
}
static void api_bsearch(void) {
    uint32_t key = A(0), base = A(1), n = A(2), sz = A(3), fn = A(4);
    int64_t lo = 0, hi = (int64_t)n - 1;
    while (lo <= hi) {
        int64_t mid = (lo + hi) / 2; uint32_t e = base + (uint32_t)mid * sz;
        int c = (int32_t)guest_call(fn, key, e, 0, 0);
        if (c == 0) { R(e); return; }
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    R(0);
}

/* setjmp / longjmp: buffer = r4..r11, sp, lr */
static void api_setjmp(void) {
    uint32_t b = A(0);
    for (int i = 0; i < 8; i++) w32(b + 4 * i, cpu.r[4 + i]);
    w32(b + 32, cpu.r[13]); w32(b + 36, cpu.r[14]); R(0);
}
static void api_longjmp(void) {
    uint32_t b = A(0), v = A(1);
    for (int i = 0; i < 8; i++) cpu.r[4 + i] = r32(b + 4 * i);
    cpu.r[13] = r32(b + 32);
    uint32_t lr = r32(b + 36);
    cpu.r[0] = v ? v : 1;
    cpu.r[15] = lr & ~1u;
    cpu.cpsr = (cpu.cpsr & ~0x20u) | ((lr & 1) << 5);
    api_flow_changed = 1;
}

/* ------------------------------------------------------------ math (doubles in r0:r1, r2:r3) */
#define M1(nm, fn) static void api_##nm(void) { R_double(fn(D_arg(0))); }
M1(sin, sin) M1(cos, cos) M1(tan, tan) M1(asin, asin) M1(acos, acos) M1(atan, atan) M1(sinh, sinh) M1(cosh, cosh) M1(tanh, tanh)
M1(sqrt, sqrt) M1(exp, exp) M1(log, log) M1(log10, log10) M1(floor, floor) M1(ceil, ceil) M1(fabs, fabs)
static void api_atan2(void) { R_double(atan2(D_arg(0), D_arg(2))); }
static void api_pow(void) { R_double(pow(D_arg(0), D_arg(2))); }
static void api_fmod(void) { R_double(fmod(D_arg(0), D_arg(2))); }
static void api_ldexp(void) { R_double(ldexp(D_arg(0), (int)A(2))); }
static void api_frexp(void) { int e; double m = frexp(D_arg(0), &e); w32(A(2), (uint32_t)e); R_double(m); }
static void api_modf(void) { double ip; double f = modf(D_arg(0), &ip); uint64_t u; memcpy(&u, &ip, 8); w32(A(2), (uint32_t)u); w32(A(2) + 4, (uint32_t)(u >> 32)); R_double(f); }
static void api_isnan(void) { R(isnan(D_arg(0)) != 0); }
static void api_finite(void) { R(isfinite(D_arg(0)) != 0); }

/* soft-float runtime (the compiler emits calls to these) */
#define DBIN(nm, op) static void api___##nm(void) { R_double(D_arg(0) op D_arg(2)); }
DBIN(addd, +) DBIN(subd, -) DBIN(muld, *) DBIN(divd, /)
#define DCMP(nm, op) static void api___##nm(void) { R(D_arg(0) op D_arg(2)); }
DCMP(eqd, ==) DCMP(ned, !=) DCMP(ltd, <) DCMP(led, <=) DCMP(gtd, >) DCMP(ged, >=)
static void api___negd(void) { R_double(-D_arg(0)); }
static void api___dtoi(void) { double d = D_arg(0); R(d >= 2147483647.0 ? 0x7FFFFFFFu : d <= -2147483648.0 ? 0x80000000u : (uint32_t)(int32_t)d); }
static void api___dtou(void) { double d = D_arg(0); R(d <= 0 ? 0 : d >= 4294967295.0 ? 0xFFFFFFFFu : (uint32_t)d); }
static void api___itod(void) { R_double((double)(int32_t)A(0)); }
static void api___utod(void) { R_double((double)A(0)); }
static void api___dtoi64(void) { R64((uint64_t)(int64_t)D_arg(0)); }
static void api___dtou64(void) { R64((uint64_t)D_arg(0)); }
static void api___i64tod(void) { R_double((double)(int64_t)(cpu.r[0] | ((uint64_t)cpu.r[1] << 32))); }
static void api___u64tod(void) { R_double((double)(cpu.r[0] | ((uint64_t)cpu.r[1] << 32))); }
static void api___dtof(void) { R_float((float)D_arg(0)); }
static void api___ftod(void) { R_double((double)F_arg(0)); }
#define FBIN(nm, op) static void api___##nm(void) { R_float(F_arg(0) op F_arg(1)); }
FBIN(addf, +) FBIN(subf, -) FBIN(mulf, *) FBIN(divf, /)
#define FCMP(nm, op) static void api___##nm(void) { R(F_arg(0) op F_arg(1)); }
FCMP(eqf, ==) FCMP(nef, !=) FCMP(ltf, <) FCMP(lef, <=) FCMP(gtf, >) FCMP(gef, >=)
static void api___negf(void) { R_float(-F_arg(0)); }
static void api___ftoi(void) { float f = F_arg(0); R((uint32_t)(int32_t)f); }
static void api___ftou(void) { float f = F_arg(0); R(f <= 0 ? 0 : (uint32_t)f); }
static void api___itof(void) { R_float((float)(int32_t)A(0)); }
static void api___utof(void) { R_float((float)A(0)); }

/* integer division helpers: r0 = divisor, r1 = dividend -> r0 quotient, r1 remainder */
static void api___rt_sdiv(void) { int32_t d = (int32_t)cpu.r[0], n = (int32_t)cpu.r[1]; if (!d) { cpu.r[0] = 0; cpu.r[1] = 0; return; } if (d == -1) { cpu.r[0] = (uint32_t)-n; cpu.r[1] = 0; return; } cpu.r[0] = (uint32_t)(n / d); cpu.r[1] = (uint32_t)(n % d); }
static void api___rt_udiv(void) { uint32_t d = cpu.r[0], n = cpu.r[1]; if (!d) { cpu.r[0] = 0; cpu.r[1] = 0; return; } cpu.r[0] = n / d; cpu.r[1] = n % d; }
/* 64-bit: dividend in r0:r1, divisor in r2:r3 -> quotient r0:r1 (remainder r2:r3) */
static void api___rt_sdiv64(void) {
    int64_t n = (int64_t)(cpu.r[0] | ((uint64_t)cpu.r[1] << 32)), d = (int64_t)(cpu.r[2] | ((uint64_t)cpu.r[3] << 32));
    int64_t q = 0, r = 0; if (d && !(d == -1 && n == INT64_MIN)) { q = n / d; r = n % d; }
    cpu.r[0] = (uint32_t)q; cpu.r[1] = (uint32_t)((uint64_t)q >> 32); cpu.r[2] = (uint32_t)r; cpu.r[3] = (uint32_t)((uint64_t)r >> 32);
}
static void api___rt_udiv64(void) {
    uint64_t n = cpu.r[0] | ((uint64_t)cpu.r[1] << 32), d = cpu.r[2] | ((uint64_t)cpu.r[3] << 32);
    uint64_t q = 0, r = 0; if (d) { q = n / d; r = n % d; }
    cpu.r[0] = (uint32_t)q; cpu.r[1] = (uint32_t)(q >> 32); cpu.r[2] = (uint32_t)r; cpu.r[3] = (uint32_t)(r >> 32);
}
static void api___rt_srem64(void) { api___rt_sdiv64(); cpu.r[0] = cpu.r[2]; cpu.r[1] = cpu.r[3]; }
static void api___rt_urem64(void) { api___rt_udiv64(); cpu.r[0] = cpu.r[2]; cpu.r[1] = cpu.r[3]; }
static void api___rt_lshl(void) { uint64_t v = cpu.r[0] | ((uint64_t)cpu.r[1] << 32); v <<= cpu.r[2] & 63; R64(v); }
static void api___rt_lshr(void) { uint64_t v = cpu.r[0] | ((uint64_t)cpu.r[1] << 32); v >>= cpu.r[2] & 63; R64(v); }
static void api___rt_ashr(void) { int64_t v = (int64_t)(cpu.r[0] | ((uint64_t)cpu.r[1] << 32)); v >>= cpu.r[2] & 63; R64((uint64_t)v); }

#define E(n, f) { n, f }
static const ApiDef cdefs[] = {
    E("sprintf", api_sprintf), E("_snprintf", api_snprintf), E("snprintf", api_snprintf), E("swprintf", api_swprintf), E("_snwprintf", api_snwprintf),
    E("wsprintfW", api_swprintf), E("vsprintf", api_vsprintf), E("_vsnprintf", api_vsnprintf), E("vswprintf", api_vswprintf), E("_vsnwprintf", api_vsnwprintf),
    E("wvsprintfW", api_vswprintf), E("printf", api_printf), E("wprintf", api_wprintf),
    E("memcpy", api_memcpy), E("memmove", api_memmove), E("memset", api_memset), E("memcmp", api_memcmp), E("memchr", api_memchr),
    E("_memicmp", api_memcmp), E("wmemset", api_wmemset), E("wmemcpy", api_wmemcpy),
    E("strlen", api_strlen), E("strcpy", api_strcpy), E("strncpy", api_strncpy), E("strcat", api_strcat), E("strncat", api_strncat),
    E("strcmp", api_strcmp), E("strncmp", api_strncmp), E("_stricmp", api_stricmp), E("stricmp", api_stricmp), E("_strnicmp", api_strnicmp), E("strnicmp", api_strnicmp),
    E("strchr", api_strchr), E("strrchr", api_strrchr), E("strstr", api_strstr), E("_strdup", api_strdup),
    E("atoi", api_atoi), E("atol", api_atoi), E("atof", api_atof), E("strtol", api_strtol), E("strtoul", api_strtoul), E("strtod", api_strtod),
    E("_itoa", api_itoa), E("_ltoa", api_itoa), E("_ultoa", api_itoa), E("toupper", api_toupper), E("tolower", api_tolower), E("_toupper", api_toupper), E("_tolower", api_tolower),
    E("isalpha", api_isalpha), E("isdigit", api_isdigit), E("isalnum", api_isalnum), E("isspace", api_isspace), E("isupper", api_isupper), E("islower", api_islower),
    E("isxdigit", api_isxdigit), E("ispunct", api_ispunct), E("isprint", api_isprint), E("iscntrl", api_iscntrl),
    E("wcslen", api_wcslen), E("wcscpy", api_wcscpy), E("wcsncpy", api_wcsncpy), E("wcscat", api_wcscat), E("wcsncat", api_wcsncat),
    E("wcscmp", api_wcscmp), E("wcsncmp", api_wcsncmp), E("_wcsicmp", api_wcsicmp), E("_wcsnicmp", api_wcsnicmp), E("wcsicmp", api_wcsicmp),
    E("wcschr", api_wcschr), E("wcsrchr", api_wcsrchr), E("wcsstr", api_wcsstr), E("_wcsdup", api_wcsdup), E("_wtoi", api_wtoi), E("_wtol", api_wtoi), E("wtoi", api_wtoi),
    E("_wtof", api_wtof), E("wcstol", api_wcstol), E("wcstoul", api_wcstoul), E("_itow", api_itow), E("towupper", api_towupper), E("towlower", api_towlower),
    E("iswspace", api_iswspace), E("iswdigit", api_iswdigit), E("iswalpha", api_iswalpha),
    E("lstrlenW", api_lstrlenW), E("lstrcpyW", api_wcscpy), E("lstrcpynW", api_lstrcpynW), E("lstrcatW", api_wcscat), E("lstrcmpW", api_wcscmp), E("lstrcmpiW", api_wcsicmp),
    E("mbstowcs", api_mbstowcs), E("wcstombs", api_wcstombs), E("MultiByteToWideChar", api_MultiByteToWideChar), E("WideCharToMultiByte", api_WideCharToMultiByte),
    E("rand", api_rand), E("srand", api_srand), E("abs", api_abs), E("labs", api_abs), E("time", api_time), E("clock", api_clock),
    E("qsort", api_qsort), E("bsearch", api_bsearch), E("setjmp", api_setjmp), E("_setjmp", api_setjmp), E("longjmp", api_longjmp),
    E("_purecall", api_NoOp0), E("_CxxThrowException", api_NoOp0), E("__CxxFrameHandler", api_NoOp0), E("_except_handler3", api_NoOp0), E("_XcptFilter", api_NoOp0), E("__C_specific_handler", api_NoOp0),
    E("sin", api_sin), E("cos", api_cos), E("tan", api_tan), E("asin", api_asin), E("acos", api_acos), E("atan", api_atan), E("sinh", api_sinh), E("cosh", api_cosh), E("tanh", api_tanh),
    E("sqrt", api_sqrt), E("exp", api_exp), E("log", api_log), E("log10", api_log10), E("floor", api_floor), E("ceil", api_ceil), E("fabs", api_fabs),
    E("atan2", api_atan2), E("pow", api_pow), E("fmod", api_fmod), E("ldexp", api_ldexp), E("frexp", api_frexp), E("modf", api_modf), E("_isnan", api_isnan), E("_finite", api_finite),
    E("__addd", api___addd), E("__subd", api___subd), E("__muld", api___muld), E("__divd", api___divd), E("__negd", api___negd),
    E("__eqd", api___eqd), E("__ned", api___ned), E("__ltd", api___ltd), E("__led", api___led), E("__gtd", api___gtd), E("__ged", api___ged),
    E("__dtoi", api___dtoi), E("__dtou", api___dtou), E("__itod", api___itod), E("__utod", api___utod), E("__dtoi64", api___dtoi64), E("__dtou64", api___dtou64),
    E("__i64tod", api___i64tod), E("__u64tod", api___u64tod), E("__dtof", api___dtof), E("__ftod", api___ftod),
    E("__addf", api___addf), E("__subf", api___subf), E("__mulf", api___mulf), E("__divf", api___divf), E("__negf", api___negf),
    E("__eqf", api___eqf), E("__nef", api___nef), E("__ltf", api___ltf), E("__lef", api___lef), E("__gtf", api___gtf), E("__gef", api___gef),
    E("__ftoi", api___ftoi), E("__ftou", api___ftou), E("__itof", api___itof), E("__utof", api___utof),
    E("__rt_sdiv", api___rt_sdiv), E("__rt_udiv", api___rt_udiv), E("__rt_sdiv64by64", api___rt_sdiv64), E("__rt_udiv64by64", api___rt_udiv64),
    E("__rt_srem64by64", api___rt_srem64), E("__rt_urem64by64", api___rt_urem64), E("__rt_lshl", api___rt_lshl), E("__rt_lshr", api___rt_lshr), E("__rt_ashr", api___rt_ashr),
};
void api_crt_init(void) { api_register(cdefs, sizeof cdefs / sizeof cdefs[0]); }
