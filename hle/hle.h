/* GizmondoDroid - HLE layer: runs Gizmondo (Windows CE, ARM920T) programs directly.
 * Guest code runs on the ARM core in core/; calls into coredll/gx/etc. are trapped and
 * implemented on the host. No firmware is needed. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "../core/cpu.h"

#define SCREEN_W 320
#define SCREEN_H 240
#define GM_LIMIT   0x20000000u      /* guest addresses below this are real memory */
#define TRAP_BASE  0x20000000u      /* imported functions resolve to addresses from here */
#define KDATA_BASE 0xFFFFC000u      /* kernel data page(s) that CE code reads directly */
#define FB_ADDR    0x0F000000u      /* display framebuffer (RGB565) lives in guest memory */

/* ---- guest memory (mem.c) */
void     gm_init(void);
int      gm_map(uint32_t addr, uint32_t size);     /* map zeroed pages; 0 if they overlap something */
int      gm_range_free(uint32_t addr, uint32_t size);
uint8_t *gm_ptr(uint32_t a);                       /* host pointer or NULL (valid within one mapping) */
uint32_t gm_alloc(uint32_t size);                  /* heap, 8-byte aligned; 0 on failure */
void     gm_free(uint32_t a);
uint32_t gm_realloc(uint32_t a, uint32_t size);
uint32_t gm_size(uint32_t a);                      /* usable size of a heap block */
uint32_t gm_valloc(uint32_t addr, uint32_t size);  /* page-granular; addr 0 = anywhere */
int      gm_vfree(uint32_t addr);
uint32_t r32(uint32_t a); uint16_t r16(uint32_t a); uint8_t r8(uint32_t a);
void     w32(uint32_t a, uint32_t v); void w16(uint32_t a, uint16_t v); void w8(uint32_t a, uint8_t v);
void     gm_read(void *dst, uint32_t a, uint32_t n);
void     gm_write(uint32_t a, const void *src, uint32_t n);
int      gs_read(uint32_t a, char *out, int max);          /* ASCII string -> host */
int      gw_read(uint32_t a, char *out, int max);          /* UTF-16 string -> UTF-8 */
int      gw_len(uint32_t a);
uint32_t gs_new(const char *s);                            /* allocate guest ASCII string */
uint32_t gw_new(const char *utf8);                         /* allocate guest UTF-16 string */
void     gw_write(uint32_t a, const char *utf8, int maxchars);
extern int gm_trace_unmapped;

/* ---- calling convention helpers */
static inline uint32_t A(int i) { return i < 4 ? cpu.r[i] : r32(cpu.r[13] + 4 * (i - 4)); }
static inline void R(uint32_t v) { cpu.r[0] = v; }
static inline void R64(uint64_t v) { cpu.r[0] = (uint32_t)v; cpu.r[1] = (uint32_t)(v >> 32); }
static inline double D_arg(int reg) { uint64_t u = cpu.r[reg] | ((uint64_t)cpu.r[reg + 1] << 32); double d; memcpy(&d, &u, 8); return d; }
static inline void R_double(double d) { uint64_t u; memcpy(&u, &d, 8); R64(u); }
static inline float F_arg(int reg) { uint32_t u = cpu.r[reg]; float f; memcpy(&f, &u, 4); return f; }
static inline void R_float(float f) { uint32_t u; memcpy(&u, &f, 4); cpu.r[0] = u; }

/* ---- API registry (api.c) */
typedef void (*ApiFn)(void);
typedef struct { const char *name; ApiFn fn; } ApiDef;
void     api_register(const ApiDef *defs, int n);
uint32_t api_trap_for(const char *dll, const char *name);   /* address guest code should call */
void     api_hook(uint32_t pc);
void     api_reset(void);                                   /* forget all traps (new program) */
void     api_return(void);                                  /* default return: pc = lr */
extern int api_flow_changed;                                /* handler redirected control itself */
const char *api_trap_name(uint32_t addr);
void     hle_log(const char *fmt, ...);
extern int hle_verbose;

/* nested call into guest code (callbacks); returns r0 */
uint32_t guest_call(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
extern int guest_depth;

/* ---- PE modules (pe.c) */
typedef struct Module {
    char name[64], path[512];
    uint32_t base, size, entry, export_va, export_size, rsrc_va, rsrc_size;
    int is_dll, refs, builtin;
    uint16_t machine;
} Module;
Module  *pe_load_exe(const char *hostpath, char *err, int errlen);
Module  *pe_load_dll(const char *name);                    /* by file name, searched in module paths */
Module  *pe_find(uint32_t hmod);
Module  *pe_by_name(const char *name);
uint32_t pe_proc(Module *m, const char *name, int ordinal);
void     pe_add_search(const char *hostdir);
void     pe_reset(void);
uint32_t pe_resource(Module *m, uint32_t type, uint32_t name, uint32_t *size_out);  /* type/name: id (<0x10000) or guest wide-string pointer */
extern Module *main_module;

/* ---- kernel objects, threads, sync (kern.c) */
enum { H_NONE, H_FILE, H_FIND, H_EVENT, H_MUTEX, H_SEM, H_THREAD, H_PROCESS, H_WINDOW, H_DC, H_BITMAP,
       H_BRUSH, H_PEN, H_FONT, H_PALETTE, H_MODULE, H_MAPPING, H_REGKEY, H_WAVE, H_MISC, H_REGION };
typedef struct Obj Obj;
uint32_t h_new(int type, void *p);           /* returns handle value (non-zero) */
void    *h_get(uint32_t h, int type);        /* NULL if wrong type */
int      h_type(uint32_t h);
void     h_close(uint32_t h);
extern uint64_t vcycles;                     /* emulated CPU cycles */
extern uint32_t cpu_hz;
uint32_t tick_ms(void);
void     kern_init(void);
int      kern_start_main(Module *m, const char *cmdline);
void     kern_run_frame(void);               /* run one 1/60 s slice of the machine */
void     kern_set_error(uint32_t e);
void     kern_wait_message(uint32_t timeout_ms);
void     kern_yield(void);
uint32_t kern_thread_id(void);
extern int hle_running;
extern int hle_exit_code;
extern int hle_exec_pending;                 /* CreateProcess of another program was requested */
extern char hle_exec_path[512], hle_exec_args[512];

/* ---- file system (fs.c) */
void fs_set_roots(const char *sdcard, const char *sysroot);
int  fs_map(const char *guest_path, char *host_out, int outlen, int create_dirs);   /* guest path -> host path */
extern char fs_cwd[512];
void fs_init_api(void);

/* ---- display, window, input (win.c, gdi.c, gx.c) */
extern uint16_t *fb_host(void);              /* RGB565 320x240 */
void win_init_api(void);
void gdi_init_api(void);
void gx_init_api(void);
void win_poll(void);                          /* timers, paint, input -> messages */
int  win_has_message_waiter(void);
void input_key(int vk, int down);
void input_touch(int x, int y, int down);
extern uint8_t key_state[256];
extern int frame_dirty;
void audio_init_api(void);
void audio_frame(int frames);                 /* advance audio by this many output frames */
int  audio_read(int16_t *out, int frames);    /* host side: pull mixed stereo frames */
extern uint32_t audio_rate;

/* api groups */
void api_kernel_init(void);
void api_crt_init(void);
void api_misc_init(void);
/* printf engine shared by sprintf/wsprintf/etc */
typedef uint32_t (*ArgFn)(void *ctx, int is64);
int  hle_format(char *out, int outmax, const char *fmt, int wide_fmt, uint32_t va, int va_is_ptr, int first_arg);
