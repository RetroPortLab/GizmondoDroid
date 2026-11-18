/* GizmondoDroid - API registry, trap dispatch, nested guest calls */
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

int hle_verbose;
int api_flow_changed;
static int log_count;
void (*hle_log_sink)(const char *line);

void hle_log(const char *fmt, ...) {
    if (log_count > 4000) return;
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    log_count++;
    fprintf(stderr, "[hle] %s\n", buf);
    if (hle_log_sink) hle_log_sink(buf);
}

/* ---- registry */
static const ApiDef **tabs; static int *tabn; static int ntabs;
void api_register(const ApiDef *defs, int n) {
    tabs = realloc(tabs, (ntabs + 1) * sizeof *tabs); tabn = realloc(tabn, (ntabs + 1) * sizeof *tabn);
    tabs[ntabs] = defs; tabn[ntabs] = n; ntabs++;
}
static const ApiDef *find_api(const char *name) {
    for (int t = 0; t < ntabs; t++) for (int i = 0; i < tabn[t]; i++) if (!strcmp(tabs[t][i].name, name)) return &tabs[t][i];
    return NULL;
}

/* ---- traps */
typedef struct { char name[96]; ApiFn fn; int hits; int unimpl; } Trap;
static Trap *traps; static int ntraps, captraps;
static uint32_t sentinel_addr;
static uint8_t *bitmap;
#define TRAP_RANGE 0x100000u

static uint32_t new_trap(const char *name, ApiFn fn, int unimpl) {
    if (ntraps >= (int)(TRAP_RANGE / 4)) return TRAP_BASE;
    if (ntraps == captraps) { captraps = captraps ? captraps * 2 : 256; traps = realloc(traps, captraps * sizeof *traps); }
    Trap *t = &traps[ntraps];
    snprintf(t->name, sizeof t->name, "%s", name); t->fn = fn; t->hits = 0; t->unimpl = unimpl;
    uint32_t addr = TRAP_BASE + 4u * ntraps;
    uint32_t bi = (addr - TRAP_BASE) >> 1;
    if (!bitmap) bitmap = calloc(1, TRAP_RANGE / 16 + 8);
    bitmap[bi >> 3] |= 1 << (bi & 7);
    if (addr - TRAP_BASE + 2 > cpu.hle_span) cpu.hle_span = addr - TRAP_BASE + 2;
    cpu.hle_bitmap = bitmap; cpu.hle_hook = api_hook;
    ntraps++;
    cpu_code_changed();
    return addr;
}

void api_reset(void) {
    ntraps = 0; ntabs = 0; sentinel_addr = 0; log_count = 0; guest_depth = 0;
    if (bitmap) memset(bitmap, 0, TRAP_RANGE / 16 + 8);
    cpu.hle_span = 0;
}

const char *api_trap_name(uint32_t addr) {
    if (addr < TRAP_BASE) return NULL;
    uint32_t i = (addr - TRAP_BASE) >> 2;
    return i < (uint32_t)ntraps ? traps[i].name : NULL;
}

static void unimpl_stub(void) {
    uint32_t i = (cpu.r[15] - TRAP_BASE) >> 2;
    Trap *t = &traps[i];
    if (t->hits < 3) hle_log("UNIMPLEMENTED %s(%08x,%08x,%08x,%08x) from %08x", t->name, cpu.r[0], cpu.r[1], cpu.r[2], cpu.r[3], cpu.r[14]);
    cpu.r[0] = strstr(t->name, "!#") ? 1 : 0;   /* ordinal-only shell/commctrl calls: pretend success */
}

static void ret_sentinel(void);
static int call_done;

uint32_t api_trap_for(const char *dll, const char *name) {
    (void)dll;
    for (int i = 0; i < ntraps; i++) if (!strcmp(traps[i].name, name)) return TRAP_BASE + 4u * i;
    const ApiDef *d = find_api(name);
    if (d) return new_trap(name, d->fn, 0);
    return new_trap(name, unimpl_stub, 1);
}

void api_return(void) {
    uint32_t lr = cpu.r[14];
    cpu.cpsr = (cpu.cpsr & ~0x20u) | ((lr & 1) << 5);
    cpu.r[15] = lr & ~1u;
}

extern void psl_call(uint32_t slot);
void api_hook(uint32_t pc) {
    if (pc >= 0xF0000000u) {   /* CE kernel call slot: 0xF0010000 - 4 * (apiset << 8 | method) */
        api_flow_changed = 0; uint32_t lr = cpu.r[14];
        psl_call((0xF0010000u - (pc & ~3u)) >> 2);
        if (!api_flow_changed) { cpu.cpsr = (cpu.cpsr & ~0x20u) | ((lr & 1) << 5); cpu.r[15] = lr & ~1u; }
        return;
    }
    uint32_t i = (pc - TRAP_BASE) >> 2;
    if (i >= (uint32_t)ntraps) return;
    Trap *t = &traps[i];
    t->hits++;
    if (hle_verbose > 1) hle_log("call %s(%08x,%08x,%08x,%08x)", t->name, cpu.r[0], cpu.r[1], cpu.r[2], cpu.r[3]);
    api_flow_changed = 0;
    uint32_t lr = cpu.r[14];
    t->fn();
    if (!api_flow_changed) {
        cpu.cpsr = (cpu.cpsr & ~0x20u) | ((lr & 1) << 5);
        cpu.r[15] = lr & ~1u;
    }
}

/* ---- nested calls into guest code */
int guest_depth;
static void ret_sentinel(void) {
    call_done = 1; cpu.halted = 1; api_flow_changed = 1; cpu.r[15] = sentinel_addr + 4;
}

typedef struct { uint32_t r[16], n, z, c, v, cpsr; int halted; int64_t budget, run_len; } Saved;

uint32_t guest_call(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    if (!sentinel_addr) sentinel_addr = new_trap("<return>", ret_sentinel, 0);
    Saved s;
    memcpy(s.r, cpu.r, sizeof s.r); s.n = cpu.n; s.z = cpu.z; s.c = cpu.c; s.v = cpu.v; s.cpsr = cpu.cpsr;
    s.halted = cpu.halted; s.budget = cpu.budget; s.run_len = cpu.run_len;
    cpu.r[0] = a0; cpu.r[1] = a1; cpu.r[2] = a2; cpu.r[3] = a3;
    cpu.r[14] = sentinel_addr;
    cpu.cpsr = (cpu.cpsr & ~0x20u) | ((fn & 1) << 5);
    cpu.r[15] = fn & ~1u;
    cpu.halted = 0; call_done = 0;
    guest_depth++;
    uint64_t guard = 0;
    while (!call_done && hle_running) {
        int n = cpu_run(200000);
        vcycles += n;
        if (++guard > 20000) { hle_log("guest_call %08x did not return", fn); break; }
        if (cpu.halted && !call_done) { cpu.halted = 0; }   /* blocking inside a callback: just keep going */
    }
    guest_depth--;
    uint32_t ret = cpu.r[0];
    memcpy(cpu.r, s.r, sizeof s.r); cpu.n = s.n; cpu.z = s.z; cpu.c = s.c; cpu.v = s.v; cpu.cpsr = s.cpsr;
    cpu.halted = s.halted; cpu.budget = s.budget; cpu.run_len = s.run_len;
    api_flow_changed = 0;
    return ret;
}
