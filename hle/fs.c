/* GizmondoDroid - file system: guest paths map onto host directories, case-insensitively */
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>

char fs_cwd[512] = "\\";
static char root_sd[512] = ".", root_sys[512] = ".";

void fs_set_roots(const char *sdcard, const char *sysroot) {
    snprintf(root_sd, sizeof root_sd, "%s", sdcard && *sdcard ? sdcard : ".");
    snprintf(root_sys, sizeof root_sys, "%s", sysroot && *sysroot ? sysroot : ".");
    mkdir(root_sys, 0777);
}

/* case-insensitive resolve of one path component inside dir; returns 1 if found */
static int resolve_comp(char *dir, const char *comp) {
    char t[1100]; snprintf(t, sizeof t, "%s/%s", dir, comp);
    struct stat st; if (!stat(t, &st)) { snprintf(dir + strlen(dir), 1100 - strlen(dir), "/%s", comp); return 1; }
    DIR *d = opendir(dir); if (!d) return 0;
    struct dirent *e; int ok = 0;
    while ((e = readdir(d))) if (!strcasecmp(e->d_name, comp)) { snprintf(dir + strlen(dir), 1100 - strlen(dir), "/%s", e->d_name); ok = 1; break; }
    closedir(d); return ok;
}

int fs_map(const char *gp, char *out, int outlen, int create) {
    char path[600]; const char *p = gp;
    if (*p != '\\' && *p != '/') snprintf(path, sizeof path, "%s\\%s", fs_cwd, p); else snprintf(path, sizeof path, "%s", p);
    for (char *c = path; *c; c++) if (*c == '\\') *c = '/';
    /* normalise . and .. */
    char norm[600]; int n = 0; norm[0] = 0;
    char *tok, *sv; char tmp[600]; snprintf(tmp, sizeof tmp, "%s", path);
    char *parts[64]; int np = 0;
    for (tok = strtok_r(tmp, "/", &sv); tok && np < 64; tok = strtok_r(NULL, "/", &sv)) {
        if (!strcmp(tok, ".") || !*tok) continue;
        if (!strcmp(tok, "..")) { if (np) np--; continue; }
        parts[np++] = tok;
    }
    (void)n; (void)norm;
    int start = 0; char base[1100];
    if (np && (!strcasecmp(parts[0], "Storage Card") || !strcasecmp(parts[0], "SD Card") || !strcasecmp(parts[0], "SDMMC") || !strcasecmp(parts[0], "Hard Disk") || !strcasecmp(parts[0], "CF Card") || !strcasecmp(parts[0], "MMC Card"))) {
        snprintf(base, sizeof base, "%s", root_sd); start = 1;
    } else snprintf(base, sizeof base, "%s", root_sys);
    for (int i = start; i < np; i++) {
        if (!resolve_comp(base, parts[i])) {
            if (i < np - 1 && !create) return 0;
            /* component doesn't exist: append as-is (file may be created) */
            for (int k = i; k < np; k++) {
                if (create && k < np - 1) { mkdir(base, 0777); }
                snprintf(base + strlen(base), sizeof base - strlen(base), "/%s", parts[k]);
            }
            if (create) { /* make parents */ char *s = strrchr(base, '/'); if (s) { *s = 0; mkdir(base, 0777); *s = '/'; } }
            break;
        }
    }
    snprintf(out, (size_t)outlen, "%s", base);
    return 1;
}

void fs_guest_path(const char *host, char *out, int n) {
    size_t l = strlen(root_sd);
    if (!strncmp(host, root_sd, l)) { snprintf(out, (size_t)n, "\\Storage Card%s", host + l); }
    else {
        l = strlen(root_sys);
        snprintf(out, (size_t)n, "%s", !strncmp(host, root_sys, l) ? host + l : host);
        if (!out[0]) snprintf(out, (size_t)n, "\\");
    }
    for (char *c = out; *c; c++) if (*c == '/') *c = '\\';
}

