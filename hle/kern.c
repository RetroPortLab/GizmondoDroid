/* GizmondoDroid - kernel layer: handles, threads and scheduler, sync objects, time, memory and module APIs */
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

uint64_t vcycles;
uint32_t cpu_hz = 400000000;
int hle_running;
int hle_exit_code;
int hle_exec_pending;
char hle_exec_path[512], hle_exec_args[512];

/* ---------------------------------------------------------------- handles */
struct Obj { int type; void *p; };
static Obj *objs; static uint32_t nobjs, capobjs;
#define H_BASE 0x00020000u

uint32_t h_new(int type, void *p) {
    uint32_t i;
    for (i = 0; i < nobjs; i++) if (objs[i].type == H_NONE) break;
    if (i == nobjs) {
        if (nobjs == capobjs) { capobjs = capobjs ? capobjs * 2 : 256; objs = realloc(objs, capobjs * sizeof *objs); }
        nobjs++;
    }
    objs[i].type = type; objs[i].p = p;
    return H_BASE + i * 4;
}
static Obj *h_obj(uint32_t h) {
    if (h < H_BASE || (h - H_BASE) & 3) return NULL;
    uint32_t i = (h - H_BASE) >> 2;
    return i < nobjs && objs[i].type != H_NONE ? &objs[i] : NULL;
}
void *h_get(uint32_t h, int type) { Obj *o = h_obj(h); return o && (type == H_NONE || o->type == type) ? o->p : NULL; }
int h_type(uint32_t h) { Obj *o = h_obj(h); return o ? o->type : H_NONE; }
void h_close(uint32_t h) { Obj *o = h_obj(h); if (o) { o->type = H_NONE; o->p = NULL; } }

/* ---------------------------------------------------------------- threads */
enum { T_FREE, T_RUN, T_READY, T_WAIT, T_DONE, T_MSG };
typedef struct Thread {
    uint32_t id, handle;
    uint32_t r[16], n, z, c, v, cpsr;
    int state, suspend, prio;
    uint64_t wake;
    uint32_t wait_h[64]; int wait_n, wait_all; uint32_t wait_cs;
    uint32_t stack, stack_size, exit_code, last_error, tls[64];
    int is_main;
} Thread;

static Thread *threads[64]; static int nthreads;
static Thread *running;          /* thread whose context is loaded in the CPU */
static uint32_t next_tid = 0x1000A;
static uint32_t thread_exit_trap;
static int slices_since_switch;
#define INF64 (~(uint64_t)0)
static uint64_t cyc_per_ms(void) { return cpu_hz / 1000; }
uint32_t tick_ms(void) { return (uint32_t)(vcycles / cyc_per_ms()); }

static void save_ctx(Thread *t) {
    memcpy(t->r, cpu.r, sizeof t->r); t->n = cpu.n; t->z = cpu.z; t->c = cpu.c; t->v = cpu.v; t->cpsr = cpu.cpsr;
}
static void load_ctx(Thread *t) {
    memcpy(cpu.r, t->r, sizeof cpu.r); cpu.n = t->n; cpu.z = t->z; cpu.c = t->c; cpu.v = t->v; cpu.cpsr = t->cpsr;
    cpu.halted = 0;
}

void kern_set_error(uint32_t e) { if (running) running->last_error = e; }

/* Make the interrupted API call return to its caller when this thread next runs. */
static void save_for_resume(Thread *t) {
    uint32_t lr = cpu.r[14];
    save_ctx(t);
    t->cpsr = (t->cpsr & ~0x20u) | ((lr & 1) << 5);
    t->r[15] = lr & ~1u;
}

/* signalled state of a waitable object; acquire = take it */
typedef struct { int manual, signaled; } Event;
typedef struct { uint32_t owner; int count; } Mutex;
typedef struct { int count, max; } Sem;
typedef struct { int done; uint32_t code; } ProcObj;
static ProcObj proc_obj;

static int try_acquire(Thread *t, uint32_t h) {
    Obj *o = h_obj(h);
    if (!o) return 1;   /* invalid handle: don't hang */
    switch (o->type) {
    case H_EVENT: { Event *e = o->p; if (!e->signaled) return 0; if (!e->manual) e->signaled = 0; return 1; }
    case H_MUTEX: { Mutex *m = o->p; if (m->owner && m->owner != t->id) return 0; m->owner = t->id; m->count++; return 1; }
    case H_SEM:   { Sem *s = o->p; if (s->count <= 0) return 0; s->count--; return 1; }
    case H_THREAD: return ((Thread *)o->p)->state == T_DONE;
    case H_PROCESS: return proc_obj.done;
    default: return 1;
    }
}
static int is_signaled(Thread *t, uint32_t h) {
    Obj *o = h_obj(h);
    if (!o) return 1;
    switch (o->type) {
    case H_EVENT: return ((Event *)o->p)->signaled;
    case H_MUTEX: { Mutex *m = o->p; return !m->owner || m->owner == t->id; }
    case H_SEM: return ((Sem *)o->p)->count > 0;
    case H_THREAD: return ((Thread *)o->p)->state == T_DONE;
    case H_PROCESS: return proc_obj.done;
    }
    return 1;
}

