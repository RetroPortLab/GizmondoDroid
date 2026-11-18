/* GizmondoDroid - registry (in memory), Global* memory, locale/string helpers, shell/COM stubs, sscanf */
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <wctype.h>

/* ------------------------------------------------------------ registry */
typedef struct RegVal { char path[256], name[96]; uint32_t type, len; uint8_t *data; int is_key; } RegVal;
static RegVal *rv; static int nrv, caprv;
typedef struct { char path[256]; } RegKey;

static void root_name(uint32_t h, char *out) {
    switch (h) { case 0x80000000: strcpy(out, "HKCR"); break; case 0x80000001: strcpy(out, "HKCU"); break; case 0x80000002: strcpy(out, "HKLM"); break; case 0x80000003: strcpy(out, "HKU"); break; default: out[0] = 0; }
}
static int key_path(uint32_t hkey, const char *sub, char *out) {
    char base[256];
    if (hkey >= 0x80000000) root_name(hkey, base);
    else { RegKey *k = h_get(hkey, H_REGKEY); if (!k) return 0; snprintf(base, sizeof base, "%s", k->path); }
    if (sub && *sub) snprintf(out, 256, "%s\\%s", base, sub); else snprintf(out, 256, "%s", base);
    for (char *c = out; *c; c++) *c = (char)tolower((unsigned char)*c);
    size_t l = strlen(out); while (l && out[l - 1] == '\\') out[--l] = 0;
    return 1;
}
static RegVal *rv_find(const char *path, const char *name, int is_key) {
    for (int i = 0; i < nrv; i++) if (rv[i].is_key == is_key && !strcmp(rv[i].path, path) && !strcasecmp(rv[i].name, name)) return &rv[i];
    return NULL;
}
static int key_exists(const char *path) {
    size_t l = strlen(path);
    for (int i = 0; i < nrv; i++) if (!strncmp(rv[i].path, path, l) && (rv[i].path[l] == 0 || rv[i].path[l] == '\\')) return 1;
    return 0;
}
static RegVal *rv_add(const char *path, const char *name, int is_key) {
    if (nrv == caprv) { caprv = caprv ? caprv * 2 : 64; rv = realloc(rv, caprv * sizeof *rv); }
    RegVal *v = &rv[nrv++]; memset(v, 0, sizeof *v); snprintf(v->path, sizeof v->path, "%s", path); snprintf(v->name, sizeof v->name, "%s", name); v->is_key = is_key; return v;
}
static void make_key(const char *path) {   /* create key and all parents as marker entries */
    char p[256]; snprintf(p, sizeof p, "%s", path);
    for (char *c = p; ; c++) {
        if (*c == '\\' || *c == 0) { char sv = *c; *c = 0; if (!rv_find(p, "", 1)) rv_add(p, "", 1); *c = sv; if (!sv) break; }
    }
}
static uint32_t open_key(uint32_t hk, uint32_t subp, uint32_t out, int create) {
    char sub[256] = ""; if (subp) gw_read(subp, sub, sizeof sub);
    char path[256]; if (!key_path(hk, sub, path)) return 6;
    if (create) make_key(path); else if (strchr(path, '\\') && !key_exists(path)) return 2;
    RegKey *k = calloc(1, sizeof *k); snprintf(k->path, sizeof k->path, "%s", path);
    w32(out, h_new(H_REGKEY, k)); return 0;
}
static void api_RegOpenKeyExW(void) { R(open_key(A(0), A(1), A(4), 0)); }
static void api_RegCreateKeyExW(void) { uint32_t r = open_key(A(0), A(1), A(7), 1); if (!r && A(8)) w32(A(8), 2); R(r); }
static void api_RegCloseKey(void) { if (A(0) < 0x80000000 && h_type(A(0)) == H_REGKEY) { free(h_get(A(0), H_REGKEY)); h_close(A(0)); } R(0); }
static void api_RegQueryValueExW(void) {   /* hkey, name, reserved, ptype, pdata, pcb */
    char path[256], nm[96] = ""; if (!key_path(A(0), "", path)) { R(6); return; }
    if (A(1)) gw_read(A(1), nm, sizeof nm);
    RegVal *v = rv_find(path, nm, 0); if (!v) { R(2); return; }
    if (A(3)) w32(A(3), v->type);
    if (A(5)) { uint32_t cap = r32(A(5)); w32(A(5), v->len); if (A(4)) { if (cap < v->len) { R(234); return; } gm_write(A(4), v->data, v->len); } }
    R(0);
}
static void api_RegSetValueExW(void) {   /* hkey, name, reserved, type, data, cb */
    char path[256], nm[96] = ""; if (!key_path(A(0), "", path)) { R(6); return; }
    if (A(1)) gw_read(A(1), nm, sizeof nm);
    make_key(path);
    RegVal *v = rv_find(path, nm, 0); if (!v) v = rv_add(path, nm, 0);
    free(v->data); v->type = A(3); v->len = A(5); v->data = malloc(v->len ? v->len : 1); gm_read(v->data, A(4), v->len); R(0);
}
static void api_RegDeleteValueW(void) {
    char path[256], nm[96] = ""; if (!key_path(A(0), "", path)) { R(6); return; } if (A(1)) gw_read(A(1), nm, sizeof nm);
    RegVal *v = rv_find(path, nm, 0); if (!v) { R(2); return; } free(v->data); *v = rv[--nrv]; R(0);
}
static void api_RegDeleteKeyW(void) {
    char sub[256] = "", path[256]; if (A(1)) gw_read(A(1), sub, sizeof sub); if (!key_path(A(0), sub, path)) { R(6); return; }
    size_t l = strlen(path);
    for (int i = 0; i < nrv; ) if (!strncmp(rv[i].path, path, l) && (rv[i].path[l] == 0 || rv[i].path[l] == '\\')) { free(rv[i].data); rv[i] = rv[--nrv]; } else i++;
    R(0);
}
static void api_RegEnumKeyExW(void) {   /* hkey, idx, name, pcchName ... */
    char path[256]; if (!key_path(A(0), "", path)) { R(6); return; }
    size_t l = strlen(path); int n = 0; char seen[64][96]; int ns = 0;
    for (int i = 0; i < nrv; i++) {
        if (strncmp(rv[i].path, path, l) || rv[i].path[l] != '\\') continue;
        const char *s = rv[i].path + l + 1; char name[96]; int k = 0; while (s[k] && s[k] != '\\' && k < 95) { name[k] = s[k]; k++; } name[k] = 0;
        int dup = 0; for (int j = 0; j < ns; j++) if (!strcmp(seen[j], name)) dup = 1;
        if (dup || ns >= 64) continue;
        snprintf(seen[ns++], 96, "%s", name);
        if (n++ == (int)A(1)) { gw_write(A(2), name, (int)r32(A(3))); w32(A(3), (uint32_t)strlen(name)); R(0); return; }
    }
    R(259);
}
static void api_RegEnumValueW(void) {   /* hkey, idx, name, pcchName, res, ptype, pdata, pcbData */
    char path[256]; if (!key_path(A(0), "", path)) { R(6); return; }
    int n = 0; for (int i = 0; i < nrv; i++) if (!rv[i].is_key && !strcmp(rv[i].path, path)) {
        if (n++ != (int)A(1)) continue;
        gw_write(A(2), rv[i].name, (int)r32(A(3))); w32(A(3), (uint32_t)strlen(rv[i].name));
        if (A(5)) w32(A(5), rv[i].type);
        if (A(7)) { if (A(6) && r32(A(7)) >= rv[i].len) gm_write(A(6), rv[i].data, rv[i].len); w32(A(7), rv[i].len); }
        R(0); return;
    }
    R(259);
}
static void api_RegFlushKey(void) { R(0); }