/* ------------------------------------------------------------ files */
typedef struct { int fd; char path[600]; int append; uint32_t access; } FileH;
typedef struct { DIR *d; char dir[600]; char pat[260]; } FindH;
typedef struct { int fd; uint32_t size; } MapH;

static int pat_match(const char *p, const char *s) {
    for (; *p; p++, s++) {
        if (*p == '*') { while (p[1] == '*') p++; if (!p[1]) return 1; for (; *s; s++) if (pat_match(p + 1, s)) return 1; return pat_match(p + 1, s); }
        if (!*s) return 0;
        if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s)) return 0;
    }
    return !*s;
}
#define FT_EPOCH 116444736000000000ULL
static void put_ft(uint32_t a, time_t t) { uint64_t ft = FT_EPOCH + (uint64_t)t * 10000000ULL; w32(a, (uint32_t)ft); w32(a + 4, (uint32_t)(ft >> 32)); }

static int map_wide(uint32_t gp, char *out, int create) {
    char g[600]; gw_read(gp, g, sizeof g);
    return fs_map(g, out, 600, create);
}
static void api_CreateFileW(void) {
    char h[600]; uint32_t acc = A(1), disp = A(4);
    int create = (acc & 0x40000000) != 0;
    if (!map_wide(A(0), h, create)) { R(0xFFFFFFFF); kern_set_error(3); return; }
    int fl = (acc & 0x40000000) ? ((acc & 0x80000000) ? O_RDWR : O_WRONLY) : O_RDONLY;
    struct stat st; int exists = !stat(h, &st);
    switch (disp) {
    case 1: fl |= O_CREAT | O_EXCL; if (exists) { R(0xFFFFFFFF); kern_set_error(80); return; } break;   /* CREATE_NEW */
    case 2: fl |= O_CREAT | O_TRUNC; break;                                                          /* CREATE_ALWAYS */
    case 3: if (!exists) { R(0xFFFFFFFF); kern_set_error(2); return; } break;                        /* OPEN_EXISTING */
    case 4: fl |= O_CREAT; break;                                                                    /* OPEN_ALWAYS */
    case 5: if (!exists) { R(0xFFFFFFFF); kern_set_error(2); return; } fl |= O_TRUNC; break;         /* TRUNCATE_EXISTING */
    }
    if (exists && S_ISDIR(st.st_mode)) { R(0xFFFFFFFF); kern_set_error(5); return; }
    int fd = open(h, fl, 0666);
    if (fd < 0) { R(0xFFFFFFFF); kern_set_error(errno == ENOENT ? 2 : 5); return; }
    FileH *f = calloc(1, sizeof *f); f->fd = fd; f->access = acc; snprintf(f->path, sizeof f->path, "%s", h);
    R(h_new(H_FILE, f));
    kern_set_error(disp == 4 || disp == 2 ? (exists ? 183 : 0) : 0);
}
static void api_ReadFile(void) {
    FileH *f = h_get(A(0), H_FILE); uint32_t buf = A(1), n = A(2);
    if (!f) { R(0); kern_set_error(6); return; }
    uint32_t total = 0; uint8_t tmp[16384];
    while (total < n) {
        uint32_t c = n - total > sizeof tmp ? sizeof tmp : n - total;
        ssize_t r = read(f->fd, tmp, c); if (r <= 0) break;
        gm_write(buf + total, tmp, (uint32_t)r); total += (uint32_t)r; if ((uint32_t)r < c) break;
    }
    if (A(3)) w32(A(3), total);
    R(1);
}
static void api_WriteFile(void) {
    FileH *f = h_get(A(0), H_FILE); uint32_t buf = A(1), n = A(2);
    if (!f) { R(0); kern_set_error(6); return; }
    uint32_t total = 0; uint8_t tmp[16384];
    while (total < n) {
        uint32_t c = n - total > sizeof tmp ? sizeof tmp : n - total;
        gm_read(tmp, buf + total, c); ssize_t r = write(f->fd, tmp, c); if (r <= 0) break; total += (uint32_t)r;
    }
    if (A(3)) w32(A(3), total);
    R(total == n);
}
static void api_SetFilePointer(void) {
    FileH *f = h_get(A(0), H_FILE); if (!f) { R(0xFFFFFFFF); return; }
    int64_t off = (int32_t)A(1); if (A(2)) off |= (int64_t)(int32_t)r32(A(2)) << 32;
    off_t r = lseek(f->fd, off, A(3) == 0 ? SEEK_SET : A(3) == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) { R(0xFFFFFFFF); return; }
    if (A(2)) w32(A(2), (uint32_t)((uint64_t)r >> 32));
    R((uint32_t)r);
}
static void api_GetFileSize(void) {
    FileH *f = h_get(A(0), H_FILE); struct stat st;
    if (!f || fstat(f->fd, &st)) { R(0xFFFFFFFF); return; }
    if (A(1)) w32(A(1), (uint32_t)((uint64_t)st.st_size >> 32));
    R((uint32_t)st.st_size);
}
static void api_SetEndOfFile(void) { FileH *f = h_get(A(0), H_FILE); if (!f) { R(0); return; } R(!ftruncate(f->fd, lseek(f->fd, 0, SEEK_CUR))); }
static void api_FlushFileBuffers(void) { FileH *f = h_get(A(0), H_FILE); if (f) fsync(f->fd); R(1); }
void fs_close_obj(int type, void *p) { if (type == H_FILE) { FileH *f = p; close(f->fd); free(f); } else if (type == H_FIND) { FindH *f = p; closedir(f->d); free(f); } else if (type == H_MAPPING) free(p); }
static void api_GetFileTime(void) {
    FileH *f = h_get(A(0), H_FILE); struct stat st; if (!f || fstat(f->fd, &st)) { R(0); return; }
    if (A(1)) put_ft(A(1), st.st_ctime); if (A(2)) put_ft(A(2), st.st_atime); if (A(3)) put_ft(A(3), st.st_mtime);
    R(1);
}
static void api_SetFileTime(void) { R(1); }
static void api_DeleteFileW(void) { char h[600]; if (!map_wide(A(0), h, 0) || unlink(h)) { R(0); kern_set_error(2); return; } R(1); }
static void api_MoveFileW(void) { char a[600], b[600]; if (!map_wide(A(0), a, 0) || !map_wide(A(1), b, 1) || rename(a, b)) { R(0); kern_set_error(2); return; } R(1); }
static void api_CopyFileW(void) {
    char a[600], b[600]; if (!map_wide(A(0), a, 0) || !map_wide(A(1), b, 1)) { R(0); kern_set_error(2); return; }
    if (A(2) && !access(b, F_OK)) { R(0); kern_set_error(80); return; }
    int i = open(a, O_RDONLY); if (i < 0) { R(0); kern_set_error(2); return; }
    int o = open(b, O_WRONLY | O_CREAT | O_TRUNC, 0666); char buf[8192]; ssize_t r;
    while ((r = read(i, buf, sizeof buf)) > 0) if (write(o, buf, (size_t)r) != r) break;
    close(i); close(o); R(1);
}
static void api_CreateDirectoryW(void) { char h[600]; if (!map_wide(A(0), h, 0)) { R(0); return; } R(!mkdir(h, 0777)); }
static void api_RemoveDirectoryW(void) { char h[600]; if (!map_wide(A(0), h, 0)) { R(0); return; } R(!rmdir(h)); }
static void api_GetFileAttributesW(void) {
    char h[600]; struct stat st;
    if (!map_wide(A(0), h, 0) || stat(h, &st)) { R(0xFFFFFFFF); kern_set_error(2); return; }
    R(S_ISDIR(st.st_mode) ? 0x10 : 0x20);
}
static void api_SetFileAttributesW(void) { R(1); }