static int check_wait(Thread *t) {   /* returns 1 and sets r[0] when the wait finished */
    if (t->wait_cs) {
        if (r32(t->wait_cs + 4) == 0 || r32(t->wait_cs + 4) == t->id) {
            w32(t->wait_cs + 4, t->id); w32(t->wait_cs, r32(t->wait_cs) + 1);
            t->wait_cs = 0; return 1;
        }
        return 0;
    }
    if (t->wait_all) {
        for (int i = 0; i < t->wait_n; i++) if (!is_signaled(t, t->wait_h[i])) goto timeout;
        for (int i = 0; i < t->wait_n; i++) try_acquire(t, t->wait_h[i]);
        t->r[0] = 0; return 1;
    }
    for (int i = 0; i < t->wait_n; i++) if (try_acquire(t, t->wait_h[i])) { t->r[0] = i; return 1; }
timeout:
    if (t->wake != INF64 && vcycles >= t->wake) { t->r[0] = 0x102; return 1; }
    return 0;
}

static Thread *pick_ready(void) {
    int start = 0;
    for (int i = 0; i < nthreads; i++) if (threads[i] == running) { start = i + 1; break; }
    for (int k = 0; k < nthreads; k++) {
        Thread *t = threads[(start + k) % nthreads];
        if (t->state == T_READY && !t->suspend) return t;
    }
    return NULL;
}

/* poll sleepers / waiters / message waiters */
static void wake_check(void) {
    win_poll();
    for (int i = 0; i < nthreads; i++) {
        Thread *t = threads[i];
        if (t->state == T_WAIT) {
            if (t->wait_n == 0 && !t->wait_cs) { if (vcycles >= t->wake) { t->state = T_READY; } }   /* Sleep */
            else if (check_wait(t)) t->state = T_READY;
        } else if (t->state == T_MSG) {
            if (win_has_message_waiter() || (t->wake != INF64 && vcycles >= t->wake)) t->state = T_READY;
        }
    }
}

/* Leave the running thread (already marked non-running) and continue with another one. */
static void switch_away(void) {
    api_flow_changed = 1;
    Thread *n = pick_ready();
    if (n && n != running) { running = n; n->state = T_RUN; load_ctx(n); slices_since_switch = 0; return; }
    if (n) { n->state = T_RUN; running = n; load_ctx(n); return; }
    cpu.halted = 1;    /* nothing to run: the main loop idles until something wakes up */
}

static void block_current(void) { save_for_resume(running); switch_away(); }
static void yield_current(void) { running->state = T_READY; block_current(); }

static Thread *new_thread(uint32_t start, uint32_t param, uint32_t stack_size, int suspended) {
    Thread *t = calloc(1, sizeof *t);
    if (!t || nthreads >= 64) { free(t); return NULL; }
    t->id = next_tid; next_tid += 2;
    if (stack_size < 0x40000) stack_size = 0x40000;
    t->stack_size = stack_size; t->stack = gm_alloc(stack_size);
    if (!t->stack) { free(t); return NULL; }
    memset(t->r, 0, sizeof t->r);
    t->r[0] = param; t->r[13] = (t->stack + stack_size - 16) & ~7u; t->r[14] = thread_exit_trap;
    t->r[15] = start & ~1u;
    t->cpsr = (M_USR | 0xC0) | ((start & 1) << 5);
    t->state = T_READY; t->suspend = suspended; t->wake = INF64;
    t->handle = h_new(H_THREAD, t);
    threads[nthreads++] = t;
    return t;
}

/* ---------------------------------------------------------------- thread APIs */
static void api_CreateThread(void) {
    Thread *t = new_thread(A(2), A(3), A(1), (A(4) & 4) != 0);
    if (!t) { R(0); return; }
    if (A(5)) w32(A(5), t->id);
    R(t->handle);
}
static void thread_finish(uint32_t code) {
    Thread *t = running;
    t->exit_code = code; t->state = T_DONE;
    if (t->is_main) { hle_exit_code = (int)code; hle_running = 0; }
    switch_away();
}
static void api_ExitThread(void) { thread_finish(cpu.r[0]); }
static void api_thread_exit_trap(void) { thread_finish(cpu.r[0]); }
static void api_ExitProcess(void) { hle_exit_code = (int)cpu.r[0]; hle_running = 0; proc_obj.done = 1; api_flow_changed = 1; cpu.halted = 1; }
static void api_TerminateThread(void) {
    Thread *t = h_get(A(0), H_THREAD);
    if (!t) { R(0); return; }
    t->exit_code = A(1); t->state = T_DONE;
    if (t == running) switch_away();
    R(1);
}
static void api_GetCurrentThreadId(void) { R(running ? running->id : 0); }
static void api_GetCurrentThread(void) { R(0x41); }
static void api_GetCurrentProcess(void) { R(0x42); }
static void api_GetCurrentProcessId(void) { R(0x4000A); }
static void api_GetOwnerProcess(void) { R(0x4000A); }
static void api_SetThreadPriority(void) { R(1); }
static void api_GetThreadPriority(void) { R(3); }
static void api_GetExitCodeThread(void) {
    Thread *t = h_get(A(0), H_THREAD);
    if (!t && A(0) == 0x41) t = running;
    if (!t) { R(0); return; }
    if (A(1)) w32(A(1), t->state == T_DONE ? t->exit_code : 259);
    R(1);
}
static void api_GetExitCodeProcess(void) { if (A(1)) w32(A(1), proc_obj.done ? proc_obj.code : 259); R(1); }
static void api_SuspendThread(void) {
    Thread *t = h_get(A(0), H_THREAD); if (!t) t = running;
    R(t->suspend++);
    if (t == running) yield_current();
}
static void api_ResumeThread(void) { Thread *t = h_get(A(0), H_THREAD); if (!t) { R(0xFFFFFFFF); return; } R(t->suspend); if (t->suspend > 0) t->suspend--; }
static void api_Sleep(void) {
    uint32_t ms = A(0);
    if (!running) return;
    if (ms == 0) { slices_since_switch = 0; if (pick_ready()) yield_current(); return; }
    running->state = T_WAIT; running->wait_n = 0; running->wait_cs = 0;
    running->wake = vcycles + (uint64_t)ms * cyc_per_ms();
    block_current();
}
static void api_SleepTillTick(void) { running->state = T_WAIT; running->wait_n = 0; running->wait_cs = 0; running->wake = vcycles + cyc_per_ms(); block_current(); }

