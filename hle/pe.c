/* GizmondoDroid - PE (Windows CE ARM) loader */
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include "coredll_ords.inc"

Module *main_module;
static Module *mods[96]; static int nmods;
static char search[16][512]; static int nsearch;
static uint32_t next_dll_hint;

static uint32_t le32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t le16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

void pe_add_search(const char *dir) {
    for (int i = 0; i < nsearch; i++) if (!strcmp(search[i], dir)) return;
    if (nsearch < 16) snprintf(search[nsearch++], sizeof search[0], "%s", dir);
}

static const char *const builtin[] = {
    "coredll", "gx", "aygshell", "commctrl", "toolhelp", "mmtimer", "winsock", "ws2", "ole32", "oleaut32",
    "ceshell", "commdlg", "imgdecmp", "cemapi", "notify", "pimstore", "wininet", "iphlpapi", "ddraw", "msgstore",
    "phone", "sms", "gpsapi", "winmm", "devload", NULL };

static int is_builtin(const char *name) {
    char b[80]; snprintf(b, sizeof b, "%s", name);
    char *dot = strrchr(b, '.'); if (dot) *dot = 0;
    for (int i = 0; builtin[i]; i++) if (!strcasecmp(b, builtin[i])) return 1;
    return 0;
}

void pe_reset(void) {
    for (int i = 0; i < nmods; i++) free(mods[i]);
    nmods = 0; main_module = NULL; nsearch = 0;
}

Module *pe_by_name(const char *name) {
    char b[80]; snprintf(b, sizeof b, "%s", name);
    char *dot = strrchr(b, '.'); if (dot) *dot = 0;
    for (int i = 0; i < nmods; i++) {
        char m[80]; snprintf(m, sizeof m, "%s", mods[i]->name);
        char *d = strrchr(m, '.'); if (d) *d = 0;
        if (!strcasecmp(m, b)) return mods[i];
    }
    return NULL;
}

Module *pe_find(uint32_t hmod) {
    for (int i = 0; i < nmods; i++) if (mods[i]->base == hmod) return mods[i];
    return NULL;
}

static int find_file(const char *name, char *out, int outlen) {
    for (int i = 0; i < nsearch; i++) {
        DIR *d = opendir(search[i]); if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) if (!strcasecmp(e->d_name, name)) { snprintf(out, outlen, "%s/%s", search[i], e->d_name); closedir(d); return 1; }
        closedir(d);
    }
    return 0;
}

static uint32_t find_base(uint32_t size) {
    for (uint32_t a = 0x01000000; a + size < 0x04000000; a += 0x10000) if (gm_range_free(a, size)) return a;
    for (uint32_t a = 0x12000000; a + size < 0x1F000000; a += 0x10000) if (gm_range_free(a, size)) return a;
    return 0;
}

static uint32_t coredll_name_ptr(int ord, char *buf, int n) {
    int lo = 0, hi = (int)(sizeof coredll_ords / sizeof coredll_ords[0]) - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (coredll_ords[m].ord == ord) { snprintf(buf, n, "%s", coredll_ords[m].name); return 1; } if (coredll_ords[m].ord < ord) lo = m + 1; else hi = m - 1; }
    snprintf(buf, n, "coredll!#%d", ord); return 0;
}

static Module *load_image(const char *path, const char *name, int is_main, char *err, int errlen);

static uint32_t resolve_import(const char *dll, const char *fname, int ordinal) {
    Module *m = NULL;
    if (!is_builtin(dll)) {
        m = pe_by_name(dll);
        if (!m) m = pe_load_dll(dll);
    }
    if (m && !m->builtin) {
        uint32_t a = pe_proc(m, fname, ordinal);
        if (a) return a;
        char nm[160]; snprintf(nm, sizeof nm, "%s!%s", dll, fname ? fname : "#ord");
        if (!fname) snprintf(nm, sizeof nm, "%s!#%d", dll, ordinal);
        return api_trap_for(dll, nm);
    }
    char nm[160];
    if (fname) snprintf(nm, sizeof nm, "%s", fname);
    else if (!strncasecmp(dll, "coredll", 7)) coredll_name_ptr(ordinal, nm, sizeof nm);
    else snprintf(nm, sizeof nm, "%s!#%d", dll, ordinal);
    return api_trap_for(dll, nm);
}