static void fill_find(uint32_t out, const char *dir, const char *name) {
    char full[1300]; struct stat st; snprintf(full, sizeof full, "%s/%s", dir, name);
    memset(gm_ptr(out), 0, 592);
    if (stat(full, &st)) return;
    w32(out, S_ISDIR(st.st_mode) ? 0x10 : 0x20);
    put_ft(out + 4, st.st_ctime); put_ft(out + 12, st.st_atime); put_ft(out + 20, st.st_mtime);
    w32(out + 28, (uint32_t)((uint64_t)st.st_size >> 32)); w32(out + 32, (uint32_t)st.st_size);
    gw_write(out + 40, name, 260);
}
static int find_next(FindH *fh, uint32_t out) {
    struct dirent *e;
    while ((e = readdir(fh->d))) { if (pat_match(fh->pat, e->d_name)) { fill_find(out, fh->dir, e->d_name); return 1; } }
    return 0;
}
static void api_FindFirstFileW(void) {
    char g[600], h[600]; gw_read(A(0), g, sizeof g);
    char *sl = strrchr(g, '\\'); char pat[260]; char dirg[600];
    if (sl) { snprintf(pat, sizeof pat, "%s", sl + 1); *sl = 0; snprintf(dirg, sizeof dirg, "%s", g[0] ? g : "\\"); }
    else { snprintf(pat, sizeof pat, "%s", g); snprintf(dirg, sizeof dirg, "%s", fs_cwd); }
    if (!fs_map(dirg, h, sizeof h, 0)) { R(0xFFFFFFFF); kern_set_error(3); return; }
    DIR *d = opendir(h); if (!d) { R(0xFFFFFFFF); kern_set_error(3); return; }
    FindH *fh = calloc(1, sizeof *fh); fh->d = d; snprintf(fh->dir, sizeof fh->dir, "%s", h); snprintf(fh->pat, sizeof fh->pat, "%s", pat);
    if (!find_next(fh, A(1))) { closedir(d); free(fh); R(0xFFFFFFFF); kern_set_error(18); return; }
    R(h_new(H_FIND, fh));
}
static void api_FindNextFileW(void) { FindH *fh = h_get(A(0), H_FIND); if (!fh || !find_next(fh, A(1))) { R(0); kern_set_error(18); return; } R(1); }
static void api_FindClose(void) { FindH *fh = h_get(A(0), H_FIND); if (fh) { closedir(fh->d); free(fh); } h_close(A(0)); R(1); }
static void api_CloseHandleFile(void) { }