/* ---------------------------------------------------------------- sync objects */
static void api_CreateEventW(void) {
    Event *e = calloc(1, sizeof *e); e->manual = A(1) != 0; e->signaled = A(2) != 0;
    R(h_new(H_EVENT, e)); kern_set_error(0);
}
static void api_EventModify(void) {
    Event *e = h_get(A(0), H_EVENT);
    if (!e) { R(0); return; }
    switch (A(1)) { case 1: e->signaled = 1; break /* pulse: wake waiters next poll */; case 2: e->signaled = 0; break; case 3: e->signaled = 1; break; }
    if (A(1) == 1) { wake_check(); e->signaled = 0; }
    R(1);
}
static void api_SetEvent(void) { Event *e = h_get(A(0), H_EVENT); if (e) e->signaled = 1; R(e != NULL); }
static void api_ResetEvent(void) { Event *e = h_get(A(0), H_EVENT); if (e) e->signaled = 0; R(e != NULL); }
static void api_PulseEvent(void) { Event *e = h_get(A(0), H_EVENT); if (e) { e->signaled = 1; wake_check(); e->signaled = 0; } R(e != NULL); }
static void api_CreateMutexW(void) {
    Mutex *m = calloc(1, sizeof *m);
    if (A(1) && running) { m->owner = running->id; m->count = 1; }
    R(h_new(H_MUTEX, m)); kern_set_error(0);
}
static void api_ReleaseMutex(void) {
    Mutex *m = h_get(A(0), H_MUTEX);
    if (!m || m->owner != running->id) { R(0); return; }
    if (--m->count <= 0) { m->count = 0; m->owner = 0; }
    R(1);
}
static void api_CreateSemaphoreW(void) {
    Sem *s = calloc(1, sizeof *s); s->count = (int)A(1); s->max = (int)A(2);
    R(h_new(H_SEM, s)); kern_set_error(0);
}
static void api_ReleaseSemaphore(void) {
    Sem *s = h_get(A(0), H_SEM);
    if (!s) { R(0); return; }
    if (A(2)) w32(A(2), (uint32_t)s->count);
    s->count += (int)A(1); if (s->max && s->count > s->max) s->count = s->max;
    R(1);
}
static void do_wait(uint32_t n, uint32_t harr, int all, uint32_t ms) {
    Thread *t = running;
    if (n == 0 || n > 64) { R(0xFFFFFFFF); return; }
    uint32_t hs[64];
    for (uint32_t i = 0; i < n; i++) hs[i] = r32(harr + 4 * i);
    t->wait_n = (int)n; t->wait_all = all; t->wait_cs = 0;
    memcpy(t->wait_h, hs, n * 4);
    /* immediate check */
    memcpy(t->r, cpu.r, sizeof t->r);   /* check_wait writes r[0] of the thread record */
    t->wake = ms == 0xFFFFFFFFu ? INF64 : vcycles + (uint64_t)ms * cyc_per_ms();
    if (ms == 0 && !all) { for (uint32_t i = 0; i < n; i++) if (try_acquire(t, hs[i])) { R(i); t->wait_n = 0; return; } R(0x102); t->wait_n = 0; return; }
    if (check_wait(t)) { R(t->r[0]); t->wait_n = 0; return; }
    if (ms == 0) { R(0x102); t->wait_n = 0; return; }
    t->state = T_WAIT;
    block_current();
}
static void api_WaitForSingleObject(void) {
    if (A(0) == 0x41 || A(0) == 0x42) { R(0); return; }
    uint32_t tmp = gm_alloc(4); w32(tmp, A(0));
    do_wait(1, tmp, 0, A(1));
    gm_free(tmp);
}
static void api_WaitForMultipleObjects(void) { do_wait(A(0), A(1), A(2) != 0, A(3)); }
extern void fs_close_obj(int type, void *p);
static void api_CloseHandle(void) {
    uint32_t h = A(0); Obj *o = h_obj(h);
    if (o && o->type != H_THREAD && o->type != H_WINDOW) {
        if (o->type == H_EVENT || o->type == H_MUTEX || o->type == H_SEM) free(o->p);
        else fs_close_obj(o->type, o->p);
        h_close(h);
    }
    R(1);
}