static void process_imports(Module *m) {
    uint32_t imp = r32(m->base + 0);   /* placeholder to keep compilers quiet */
    (void)imp;
    uint32_t pe = m->base + r32(m->base + 0x3C);
    uint32_t va = r32(pe + 0x80), sz = r32(pe + 0x84);
    if (!va || !sz) return;
    for (uint32_t d = m->base + va; ; d += 20) {
        uint32_t ilt = r32(d), nameva = r32(d + 12), iat = r32(d + 16);
        if (!nameva && !iat) break;
        char dll[128]; gs_read(m->base + nameva, dll, sizeof dll);
        uint32_t t = m->base + (ilt ? ilt : iat), slot = m->base + iat;
        for (;; t += 4, slot += 4) {
            uint32_t v = r32(t);
            if (!v) break;
            uint32_t addr;
            if (v & 0x80000000u) addr = resolve_import(dll, NULL, v & 0xFFFF);
            else { char fn[160]; gs_read(m->base + v + 2, fn, sizeof fn); addr = resolve_import(dll, fn, 0); }
            w32(slot, addr);
        }
    }
}

static void apply_relocs(Module *m, uint32_t delta) {
    uint32_t pe = m->base + r32(m->base + 0x3C);
    uint32_t va = r32(pe + 0xA0), sz = r32(pe + 0xA4);
    if (!va || !sz) return;
    uint32_t p = m->base + va, end = p + sz;
    while (p + 8 <= end) {
        uint32_t page = r32(p), bsz = r32(p + 4);
        if (bsz < 8) break;
        for (uint32_t q = p + 8; q + 2 <= p + bsz; q += 2) {
            uint16_t e = r16(q); int ty = e >> 12; uint32_t off = e & 0xFFF;
            uint32_t a = m->base + page + off;
            if (ty == 3) w32(a, r32(a) + delta);
            else if (ty != 0) hle_log("%s: unsupported reloc type %d", m->name, ty);
        }
        p += bsz;
    }
}

static Module *load_image(const char *path, const char *name, int is_main, char *err, int errlen) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errlen, "can't open %s", path); return NULL; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(len);
    if (fread(buf, 1, len, f) != (size_t)len) { fclose(f); free(buf); snprintf(err, errlen, "read error"); return NULL; }
    fclose(f);
    if (len < 0x200 || buf[0] != 'M' || buf[1] != 'Z') { free(buf); snprintf(err, errlen, "%s is not a Windows CE program", name); return NULL; }
    uint32_t pe = le32(buf + 0x3C);
    if (pe + 0x100 > (uint32_t)len || memcmp(buf + pe, "PE\0\0", 4)) { free(buf); snprintf(err, errlen, "%s: bad PE header", name); return NULL; }
    uint16_t machine = le16(buf + pe + 4), nsec = le16(buf + pe + 6), optsz = le16(buf + pe + 20), chars = le16(buf + pe + 22);
    const uint8_t *opt = buf + pe + 24;
    if (machine != 0x1C0 && machine != 0x1C2 && machine != 0x1C4 && machine != 0x1C6) {
        free(buf); snprintf(err, errlen, "%s is not an ARM program (machine %04x)", name, machine); return NULL;
    }
    uint32_t entry = le32(opt + 16), base = le32(opt + 28), size = le32(opt + 56), hdrs = le32(opt + 60);
    uint32_t reloc_sz = le32(opt + 96 + 5 * 8 + 4);
    size = (size + 0xFFF) & ~0xFFFu;
    uint32_t at = base;
    if (!base || base + size > GM_LIMIT - 0x1000 || !gm_range_free(base, size)) {
        if (!reloc_sz) { free(buf); snprintf(err, errlen, "%s: no room at %08x and not relocatable", name, base); return NULL; }
        at = find_base(size);
        if (!at) { free(buf); snprintf(err, errlen, "%s: out of address space", name); return NULL; }
    }
    if (!gm_map(at, size)) { free(buf); snprintf(err, errlen, "%s: can't map image", name); return NULL; }
    gm_write(at, buf, hdrs < (uint32_t)len ? hdrs : (uint32_t)len);
    const uint8_t *sh = opt + optsz;
    for (int i = 0; i < nsec; i++, sh += 40) {
        uint32_t vsz = le32(sh + 8), va = le32(sh + 12), rsz = le32(sh + 16), rptr = le32(sh + 20);
        uint32_t n = rsz < vsz || !vsz ? rsz : vsz;
        if (rptr + n > (uint32_t)len) n = rptr < (uint32_t)len ? len - rptr : 0;
        if (va + n <= size) gm_write(at + va, buf + rptr, n);
    }
    Module *m = calloc(1, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", name); snprintf(m->path, sizeof m->path, "%s", path);
    m->base = at; m->size = size; m->machine = machine; m->is_dll = (chars & 0x2000) != 0 && !is_main;
    m->entry = entry ? at + entry : 0;
    if (machine == 0x1C2 && m->entry) m->entry |= 1;           /* THUMB image: entry is Thumb code */
    m->export_va = le32(opt + 96); m->export_size = le32(opt + 100);
    m->rsrc_va = le32(opt + 96 + 2 * 8); m->rsrc_size = le32(opt + 96 + 2 * 8 + 4);
    m->refs = 1;
    free(buf);
    if (nmods < 96) mods[nmods++] = m;
    if (at != base) apply_relocs(m, at - base);
    process_imports(m);
    cpu_code_changed();
    return m;
}