static void api_GetFullPathNameW(void) {
    char g[600]; gw_read(A(0), g, sizeof g);
    char out[700]; if (g[0] == '\\') snprintf(out, sizeof out, "%s", g); else snprintf(out, sizeof out, "%s%s%s", fs_cwd, strcmp(fs_cwd, "\\") ? "\\" : "", g);
    gw_write(A(2), out, (int)A(1)); R((uint32_t)strlen(out));
}
static void api_GetTempPathW(void) { gw_write(A(1), "\\Temp\\", (int)A(0)); R(6); }
static void api_GetTempFileNameW(void) { static int n; char b[64]; snprintf(b, sizeof b, "\\Temp\\gz%04x.tmp", ++n); gw_write(A(3), b, 260); R((uint32_t)n); }
static void api_GetDiskFreeSpaceExW(void) { if (A(1)) { w32(A(1), 0x10000000); w32(A(1) + 4, 0); } if (A(2)) { w32(A(2), 0x20000000); w32(A(2) + 4, 0); } if (A(3)) { w32(A(3), 0x10000000); w32(A(3) + 4, 0); } R(1); }
static void api_SetCurrentDirectoryW(void) { gw_read(A(0), fs_cwd, sizeof fs_cwd); R(1); }
static void api_GetCurrentDirectoryW(void) { gw_write(A(1), fs_cwd, (int)A(0)); R((uint32_t)strlen(fs_cwd)); }