/* critical sections: CRITICAL_SECTION in guest memory, [0] = recursion count, [4] = owner thread id */
static void api_InitializeCriticalSection(void) { w32(A(0), 0); w32(A(0) + 4, 0); }
static void api_DeleteCriticalSection(void) { R(0); }
static void api_EnterCriticalSection(void) {
    uint32_t cs = A(0);
    uint32_t own = r32(cs + 4);
    if (own == 0 || own == running->id) { w32(cs + 4, running->id); w32(cs, r32(cs) + 1); return; }
    running->state = T_WAIT; running->wait_n = 0; running->wait_cs = cs; running->wake = INF64;
    block_current();
}
static void api_TryEnterCriticalSection(void) {
    uint32_t cs = A(0), own = r32(cs + 4);
    if (own == 0 || own == running->id) { w32(cs + 4, running->id); w32(cs, r32(cs) + 1); R(1); } else R(0);
}
static void api_LeaveCriticalSection(void) {
    uint32_t cs = A(0), c = r32(cs);
    if (c > 0) c--;
    w32(cs, c); if (c == 0) w32(cs + 4, 0);
}
static void api_InterlockedIncrement(void) { uint32_t v = r32(A(0)) + 1; w32(A(0), v); R(v); }
static void api_InterlockedDecrement(void) { uint32_t v = r32(A(0)) - 1; w32(A(0), v); R(v); }
static void api_InterlockedExchange(void) { uint32_t o = r32(A(0)); w32(A(0), A(1)); R(o); }
static void api_InterlockedExchangeAdd(void) { uint32_t o = r32(A(0)); w32(A(0), o + A(1)); R(o); }
static void api_InterlockedCompareExchange(void) { uint32_t o = r32(A(0)); if (o == A(2)) w32(A(0), A(1)); R(o); }
static void api_InterlockedTestExchange(void) { uint32_t o = r32(A(0)); if (o == A(1)) w32(A(0), A(2)); R(o); }

/* TLS */
static uint32_t tls_used_mask_lo, tls_used_mask_hi;
static void api_TlsAlloc(void) {
    for (int i = 4; i < 64; i++) {
        uint32_t *m = i < 32 ? &tls_used_mask_lo : &tls_used_mask_hi; int b = i & 31;
        if (!(*m & (1u << b))) { *m |= 1u << b; R(i); return; }
    }
    R(0xFFFFFFFF);
}
static void api_TlsFree(void) {
    uint32_t i = A(0); if (i >= 64) { R(0); return; }
    uint32_t *m = i < 32 ? &tls_used_mask_lo : &tls_used_mask_hi; *m &= ~(1u << (i & 31));
    for (int k = 0; k < nthreads; k++) threads[k]->tls[i] = 0;
    R(1);
}
static void api_TlsGetValue(void) { uint32_t i = A(0); R(i < 64 && running ? running->tls[i] : 0); kern_set_error(0); }
static void api_TlsSetValue(void) { uint32_t i = A(0); if (i < 64 && running) running->tls[i] = A(1); R(i < 64); }
static void api_TlsCall(void) {   /* CE: TlsCall(action, slot) 0 = alloc, 1 = free */
    if (A(0) == 0) api_TlsAlloc(); else { cpu.r[0] = A(1); api_TlsFree(); }
}
static void api_GetLastError(void) { R(running ? running->last_error : 0); }
static void api_SetLastError(void) { if (running) running->last_error = A(0); }

/* ---------------------------------------------------------------- time */
static void api_GetTickCount(void) { R(tick_ms()); }
static void api_QueryPerformanceFrequency(void) { w32(A(0), 1000000); w32(A(0) + 4, 0); R(1); }
static void api_QueryPerformanceCounter(void) {
    uint64_t us = vcycles / (cpu_hz / 1000000);
    w32(A(0), (uint32_t)us); w32(A(0) + 4, (uint32_t)(us >> 32)); R(1);
}
static void api_timeGetTime(void) { R(tick_ms()); }