Module *pe_load_exe(const char *hostpath, char *err, int errlen) {
    const char *slash = strrchr(hostpath, '/');
    char dir[512];
    if (slash) { snprintf(dir, sizeof dir, "%.*s", (int)(slash - hostpath), hostpath); pe_add_search(dir); }
    Module *m = load_image(hostpath, slash ? slash + 1 : hostpath, 1, err, errlen);
    if (m) main_module = m;
    return m;
}

Module *pe_load_dll(const char *name) {
    Module *m = pe_by_name(name);
    if (m) { m->refs++; return m; }
    char path[600], nm[128];
    snprintf(nm, sizeof nm, "%s", name);
    if (!strchr(nm, '.')) strncat(nm, ".dll", sizeof nm - strlen(nm) - 1);
    if (!find_file(nm, path, sizeof path)) { hle_log("DLL not found: %s", name); return NULL; }
    char err[200] = "";
    m = load_image(path, nm, 0, err, sizeof err);
    if (!m) { hle_log("DLL %s: %s", name, err); return NULL; }
    m->is_dll = 1;
    if (m->entry) {
        uint32_t ok = guest_call(m->entry, m->base, 1, 0, 0);
        if (!ok) hle_log("%s: DllMain(ATTACH) returned 0", nm);
    }
    return m;
}

uint32_t pe_proc(Module *m, const char *name, int ordinal) {
    if (!m || !m->export_va) return 0;
    uint32_t ex = m->base + m->export_va;
    uint32_t obase = r32(ex + 16), nfun = r32(ex + 20), nnames = r32(ex + 24);
    uint32_t fun = m->base + r32(ex + 28), nam = m->base + r32(ex + 32), ords = m->base + r32(ex + 36);
    int idx = -1;
    if (name) {
        for (uint32_t i = 0; i < nnames; i++) {
            char s[160]; gs_read(m->base + r32(nam + 4 * i), s, sizeof s);
            if (!strcmp(s, name)) { idx = r16(ords + 2 * i); break; }
        }
    } else idx = ordinal - (int)obase;
    if (idx < 0 || (uint32_t)idx >= nfun) return 0;
    uint32_t rva = r32(fun + 4 * idx);
    if (!rva) return 0;
    uint32_t a = m->base + rva;
    if (m->machine == 0x1C2) a |= 1;
    return a;
}

/* ---- resources */
static int name_matches(uint32_t entry_name, uint32_t want, uint32_t rbase) {
    if (want < 0x10000) return !(entry_name & 0x80000000u) && entry_name == want;
    if (!(entry_name & 0x80000000u)) return 0;
    uint32_t p = rbase + (entry_name & 0x7FFFFFFF);
    uint32_t n = r16(p);
    for (uint32_t i = 0; i < n; i++) {
        uint16_t a = r16(p + 2 + 2 * i), b = r16(want + 2 * i);
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    return r16(want + 2 * n) == 0;
}

static uint32_t res_find(uint32_t rbase, uint32_t dir, uint32_t want, int *is_dir) {
    uint32_t nn = r16(dir + 12), ni = r16(dir + 14);
    for (uint32_t i = 0; i < nn + ni; i++) {
        uint32_t e = dir + 16 + 8 * i, name = r32(e), off = r32(e + 4);
        int match = (want == 0xFFFFFFFFu) ? 1 : name_matches(name, want, rbase);
        if (match) { *is_dir = (off & 0x80000000u) != 0; return rbase + (off & 0x7FFFFFFF); }
    }
    return 0;
}

uint32_t pe_resource(Module *m, uint32_t type, uint32_t name, uint32_t *size_out) {
    if (!m || !m->rsrc_va) return 0;
    uint32_t rbase = m->base + m->rsrc_va; int isd;
    uint32_t d1 = res_find(rbase, rbase, type, &isd); if (!d1 || !isd) return 0;
    uint32_t d2 = res_find(rbase, d1, name, &isd); if (!d2 || !isd) return 0;
    uint32_t de = res_find(rbase, d2, 0xFFFFFFFFu, &isd); if (!de || isd) return 0;
    if (size_out) *size_out = r32(de + 4);
    return m->base + r32(de);
}