/* ------------------------------------------------------------ Global* memory (maps onto the heap) */
static void api_GlobalAlloc(void) { uint32_t a = gm_alloc(A(1)); if (a && (A(0) & 0x40)) memset(gm_ptr(a), 0, A(1)); R(a); }
static void api_GlobalFree(void) { gm_free(A(0)); R(0); }
static void api_GlobalLock(void) { R(A(0)); }
static void api_GlobalUnlock(void) { R(0); }
static void api_GlobalReAlloc(void) { R(gm_realloc(A(0), A(1))); }
static void api_GlobalSize(void) { R(gm_size(A(0))); }

/* ------------------------------------------------------------ locale / strings */
static void api_CompareStringW(void) {   /* lcid, flags, s1, n1, s2, n2 */
    int n1 = (int)A(3), n2 = (int)A(5); if (n1 < 0) n1 = gw_len(A(2)); if (n2 < 0) n2 = gw_len(A(4));
    int ci = (A(1) & 1) != 0;
    for (int i = 0; i < n1 || i < n2; i++) {
        int a = i < n1 ? r16(A(2) + 2 * i) : 0, b = i < n2 ? r16(A(4) + 2 * i) : 0;
        if (ci) { a = towlower(a); b = towlower(b); }
        if (a != b) { R(a < b ? 1 : 3); return; }
    }
    R(2);
}
static void api_LCMapStringW(void) { int n = (int)A(3); if (n < 0) n = gw_len(A(2)) + 1; if (!A(5)) { R((uint32_t)n); return; } for (int i = 0; i < n && i < (int)A(5); i++) { int c = r16(A(2) + 2 * i); if (A(1) & 0x200) c = towupper(c); else if (A(1) & 0x100) c = towlower(c); w16(A(4) + 2 * i, (uint16_t)c); } R((uint32_t)n); }
static void api_GetLocaleInfoW(void) { R(0); }
static void api_GetUserDefaultLCID(void) { R(0x409); }
static void api_GetACP(void) { R(1252); }
static void api_CharUpperW(void) { if (A(0) < 0x10000) { R((uint32_t)towupper((wint_t)A(0))); return; } for (uint32_t a = A(0); r16(a); a += 2) w16(a, (uint16_t)towupper(r16(a))); R(A(0)); }
static void api_CharLowerW(void) { if (A(0) < 0x10000) { R((uint32_t)towlower((wint_t)A(0))); return; } for (uint32_t a = A(0); r16(a); a += 2) w16(a, (uint16_t)towlower(r16(a))); R(A(0)); }
static void api_CharNextW(void) { R(r16(A(0)) ? A(0) + 2 : A(0)); }
static void api_CharPrevW(void) { R(A(1) > A(0) ? A(1) - 2 : A(0)); }
static void api_strupr(void) { for (uint32_t a = A(0); r8(a); a++) w8(a, (uint8_t)toupper(r8(a))); R(A(0)); }
static void api_strlwr(void) { for (uint32_t a = A(0); r8(a); a++) w8(a, (uint8_t)tolower(r8(a))); R(A(0)); }
static void api_wcsupr(void) { for (uint32_t a = A(0); r16(a); a += 2) w16(a, (uint16_t)towupper(r16(a))); R(A(0)); }
static void api_wcslwr(void) { for (uint32_t a = A(0); r16(a); a += 2) w16(a, (uint16_t)towlower(r16(a))); R(A(0)); }