static void fill_systemtime(uint32_t a, struct tm *tm, int ms) {
    w16(a, tm->tm_year + 1900); w16(a + 2, tm->tm_mon + 1); w16(a + 4, tm->tm_wday); w16(a + 6, tm->tm_mday);
    w16(a + 8, tm->tm_hour); w16(a + 10, tm->tm_min); w16(a + 12, tm->tm_sec); w16(a + 14, ms);
}
static void api_GetLocalTime(void) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); struct tm tm; localtime_r(&ts.tv_sec, &tm); fill_systemtime(A(0), &tm, (int)(ts.tv_nsec / 1000000)); }
static void api_GetSystemTime(void) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); struct tm tm; gmtime_r(&ts.tv_sec, &tm); fill_systemtime(A(0), &tm, (int)(ts.tv_nsec / 1000000)); }
#define FT_EPOCH 116444736000000000ULL
static void api_GetSystemTimeAsFileTime(void) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ft = FT_EPOCH + (uint64_t)ts.tv_sec * 10000000ULL + ts.tv_nsec / 100;
    w32(A(0), (uint32_t)ft); w32(A(0) + 4, (uint32_t)(ft >> 32));
}
static void api_SystemTimeToFileTime(void) {
    struct tm tm = { 0 }; uint32_t a = A(0);
    tm.tm_year = r16(a) - 1900; tm.tm_mon = r16(a + 2) - 1; tm.tm_mday = r16(a + 6); tm.tm_hour = r16(a + 8); tm.tm_min = r16(a + 10); tm.tm_sec = r16(a + 12);
    time_t t = timegm(&tm);
    uint64_t ft = FT_EPOCH + (uint64_t)t * 10000000ULL + r16(a + 14) * 10000ULL;
    w32(A(1), (uint32_t)ft); w32(A(1) + 4, (uint32_t)(ft >> 32)); R(1);
}
static void api_FileTimeToSystemTime(void) {
    uint64_t ft = r32(A(0)) | ((uint64_t)r32(A(0) + 4) << 32);
    if (ft < FT_EPOCH) ft = FT_EPOCH;
    time_t t = (time_t)((ft - FT_EPOCH) / 10000000ULL); struct tm tm; gmtime_r(&t, &tm);
    fill_systemtime(A(1), &tm, (int)((ft / 10000) % 1000)); R(1);
}
static void api_FileTimeToLocalFileTime(void) { w32(A(1), r32(A(0))); w32(A(1) + 4, r32(A(0) + 4)); R(1); }
static void api_CompareFileTime(void) {
    uint64_t a = r32(A(0)) | ((uint64_t)r32(A(0) + 4) << 32), b = r32(A(1)) | ((uint64_t)r32(A(1) + 4) << 32);
    R(a < b ? 0xFFFFFFFFu : a > b);
}
static void api_GetTimeZoneInformation(void) { for (int i = 0; i < 43; i++) w32(A(0) + 4 * i, 0); R(0); }

/* ---------------------------------------------------------------- memory */
static void api_LocalAlloc(void) { uint32_t a = gm_alloc(A(1)); if (a && (A(0) & 0x40)) memset(gm_ptr(a), 0, A(1)); R(a); }
static void api_LocalFree(void) { gm_free(A(0)); R(0); }
static void api_LocalReAlloc(void) { R(gm_realloc(A(0), A(1))); }
static void api_LocalSize(void) { R(gm_size(A(0))); }
static void api_HeapCreate(void) { R(0x1000); }
static void api_GetProcessHeap(void) { R(0x1000); }
static void api_HeapDestroy(void) { R(1); }
static void api_HeapAlloc(void) { uint32_t a = gm_alloc(A(2)); if (a && (A(1) & 8)) memset(gm_ptr(a), 0, A(2)); R(a); }
static void api_HeapFree(void) { gm_free(A(2)); R(1); }
static void api_HeapReAlloc(void) { R(gm_realloc(A(2), A(3))); }
static void api_HeapSize(void) { R(gm_size(A(2))); }
static void api_VirtualAlloc(void) { uint32_t a = gm_valloc(A(0), A(1)); R(a); if (!a) kern_set_error(8); }
static void api_VirtualFree(void) { if (A(2) & 0x8000) gm_vfree(A(0)); R(1); }
static void api_VirtualProtect(void) { if (A(3)) w32(A(3), 4); R(1); }
static void api_VirtualQuery(void) {
    uint32_t a = A(0) & ~0xFFFu, mbi = A(1);
    int ok = gm_ptr(a) != NULL;
    w32(mbi, a); w32(mbi + 4, a); w32(mbi + 8, 4); w32(mbi + 12, 0x1000); w32(mbi + 16, ok ? 0x1000 : 0x10000); w32(mbi + 20, ok ? 4 : 1); w32(mbi + 24, ok ? 0x20000 : 0);
    R(28);
}
static void api_GlobalMemoryStatus(void) {
    uint32_t a = A(0);
    w32(a, 32); w32(a + 4, 20); w32(a + 8, 64u << 20); w32(a + 12, 40u << 20); w32(a + 16, 64u << 20); w32(a + 20, 40u << 20); w32(a + 24, 32u << 20); w32(a + 28, 30u << 20);
}
static void api_malloc(void) { R(gm_alloc(A(0))); }
static void api_calloc(void) { uint64_t n = (uint64_t)A(0) * A(1); uint32_t a = n < 0x20000000 ? gm_alloc((uint32_t)n) : 0; if (a) memset(gm_ptr(a), 0, (size_t)n); R(a); }
static void api_free(void) { gm_free(A(0)); }
static void api_realloc(void) { R(gm_realloc(A(0), A(1))); }
static void api_msize(void) { R(gm_size(A(0))); }