/* file mapping */
static void api_CreateFileMappingW(void) {
    FileH *f = h_get(A(0), H_FILE); MapH *m = calloc(1, sizeof *m);
    struct stat st; m->fd = -1;
    if (f && !fstat(f->fd, &st)) { m->fd = f->fd; m->size = (uint32_t)st.st_size; }
    if (A(4)) m->size = A(4);
    R(h_new(H_MAPPING, m));
}
static void api_MapViewOfFile(void) {
    MapH *m = h_get(A(0), H_MAPPING); if (!m) { R(0); return; }
    uint32_t n = A(4) ? A(4) : m->size; uint32_t a = gm_valloc(0, n); if (!a) { R(0); return; }
    if (m->fd >= 0) { uint8_t tmp[8192]; uint32_t off = A(3), got = 0; while (got < n) { ssize_t r = pread(m->fd, tmp, sizeof tmp < n - got ? sizeof tmp : n - got, off + got); if (r <= 0) break; gm_write(a + got, tmp, (uint32_t)r); got += (uint32_t)r; } }
    R(a);
}
static void api_UnmapViewOfFile(void) { gm_vfree(A(0)); R(1); }

/* ------------------------------------------------------------ stdio (FILE*) */
typedef struct { FILE *fp; } CFile;
static void api_wfopen(void) {
    char g[600], m[16], h[600]; gw_read(A(0), g, sizeof g); gw_read(A(1), m, sizeof m);
    if (!fs_map(g, h, sizeof h, strpbrk(m, "wa+") != NULL)) { R(0); return; }
    FILE *fp = fopen(h, m); if (!fp) { R(0); return; }
    R(h_new(H_MISC, fp));
}
static void api_fopen(void) {
    char g[600], m[16], h[600]; gs_read(A(0), g, sizeof g); gs_read(A(1), m, sizeof m);
    if (!fs_map(g, h, sizeof h, strpbrk(m, "wa+") != NULL)) { R(0); return; }
    FILE *fp = fopen(h, m); if (!fp) { R(0); return; }
    R(h_new(H_MISC, fp));
}
static FILE *cf(uint32_t h) { return h_get(h, H_MISC); }
static void api_fclose(void) { FILE *f = cf(A(0)); if (f) fclose(f); h_close(A(0)); R(0); }
static void api_fread(void) {
    FILE *f = cf(A(3)); uint32_t sz = A(1), n = A(2), done = 0; uint8_t tmp[4096];
    if (!f || !sz) { R(0); return; }
    while (done < n) { uint32_t c = (uint32_t)(sizeof tmp / sz); if (!c) c = 1; if (c > n - done) c = n - done; size_t r = fread(tmp, sz, c, f); if (!r) break; gm_write(A(0) + done * sz, tmp, (uint32_t)(r * sz)); done += (uint32_t)r; if (r < c) break; }
    R(done);
}
static void api_fwrite(void) {
    FILE *f = cf(A(3)); uint32_t sz = A(1), n = A(2), done = 0; uint8_t tmp[4096];
    if (!f || !sz) { R(0); return; }
    while (done < n) { uint32_t c = (uint32_t)(sizeof tmp / sz); if (!c) c = 1; if (c > n - done) c = n - done; gm_read(tmp, A(0) + done * sz, c * sz); size_t r = fwrite(tmp, sz, c, f); done += (uint32_t)r; if (r < c) break; }
    R(done);
}
static void api_fseek(void) { FILE *f = cf(A(0)); R(f ? (uint32_t)fseek(f, (int32_t)A(1), (int)A(2)) : 0xFFFFFFFFu); }
static void api_ftell(void) { FILE *f = cf(A(0)); R(f ? (uint32_t)ftell(f) : 0xFFFFFFFFu); }
static void api_feof(void) { FILE *f = cf(A(0)); R(f ? feof(f) != 0 : 1); }
static void api_fflush(void) { FILE *f = cf(A(0)); if (f) fflush(f); R(0); }
static void api_fgetc(void) { FILE *f = cf(A(0)); R(f ? (uint32_t)fgetc(f) : 0xFFFFFFFFu); }
static void api_fputc(void) { FILE *f = cf(A(1)); R(f ? (uint32_t)fputc((int)A(0), f) : 0xFFFFFFFFu); }
static void api_fgets(void) {
    FILE *f = cf(A(2)); int n = (int)A(1); char b[1024]; if (!f || n <= 0) { R(0); return; }
    if (n > 1024) n = 1024; if (!fgets(b, n, f)) { R(0); return; }
    gm_write(A(0), b, (uint32_t)strlen(b) + 1); R(A(0));
}
static void api_fputs(void) { FILE *f = cf(A(1)); char b[2048]; gs_read(A(0), b, sizeof b); if (f) fputs(b, f); R(0); }
static void api_fprintf(void) {
    FILE *f = cf(A(0)); char fm[2048], out[8192]; gs_read(A(1), fm, sizeof fm);
    int n = hle_format(out, sizeof out, fm, 0, 0, 0, 2); if (f) fputs(out, f); R(n);
}
static void api_remove(void) { char g[600], h[600]; gs_read(A(0), g, sizeof g); if (!fs_map(g, h, sizeof h, 0)) { R(0xFFFFFFFF); return; } R((uint32_t)remove(h)); }
static void api_fileno(void) { R(A(0)); }
static void api_rewind(void) { FILE *f = cf(A(0)); if (f) rewind(f); }