/* ------------------------------------------------------------ sscanf (subset) */
static void api_sscanf(void) {
    char in[2048]; gs_read(A(0), in, sizeof in); char fm[512]; gs_read(A(1), fm, sizeof fm);
    int idx = 2, done = 0; const char *s = in;
    for (const char *f = fm; *f; f++) {
        if (isspace((unsigned char)*f)) { while (isspace((unsigned char)*s)) s++; continue; }
        if (*f != '%') { if (*s != *f) break; s++; continue; }
        f++; int skip = 0; if (*f == '*') { skip = 1; f++; }
        int lng = 0, wid = 0; while (isdigit((unsigned char)*f)) wid = wid * 10 + *f++ - '0';
        if (*f == 'l' || *f == 'h') { lng = *f == 'l'; f++; }
        if (*f == '%') { if (*s != '%') break; s++; continue; }
        if (*f != 'c') while (isspace((unsigned char)*s)) s++;
        if (!*s && *f != 'n') break;
        uint32_t dst = skip ? 0 : A(idx); char *e;
        if (*f == '[') {
            const char *q = f + 1; int neg = 0; if (*q == '^') { neg = 1; q++; }
            char set[256] = {0}; int first = 1;
            while (*q && (*q != ']' || first)) {
                if (q[1] == '-' && q[2] && q[2] != ']') { for (int c = (unsigned char)q[0]; c <= (unsigned char)q[2]; c++) set[c] = 1; q += 3; }
                else set[(unsigned char)*q++] = 1;
                first = 0;
            }
            f = q; int k = 0;
            while (*s && (set[(unsigned char)*s] != 0) != neg && (!wid || k < wid)) { if (!skip) w8(dst + (uint32_t)k, (uint8_t)*s); k++; s++; }
            if (!k) goto out;
            if (!skip) { w8(dst + (uint32_t)k, 0); idx++; done++; }
            continue;
        }
        if (skip) { if (*f == 's') { while (*s && !isspace((unsigned char)*s)) s++; } else if (*f == 'd' || *f == 'i' || *f == 'u') { strtol(s, &e, 10); s = e; } else if (*f == 'f') { strtod(s, &e); s = e; } continue; }
        switch (*f) {
        case 'd': case 'i': { long v = strtol(s, &e, *f == 'i' ? 0 : 10); if (e == s) goto out; w32(dst, (uint32_t)v); s = e; idx++; done++; break; }
        case 'u': { unsigned long v = strtoul(s, &e, 10); if (e == s) goto out; w32(dst, (uint32_t)v); s = e; idx++; done++; break; }
        case 'x': case 'X': { unsigned long v = strtoul(s, &e, 16); if (e == s) goto out; w32(dst, (uint32_t)v); s = e; idx++; done++; break; }
        case 'f': case 'g': case 'e': { double v = strtod(s, &e); if (e == s) goto out; if (lng) { uint64_t u; memcpy(&u, &v, 8); w32(dst, (uint32_t)u); w32(dst + 4, (uint32_t)(u >> 32)); } else { float fv = (float)v; uint32_t u; memcpy(&u, &fv, 4); w32(dst, u); } s = e; idx++; done++; break; }
        case 's': { int k = 0; while (*s && !isspace((unsigned char)*s) && (!wid || k < wid)) w8(dst + (uint32_t)k++, (uint8_t)*s++); w8(dst + (uint32_t)k, 0); idx++; done++; break; }
        case 'c': { w8(dst, (uint8_t)*s++); idx++; done++; break; }
        case 'n': w32(dst, (uint32_t)(s - in)); idx++; break;
        default: goto out;
        }
    }
out:
    if (hle_verbose) hle_log("sscanf fmt='%s' in='%.60s' -> %d", fm, in, done);
    R((uint32_t)done);
}