/* ---------------------------------------------------------------- modules */
extern void fs_guest_path(const char *host, char *out, int n);
#define FAKE_MOD 0x7F000000u
static char fake_names[16][64]; static int nfake;
static void api_GetModuleHandleW(void) {
    uint32_t n = A(0);
    if (!n) { R(main_module ? main_module->base : 0); return; }
    char nm[128]; gw_read(n, nm, sizeof nm);
    Module *m = pe_by_name(nm);
    if (m) { R(m->base); return; }
    if (!strncasecmp(nm, "coredll", 7)) { R(FAKE_MOD); return; }
    R(0);
}
static void api_GetModuleFileNameW(void) {
    Module *m = A(0) ? pe_find(A(0)) : main_module;
    char g[600];
    if (!m) { R(0); return; }
    fs_guest_path(m->path, g, sizeof g);
    gw_write(A(1), g, (int)A(2));
    R((uint32_t)strlen(g));
}
static uint32_t load_library_name(const char *nm) {
    char b[128]; snprintf(b, sizeof b, "%s", nm);
    const char *base = strrchr(b, '\\'); base = base ? base + 1 : b;
    Module *m = pe_by_name(base);
    if (m) { m->refs++; return m->base; }
    if (!strncasecmp(base, "coredll", 7) || !strncasecmp(base, "gx", 2) || !strncasecmp(base, "aygshell", 8) || !strncasecmp(base, "commctrl", 8) || !strncasecmp(base, "ceshell", 7) || !strncasecmp(base, "toolhelp", 8) || !strncasecmp(base, "mmtimer", 7) || !strncasecmp(base, "ddraw", 5)) {
        for (int i = 0; i < nfake; i++) if (!strcasecmp(fake_names[i], base)) return FAKE_MOD + i + 1;
        if (nfake < 16) { snprintf(fake_names[nfake], 64, "%s", base); return FAKE_MOD + (++nfake); }
    }
    m = pe_load_dll(base);
    return m ? m->base : 0;
}
static void api_LoadLibraryW(void) { char nm[256]; gw_read(A(0), nm, sizeof nm); uint32_t h = load_library_name(nm); R(h); if (!h) kern_set_error(126); }
static void api_LoadLibraryExW(void) { api_LoadLibraryW(); }
static void api_FreeLibrary(void) { R(1); }
static uint32_t proc_lookup(uint32_t h, const char *name, int ord) {
    if (h > FAKE_MOD && h <= FAKE_MOD + 17) return name ? api_trap_for("fake", name) : 0;
    if (h == FAKE_MOD) return name ? api_trap_for("coredll", name) : 0;
    Module *m = pe_find(h);
    return m ? pe_proc(m, name, ord) : 0;
}
static void api_GetProcAddressW(void) {
    uint32_t n = A(1);
    if (n < 0x10000) { R(proc_lookup(A(0), NULL, (int)n)); return; }
    char nm[160]; gw_read(n, nm, sizeof nm); R(proc_lookup(A(0), nm, 0));
}
static void api_GetProcAddressA(void) {
    uint32_t n = A(1);
    if (n < 0x10000) { R(proc_lookup(A(0), NULL, (int)n)); return; }
    char nm[160]; gs_read(n, nm, sizeof nm); R(proc_lookup(A(0), nm, 0));
}
static void api_DisableThreadLibraryCalls(void) { R(1); }
static uint32_t cmdline_guest;
static void api_GetCommandLineW(void) { R(cmdline_guest); }
static void api_CreateProcessW(void) {
    char app[512] = "", args[512] = "";
    if (A(0)) gw_read(A(0), app, sizeof app);
    if (A(1)) gw_read(A(1), args, sizeof args);
    const char *p = app[0] ? app : args;
    char path[512];
    if (!app[0]) {   /* first token of the command line */
        int q = args[0] == '"'; const char *s = args + q; int i = 0;
        while (s[i] && (q ? s[i] != '"' : s[i] != ' ') && i < 500) { path[i] = s[i]; i++; }
        path[i] = 0; p = path;
    }
    if (!fs_map(p, hle_exec_path, sizeof hle_exec_path, 0)) { R(0); kern_set_error(2); return; }
    snprintf(hle_exec_args, sizeof hle_exec_args, "%s", app[0] ? args : "");
    hle_exec_pending = 1;
    if (A(9)) { w32(A(9), 0x43); w32(A(9) + 4, 0x44); w32(A(9) + 8, 0x4000B); w32(A(9) + 12, 0x1000C); }
    R(1);
}
static void api_OutputDebugStringW(void) { char s[256]; gw_read(A(0), s, sizeof s); hle_log("debug: %s", s); }
static void api_GetVersionExW(void) {
    uint32_t a = A(0);
    w32(a + 4, 4); w32(a + 8, 20); w32(a + 12, 1088); w32(a + 16, 3);   /* CE 4.20, platform WIN32_CE */
    R(1);
}
static void api_GetSystemInfo(void) {
    uint32_t a = A(0);
    for (int i = 0; i < 9; i++) w32(a + 4 * i, 0);
    w16(a, 5); w32(a + 4, 4096); w32(a + 8, 0x10000); w32(a + 12, 0x7FFFFFFF); w32(a + 20, 1); w32(a + 24, 0x920); w32(a + 28, 0x10000);
}
static void api_QueryInstructionSet(void) { R(1); }
static void api_IsBadPtr(void) { R(gm_ptr(A(0)) == NULL); }
static void api_CacheRangeFlush(void) { cpu_code_changed(); }
static void api_CacheSync(void) { cpu_code_changed(); }
static void api_NoOp1(void) { R(1); }
static void api_NoOp0(void) { R(0); }
static void api_GetCurrentPermissions(void) { R(1); }
static void api_SetKMode(void) { R(1); }
static void api_KernelIoControl(void) { R(0); }
static void api_SystemParametersInfoW(void) {
    uint32_t act = A(0);
    if (act == 257) { /* SPI_GETPLATFORMTYPE */ gw_write(A(2), "Gizmondo", (int)A(1) / 2); R(1); return; }
    if (act == 258) { gw_write(A(2), "Gizmondo", (int)A(1) / 2); R(1); return; }
    R(0);
}