#define E(n, f) { n, f }
static void api_CloseHandleHook(void) { }
static const ApiDef fdefs[] = {
    E("CreateFileW", api_CreateFileW), E("ReadFile", api_ReadFile), E("WriteFile", api_WriteFile), E("SetFilePointer", api_SetFilePointer),
    E("GetFileSize", api_GetFileSize), E("SetEndOfFile", api_SetEndOfFile), E("FlushFileBuffers", api_FlushFileBuffers), E("GetFileTime", api_GetFileTime), E("SetFileTime", api_SetFileTime),
    E("DeleteFileW", api_DeleteFileW), E("MoveFileW", api_MoveFileW), E("CopyFileW", api_CopyFileW), E("CreateDirectoryW", api_CreateDirectoryW), E("RemoveDirectoryW", api_RemoveDirectoryW),
    E("GetFileAttributesW", api_GetFileAttributesW), E("SetFileAttributesW", api_SetFileAttributesW),
    E("FindFirstFileW", api_FindFirstFileW), E("FindNextFileW", api_FindNextFileW), E("FindClose", api_FindClose),
    E("GetFullPathNameW", api_GetFullPathNameW), E("GetTempPathW", api_GetTempPathW), E("GetTempFileNameW", api_GetTempFileNameW), E("GetDiskFreeSpaceExW", api_GetDiskFreeSpaceExW),
    E("SetCurrentDirectoryW", api_SetCurrentDirectoryW), E("GetCurrentDirectoryW", api_GetCurrentDirectoryW),
    E("CreateFileMappingW", api_CreateFileMappingW), E("MapViewOfFile", api_MapViewOfFile), E("UnmapViewOfFile", api_UnmapViewOfFile),
    E("_wfopen", api_wfopen), E("fopen", api_fopen), E("fclose", api_fclose), E("fread", api_fread), E("fwrite", api_fwrite), E("fseek", api_fseek), E("ftell", api_ftell),
    E("feof", api_feof), E("fflush", api_fflush), E("fgetc", api_fgetc), E("getc", api_fgetc), E("fputc", api_fputc), E("putc", api_fputc), E("fgets", api_fgets), E("fputs", api_fputs),
    E("fprintf", api_fprintf), E("remove", api_remove), E("_fileno", api_fileno), E("rewind", api_rewind),
};
void fs_init_api(void) { api_register(fdefs, sizeof fdefs / sizeof fdefs[0]); (void)api_CloseHandleFile; (void)api_CloseHandleHook; }