/* ------------------------------------------------------------ shell / COM / power / misc stubs */
static void api_zero(void) { R(0); }
static void api_one(void) { R(1); }
static void api_CoCreateInstance(void) { R(0x80040154); }
static void api_E_FAIL(void) { R(0x80004005); }
static void api_GetSystemPowerStatusEx(void) {
    uint32_t p = A(0); for (int i = 0; i < 9; i++) w32(p + 4 * i, 0);
    w8(p, 1); w8(p + 1, 1); w8(p + 2, 100); w8(p + 3, 0xFF); w32(p + 4, 0xFFFFFFFF); w8(p + 8, 100); w32(p + 12, 0xFFFFFFFF); R(1);
}
static void api_GetStartupInfo(void) { for (int i = 0; i < 17; i++) w32(A(0) + 4 * i, 0); w32(A(0), 68); }
static void api_FormatMessageW(void) { R(0); }
static void api_timeBeginPeriod(void) { R(0); }
static void api_RequestPowerNotifications(void) { R(h_new(H_MISC, calloc(1, 8))); }
static void api_OpenEventW(void) { R(0); kern_set_error(2); }
static void api_waveOutGetDevCaps(void) { if (A(1)) for (uint32_t i = 0; i < A(2) && i < 256; i++) w8(A(1) + i, 0); R(0); }
static void api_DeviceIoControl(void) { if (hle_verbose) hle_log("DeviceIoControl code=%08x in=%u out=%u", A(1), A(3), A(5)); if (A(4) && A(5) < 0x10000) for (uint32_t i = 0; i < A(5); i++) w8(A(4) + i, 0); R(1); }
static void api_SHGetSpecialFolderPathW(void) {
    const char *p = A(1) == 5 ? "\\My Documents" : A(1) == 2 ? "\\Windows\\Programs" : "\\Windows";
    gw_write(A(2), p, 260); R(1);
}
static void api_GetSystemDefaultLangID(void) { R(0x409); }