/* block the running thread until a window message may be available (or timeout_ms elapses; 0xFFFFFFFF = forever) */
void kern_wait_message(uint32_t timeout_ms) {
    if (!running) return;
    running->state = T_MSG;
    running->wake = timeout_ms == 0xFFFFFFFFu ? INF64 : vcycles + (uint64_t)timeout_ms * cyc_per_ms();
    save_ctx(running);          /* resume at the trap itself: the API call is re-executed when woken */
    switch_away();
}
void kern_yield(void) { if (running && pick_ready()) yield_current(); }
uint32_t kern_thread_id(void) { return running ? running->id : 0; }

/* ---------------------------------------------------------------- start / run */
static Module *kmain;
int kern_start_main(Module *m, const char *cmdline) {
    kmain = m;
    char cl[600]; snprintf(cl, sizeof cl, "%s", cmdline ? cmdline : "");
    cmdline_guest = gw_new(cl);
    for (int i = 0; i < nthreads; i++) { }
    Thread *t = new_thread(m->entry, m->base, 0x100000, 0);
    if (!t) return 0;
    t->is_main = 1;
    t->r[1] = 0; t->r[2] = cmdline_guest; t->r[3] = 1;
    cpu_reset();
    cpu_set_cpsr(M_USR | 0xC0);
    cpu.r[15] = 0;
    cpu.ctrl = 0x78 & ~1u;                 /* MMU off, identity mapped */
    t->state = T_RUN; running = t;
    load_ctx(t);
    hle_running = 1;
    return 1;
}

static void crash(const char *why) {
    hle_log("%s: pc=%08x lr=%08x sp=%08x r0=%08x r1=%08x r2=%08x r3=%08x", why, cpu.r[15], cpu.r[14], cpu.r[13], cpu.r[0], cpu.r[1], cpu.r[2], cpu.r[3]);
    hle_exit_code = -1; hle_running = 0;
}

void kern_run_frame(void) {
    int64_t left = cpu_hz / 60;
    if (!hle_running) return;
    while (left > 0 && hle_running) {
        wake_check();
        if (!running || running->state != T_RUN) {
            Thread *n = pick_ready();
            if (n) { running = n; n->state = T_RUN; load_ctx(n); slices_since_switch = 0; }
            else {
                /* idle: jump to the next wake-up */
                uint64_t next = INF64;
                for (int i = 0; i < nthreads; i++) if ((threads[i]->state == T_WAIT || threads[i]->state == T_MSG) && threads[i]->wake < next) next = threads[i]->wake;
                int64_t adv = left;
                if (next != INF64 && next > vcycles && (int64_t)(next - vcycles) < adv) adv = (int64_t)(next - vcycles);
                if (adv < 1) adv = 1;
                if (adv > 400000) adv = 400000;      /* keep polling input/timers */
                vcycles += adv; left -= adv;
                continue;
            }
        }
        uint32_t pc = cpu.r[15];
        if (pc < TRAP_BASE && !gm_ptr(pc)) { crash("jump to unmapped address"); break; }
        int slice = left < 20000 ? (int)left : 20000;
        int n = cpu_run(slice);
        if (n <= 0) n = 1;
        vcycles += n; left -= n;
        if (running && running->state == T_RUN && ++slices_since_switch >= 4) {
            slices_since_switch = 0;
            if (pick_ready()) { running->state = T_READY; save_ctx(running); switch_away(); }
        }
    }
}

void kern_init(void) {
    vcycles = 0; nthreads = 0; running = NULL; nobjs = 0; proc_obj.done = 0; nfake = 0;
    tls_used_mask_lo = tls_used_mask_hi = 0; hle_exec_pending = 0;
    thread_exit_trap = api_trap_for("kern", "<ThreadExit>");
}