#define E(n, f) { n, f }
static const ApiDef mdefs[] = {
    E("RegOpenKeyExW", api_RegOpenKeyExW), E("RegCreateKeyExW", api_RegCreateKeyExW), E("RegCloseKey", api_RegCloseKey), E("RegQueryValueExW", api_RegQueryValueExW),
    E("RegSetValueExW", api_RegSetValueExW), E("RegDeleteValueW", api_RegDeleteValueW), E("RegDeleteKeyW", api_RegDeleteKeyW), E("RegEnumKeyExW", api_RegEnumKeyExW),
    E("RegEnumValueW", api_RegEnumValueW), E("RegFlushKey", api_RegFlushKey),
    E("GlobalAlloc", api_GlobalAlloc), E("GlobalFree", api_GlobalFree), E("GlobalLock", api_GlobalLock), E("GlobalUnlock", api_GlobalUnlock), E("GlobalReAlloc", api_GlobalReAlloc), E("GlobalSize", api_GlobalSize),
    E("CompareStringW", api_CompareStringW), E("LCMapStringW", api_LCMapStringW), E("GetLocaleInfoW", api_GetLocaleInfoW), E("GetUserDefaultLCID", api_GetUserDefaultLCID), E("GetSystemDefaultLCID", api_GetUserDefaultLCID),
    E("GetUserDefaultLangID", api_GetSystemDefaultLangID), E("GetSystemDefaultLangID", api_GetSystemDefaultLangID), E("GetACP", api_GetACP), E("GetOEMCP", api_GetACP),
    E("CharUpperW", api_CharUpperW), E("CharLowerW", api_CharLowerW), E("CharNextW", api_CharNextW), E("CharPrevW", api_CharPrevW),
    E("_strupr", api_strupr), E("_strlwr", api_strlwr), E("_wcsupr", api_wcsupr), E("_wcslwr", api_wcslwr), E("sscanf", api_sscanf),
    E("CoInitializeEx", api_zero), E("CoInitialize", api_zero), E("CoUninitialize", api_zero), E("CoCreateInstance", api_CoCreateInstance), E("CoTaskMemAlloc", api_zero),
    E("OleInitialize", api_zero), E("OleUninitialize", api_zero), E("InitCommonControlsEx", api_one), E("InitCommonControls", api_zero), E("SHInitDialog", api_one),
    E("GetSystemPowerStatusEx", api_GetSystemPowerStatusEx), E("GetSystemPowerStatusEx2", api_GetSystemPowerStatusEx), E("GetStartupInfo", api_GetStartupInfo),
    E("FormatMessageW", api_FormatMessageW), E("timeBeginPeriod", api_timeBeginPeriod), E("timeEndPeriod", api_timeBeginPeriod), E("DeviceIoControl", api_DeviceIoControl), E("RequestPowerNotifications", api_RequestPowerNotifications), E("OpenEventW", api_OpenEventW), E("waveOutGetDevCaps", api_waveOutGetDevCaps),
    E("SHGetSpecialFolderPathW", api_SHGetSpecialFolderPathW), E("SetPowerRequirement", api_zero), E("ReleasePowerRequirement", api_zero), E("SetSystemPowerState", api_zero),
    E("PowerPolicyNotify", api_zero), E("ShellExecuteEx", api_zero), E("GetOpenFileNameW", api_zero), E("GetSaveFileNameW", api_zero), E("WSAStartup", api_E_FAIL), E("IsProcessorFeaturePresent", api_zero),
    E("EnableHardwareKeyboard", api_zero), E("GetKeyboardType", api_zero), E("Beep", api_one), E("SetThreadLocale", api_one),
};
void api_misc_init(void) {
    for (int i = 0; i < nrv; i++) free(rv[i].data);
    nrv = 0;
    api_register(mdefs, sizeof mdefs / sizeof mdefs[0]);
}