#define E(n, f) { n, f }
static const ApiDef kdefs[] = {
    E("CreateThread", api_CreateThread), E("ExitThread", api_ExitThread), E("<ThreadExit>", api_thread_exit_trap),
    E("ExitProcess", api_ExitProcess), E("TerminateProcess", api_ExitProcess), E("TerminateThread", api_TerminateThread),
    E("GetCurrentThreadId", api_GetCurrentThreadId), E("GetCurrentThread", api_GetCurrentThread), E("GetCurrentProcess", api_GetCurrentProcess),
    E("GetCurrentProcessId", api_GetCurrentProcessId), E("GetOwnerProcess", api_GetOwnerProcess),
    E("SetThreadPriority", api_SetThreadPriority), E("GetThreadPriority", api_GetThreadPriority), E("CeSetThreadPriority", api_SetThreadPriority), E("CeGetThreadPriority", api_GetThreadPriority),
    E("GetExitCodeThread", api_GetExitCodeThread), E("GetExitCodeProcess", api_GetExitCodeProcess),
    E("SuspendThread", api_SuspendThread), E("ResumeThread", api_ResumeThread), E("Sleep", api_Sleep), E("SleepTillTick", api_SleepTillTick),
    E("CreateEventW", api_CreateEventW), E("EventModify", api_EventModify), E("SetEvent", api_SetEvent), E("ResetEvent", api_ResetEvent), E("PulseEvent", api_PulseEvent),
    E("CreateMutexW", api_CreateMutexW), E("ReleaseMutex", api_ReleaseMutex), E("CreateSemaphoreW", api_CreateSemaphoreW), E("ReleaseSemaphore", api_ReleaseSemaphore),
    E("WaitForSingleObject", api_WaitForSingleObject), E("WaitForMultipleObjects", api_WaitForMultipleObjects), E("CloseHandle", api_CloseHandle),
    E("InitializeCriticalSection", api_InitializeCriticalSection), E("DeleteCriticalSection", api_DeleteCriticalSection), E("EnterCriticalSection", api_EnterCriticalSection),
    E("LeaveCriticalSection", api_LeaveCriticalSection), E("TryEnterCriticalSection", api_TryEnterCriticalSection),
    E("InterlockedIncrement", api_InterlockedIncrement), E("InterlockedDecrement", api_InterlockedDecrement), E("InterlockedExchange", api_InterlockedExchange),
    E("InterlockedExchangeAdd", api_InterlockedExchangeAdd), E("InterlockedCompareExchange", api_InterlockedCompareExchange), E("InterlockedTestExchange", api_InterlockedTestExchange),
    E("TlsAlloc", api_TlsAlloc), E("TlsFree", api_TlsFree), E("TlsGetValue", api_TlsGetValue), E("TlsSetValue", api_TlsSetValue), E("TlsCall", api_TlsCall),
    E("GetLastError", api_GetLastError), E("SetLastError", api_SetLastError),
    E("GetTickCount", api_GetTickCount), E("QueryPerformanceFrequency", api_QueryPerformanceFrequency), E("QueryPerformanceCounter", api_QueryPerformanceCounter), E("timeGetTime", api_timeGetTime),
    E("GetLocalTime", api_GetLocalTime), E("GetSystemTime", api_GetSystemTime), E("GetSystemTimeAsFileTime", api_GetSystemTimeAsFileTime), E("SystemTimeToFileTime", api_SystemTimeToFileTime),
    E("FileTimeToSystemTime", api_FileTimeToSystemTime), E("FileTimeToLocalFileTime", api_FileTimeToLocalFileTime), E("LocalFileTimeToFileTime", api_FileTimeToLocalFileTime),
    E("CompareFileTime", api_CompareFileTime), E("GetTimeZoneInformation", api_GetTimeZoneInformation),
    E("LocalAlloc", api_LocalAlloc), E("LocalFree", api_LocalFree), E("LocalReAlloc", api_LocalReAlloc), E("LocalSize", api_LocalSize),
    E("HeapCreate", api_HeapCreate), E("GetProcessHeap", api_GetProcessHeap), E("HeapDestroy", api_HeapDestroy), E("HeapAlloc", api_HeapAlloc),
    E("HeapFree", api_HeapFree), E("HeapReAlloc", api_HeapReAlloc), E("HeapSize", api_HeapSize),
    E("VirtualAlloc", api_VirtualAlloc), E("VirtualFree", api_VirtualFree), E("VirtualProtect", api_VirtualProtect), E("VirtualQuery", api_VirtualQuery),
    E("GlobalMemoryStatus", api_GlobalMemoryStatus),
    E("malloc", api_malloc), E("calloc", api_calloc), E("free", api_free), E("realloc", api_realloc), E("_msize", api_msize),
    E("??2@YAPAXI@Z", api_malloc), E("??3@YAXPAX@Z", api_free), E("??_U@YAPAXI@Z", api_malloc), E("??_V@YAXPAX@Z", api_free),
    E("??2@YAPAXIABUnothrow_t@std@@@Z", api_malloc), E("??_U@YAPAXIABUnothrow_t@std@@@Z", api_malloc),
    E("??3@YAXPAXABUnothrow_t@std@@@Z", api_free), E("??_V@YAXPAXABUnothrow_t@std@@@Z", api_free),
    E("GetModuleHandleW", api_GetModuleHandleW), E("GetModuleFileNameW", api_GetModuleFileNameW), E("LoadLibraryW", api_LoadLibraryW), E("LoadLibraryExW", api_LoadLibraryExW),
    E("FreeLibrary", api_FreeLibrary), E("GetProcAddressW", api_GetProcAddressW), E("GetProcAddressA", api_GetProcAddressA), E("DisableThreadLibraryCalls", api_DisableThreadLibraryCalls),
    E("GetCommandLineW", api_GetCommandLineW), E("CreateProcessW", api_CreateProcessW), E("OutputDebugStringW", api_OutputDebugStringW),
    E("GetVersionExW", api_GetVersionExW), E("GetSystemInfo", api_GetSystemInfo), E("QueryInstructionSet", api_QueryInstructionSet),
    E("IsBadReadPtr", api_NoOp0), E("IsBadWritePtr", api_NoOp0), E("IsBadCodePtr", api_NoOp0), E("CacheRangeFlush", api_CacheRangeFlush), E("CacheSync", api_CacheSync),
    E("GetCurrentPermissions", api_GetCurrentPermissions), E("SetKMode", api_SetKMode), E("KernelIoControl", api_KernelIoControl),
    E("SystemParametersInfoW", api_SystemParametersInfoW), E("SetProcPermissions", api_NoOp1), E("DebugBreak", api_NoOp0),
};
void api_kernel_init(void) { api_register(kdefs, sizeof kdefs / sizeof kdefs[0]); }
