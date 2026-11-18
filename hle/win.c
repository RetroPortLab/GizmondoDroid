/* GizmondoDroid - windowing: classes, windows, message queue, timers, input, misc user32-ish APIs */
#include "hle.h"
#include "gdi.h"
#include <stdio.h>
#include <stdlib.h>

#define WM_CREATE 1
#define WM_DESTROY 2
#define WM_SIZE 5
#define WM_ACTIVATE 6
#define WM_SETFOCUS 7
#define WM_KILLFOCUS 8
#define WM_PAINT 0xF
#define WM_CLOSE 0x10
#define WM_QUIT 0x12
#define WM_ERASEBKGND 0x14
#define WM_SHOWWINDOW 0x18
#define WM_SETTEXT 0xC
#define WM_GETTEXT 0xD
#define WM_KEYDOWN 0x100
#define WM_KEYUP 0x101
#define WM_TIMER 0x113
#define WM_MOUSEMOVE 0x200
#define WM_LBUTTONDOWN 0x201
#define WM_LBUTTONUP 0x202
#define CW_USEDEFAULT 0x80000000u

typedef struct Class { char name[64]; uint32_t proc, bg, style, extra_cls, extra_wnd, inst; } Class;
typedef struct Wnd {
    uint32_t h, proc, parent, style, exstyle, id, inst, userdata;
    int x, y, w, ht, visible, invalid, enabled, activated;
    char title[128], cls[64]; uint32_t bg, extra[16]; uint32_t tid;
} Wnd;
typedef struct { uint32_t hwnd, msg, wp, lp, time; int x, y; } Msg;
typedef struct { uint32_t hwnd, id, proc; uint64_t interval, next; int active; } Timer;

static Class classes[32]; static int nclasses;
static Msg queue[512]; static int qh, qt, qn;
static Timer timers[32];
static uint32_t focus_wnd, active_wnd, capture_wnd, desktop_wnd;
static int quit_posted, quit_code;
uint8_t key_state[256];
static uint32_t next_timer_id = 100, next_user_msg = 0xC000;
static int cursor_x, cursor_y;
static Wnd *wnds[128]; static int nwnds;

static Wnd *wnd_get(uint32_t h) { return h_get(h, H_WINDOW); }
static void abs_origin(Wnd *w, int *ox, int *oy) {
    int x = 0, y = 0;
    for (Wnd *p = w; p; p = p->parent ? wnd_get(p->parent) : NULL) { x += p->x; y += p->y; if (!p->parent) break; }
    *ox = x; *oy = y;
}
static void post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    if (qn >= 512) return;
    Msg *m = &queue[qt]; m->hwnd = hwnd; m->msg = msg; m->wp = wp; m->lp = lp; m->time = tick_ms(); m->x = cursor_x; m->y = cursor_y;
    qt = (qt + 1) % 512; qn++;
}
void win_post_message(uint32_t h, uint32_t m, uint32_t wp, uint32_t lp) { post(h, m, wp, lp); }
static uint32_t send_msg(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);

/* ------------------------------------------------------------ timers & pending message detection */
static void gen_timers(void) {
    for (int i = 0; i < 32; i++) {
        Timer *t = &timers[i];
        if (!t->active || vcycles < t->next) continue;
        int dup = 0;
        for (int k = 0, j = qh; k < qn; k++, j = (j + 1) % 512) if (queue[j].msg == WM_TIMER && queue[j].hwnd == t->hwnd && queue[j].wp == t->id) { dup = 1; break; }
        t->next = vcycles + t->interval;
        if (!dup) post(t->hwnd, WM_TIMER, t->id, t->proc);
    }
}
static Wnd *paint_pending(void) {
    for (int i = 0; i < nwnds; i++) if (wnds[i] && wnds[i]->invalid && wnds[i]->visible) return wnds[i];
    return NULL;
}
void win_poll(void) { gen_timers(); }
int win_has_message_waiter(void) {
    gen_timers();
    return qn > 0 || quit_posted || paint_pending() != NULL;
}
static uint32_t next_timer_ms(void) {
    uint64_t best = ~(uint64_t)0;
    for (int i = 0; i < 32; i++) if (timers[i].active && timers[i].next < best) best = timers[i].next;
    if (best == ~(uint64_t)0) return 0xFFFFFFFFu;
    if (best <= vcycles) return 0;
    return (uint32_t)((best - vcycles) / (cpu_hz / 1000)) + 1;
}

/* fetch the next message (no blocking); returns 1 and fills *out */
static int get_msg(Msg *out, uint32_t hwnd, uint32_t mn, uint32_t mx, int remove) {
    gen_timers();
    for (int k = 0, j = qh; k < qn; k++, j = (j + 1) % 512) {
        Msg *m = &queue[j];
        if (hwnd && m->hwnd != hwnd) continue;
        if (mn || mx) if (m->msg < mn || m->msg > mx) continue;
        *out = *m;
        if (remove) { /* delete entry j by shifting */
            for (int a = j; a != qt; ) { int nx = (a + 1) % 512; if (nx == qt) break; queue[a] = queue[nx]; a = nx; }
            qt = (qt + 511) % 512; qn--;
        }
        return 1;
    }
    if (quit_posted && !(mn || mx) ) { memset(out, 0, sizeof *out); out->msg = WM_QUIT; out->wp = (uint32_t)quit_code; if (remove) quit_posted = 0; return 1; }
    if (!(mn || mx) || (mn <= WM_PAINT && mx >= WM_PAINT)) {
        Wnd *w = paint_pending();
        if (w && (!hwnd || hwnd == w->h)) { memset(out, 0, sizeof *out); out->hwnd = w->h; out->msg = WM_PAINT; out->time = tick_ms(); return 1; }
    }
    return 0;
}
static void put_msg(uint32_t p, Msg *m) {
    w32(p, m->hwnd); w32(p + 4, m->msg); w32(p + 8, m->wp); w32(p + 12, m->lp); w32(p + 16, m->time); w32(p + 20, (uint32_t)m->x); w32(p + 24, (uint32_t)m->y);
}
static void api_GetMessageW(void) {
    Msg m;
    if (get_msg(&m, A(1), A(2), A(3), 1)) { put_msg(A(0), &m); R(m.msg != WM_QUIT); return; }
    uint32_t ms = next_timer_ms();
    kern_wait_message(ms);          /* re-executes this call when woken */
}
static void api_PeekMessageW(void) {
    Msg m;
    if (get_msg(&m, A(1), A(2), A(3), (A(4) & 1) != 0)) { put_msg(A(0), &m); R(1); return; }
    R(0);
    kern_yield();
}
static void api_PostMessageW(void) { if (A(0) && !wnd_get(A(0))) { R(0); return; } post(A(0), A(1), A(2), A(3)); R(1); }
static void api_PostThreadMessageW(void) { post(0, A(1), A(2), A(3)); R(1); }
static void api_PostQuitMessage(void) { quit_posted = 1; quit_code = (int)A(0); }
static uint32_t def_proc(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
static uint32_t send_msg(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    Wnd *w = wnd_get(hwnd); if (!w) return 0;
    if (!w->proc) return def_proc(hwnd, msg, wp, lp);
    return guest_call(w->proc, hwnd, msg, wp, lp);
}
static void api_SendMessageW(void) { R(send_msg(A(0), A(1), A(2), A(3))); }
static void api_DispatchMessageW(void) {
    uint32_t p = A(0), hwnd = r32(p), msg = r32(p + 4), wp = r32(p + 8), lp = r32(p + 12);
    if (msg == WM_TIMER && lp) { R(guest_call(lp, hwnd, WM_TIMER, wp, tick_ms())); return; }
    if (!hwnd) { R(0); return; }
    R(send_msg(hwnd, msg, wp, lp));
}
static void api_TranslateMessage(void) { R(0); }
static void api_NoOp0(void) { R(0); }
static void api_NoOp1(void) { R(1); }

static void api_SetTimer(void) {
    uint32_t hwnd = A(0), id = A(1), ms = A(2), proc = A(3);
    Timer *t = NULL;
    for (int i = 0; i < 32; i++) if (timers[i].active && timers[i].hwnd == hwnd && hwnd && timers[i].id == id) t = &timers[i];
    for (int i = 0; i < 32 && !t; i++) if (!timers[i].active) t = &timers[i];
    if (!t) { R(0); return; }
    if (!hwnd) id = next_timer_id++;
    t->active = 1; t->hwnd = hwnd; t->id = id; t->proc = proc; t->interval = (uint64_t)(ms ? ms : 1) * (cpu_hz / 1000); t->next = vcycles + t->interval;
    R(id);
}
static void api_KillTimer(void) { for (int i = 0; i < 32; i++) if (timers[i].active && timers[i].hwnd == A(0) && timers[i].id == A(1)) { timers[i].active = 0; R(1); return; } R(0); }

/* ------------------------------------------------------------ classes and windows */
static Class *find_class(const char *name) { for (int i = 0; i < nclasses; i++) if (!strcasecmp(classes[i].name, name)) return &classes[i]; return NULL; }
static void api_RegisterClassW(void) {
    uint32_t p = A(0); char nm[64]; uint32_t np = r32(p + 36);
    if (np < 0x10000) snprintf(nm, sizeof nm, "#%u", np); else gw_read(np, nm, sizeof nm);
    Class *c = find_class(nm); if (!c) { if (nclasses >= 32) { R(0); return; } c = &classes[nclasses++]; }
    memset(c, 0, sizeof *c); snprintf(c->name, sizeof c->name, "%s", nm);
    c->style = r32(p); c->proc = r32(p + 4); c->extra_cls = r32(p + 8); c->extra_wnd = r32(p + 12); c->inst = r32(p + 16); c->bg = r32(p + 28);
    R(0xC000 + (uint32_t)(c - classes));
}
static void api_UnregisterClassW(void) { R(1); }
static void api_GetClassInfoW(void) { R(0); }

static void api_CreateWindowExW(void) {
    uint32_t ex = A(0), style = A(3), clsp = A(1);
    char cls[64];
    if (clsp < 0x10000) { snprintf(cls, sizeof cls, "#%u", clsp - 0xC000); if (clsp >= 0xC000 && clsp - 0xC000 < (uint32_t)nclasses) snprintf(cls, sizeof cls, "%s", classes[clsp - 0xC000].name); }
    else gw_read(clsp, cls, sizeof cls);
    Class *c = find_class(cls);
    Wnd *w = calloc(1, sizeof *w);
    snprintf(w->cls, sizeof w->cls, "%s", cls);
    if (A(2)) gw_read(A(2), w->title, sizeof w->title);
    w->style = style; w->exstyle = ex; w->parent = A(8); w->id = A(9); w->inst = A(10); w->enabled = 1; w->tid = kern_thread_id();
    int x = (int)A(4), y = (int)A(5), cw = (int)A(6), ch = (int)A(7);
    if ((uint32_t)x == CW_USEDEFAULT) x = 0; if ((uint32_t)y == CW_USEDEFAULT) y = 0;
    if ((uint32_t)cw == CW_USEDEFAULT) cw = w->parent ? 100 : SCREEN_W; if ((uint32_t)ch == CW_USEDEFAULT) ch = w->parent ? 100 : SCREEN_H;
    w->x = x; w->y = y; w->w = cw; w->ht = ch;
    if (c) { w->proc = c->proc; w->bg = c->bg; }
    w->visible = (style & 0x10000000) != 0;   /* WS_VISIBLE */
    w->invalid = w->visible;
    w->h = h_new(H_WINDOW, w);
    wnds[nwnds++] = w;
    uint32_t cs = gm_alloc(48);
    w32(cs, A(11)); w32(cs + 4, A(10)); w32(cs + 8, A(9)); w32(cs + 12, A(8)); w32(cs + 16, (uint32_t)ch); w32(cs + 20, (uint32_t)cw); w32(cs + 24, (uint32_t)y); w32(cs + 28, (uint32_t)x);
    w32(cs + 32, style); w32(cs + 36, A(2)); w32(cs + 40, A(1)); w32(cs + 44, ex);
    uint32_t r = send_msg(w->h, WM_CREATE, 0, cs);
    gm_free(cs);
    if (r == 0xFFFFFFFFu) { w->visible = 0; R(0); return; }
    send_msg(w->h, WM_SIZE, 0, (uint32_t)(cw | (ch << 16)));
    if (!w->parent && !focus_wnd) { focus_wnd = active_wnd = w->h; }
    R(w->h);
}
static void activate(Wnd *w) {
    if (w->activated || w->parent) return;
    w->activated = 1; active_wnd = focus_wnd = w->h;
    send_msg(w->h, WM_ACTIVATE, 1, 0); send_msg(w->h, WM_SETFOCUS, 0, 0);
}
static void api_ShowWindow(void) {
    Wnd *w = wnd_get(A(0)); if (!w) { R(0); return; }
    int was = w->visible; w->visible = A(1) != 0;
    if (w->visible) { w->invalid = 1; send_msg(w->h, WM_SHOWWINDOW, 1, 0); activate(w); }
    R(was);
}
static void api_UpdateWindow(void) {
    Wnd *w = wnd_get(A(0)); if (!w) { R(0); return; }
    if (w->invalid && w->visible) send_msg(w->h, WM_PAINT, 0, 0);
    R(1);
}
static void api_DestroyWindow(void) {
    Wnd *w = wnd_get(A(0)); if (!w) { R(0); return; }
    send_msg(w->h, 0x2, 0, 0);
    w->visible = 0; w->invalid = 0;
    for (int i = 0; i < nwnds; i++) if (wnds[i] == w) { wnds[i] = wnds[--nwnds]; break; }
    if (focus_wnd == w->h) focus_wnd = 0; if (active_wnd == w->h) active_wnd = 0;
    h_close(w->h); free(w); R(1);
}
static uint32_t def_proc(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    Wnd *w = wnd_get(hwnd); if (!w) return 0;
    switch (msg) {
    case WM_CLOSE: send_msg(hwnd, 2, 0, 0); { cpu.r[0] = hwnd; api_DestroyWindow(); } return 0;
    case WM_ERASEBKGND: {
        DC *d = dc_get(wp); if (d) { uint32_t br = w->bg; if (br < 32) br = gdi_syscolor_brush(br ? br - 1 : 5); dc_fill_brush(d, br, 0, 0, w->w, w->ht); } return 1; }
    case WM_PAINT: w->invalid = 0; return 0;
    case WM_SETTEXT: gw_read(lp, w->title, sizeof w->title); return 1;
    case WM_GETTEXT: gw_write(lp, w->title, (int)wp); return (uint32_t)strlen(w->title);
    case 0x1F: case 0x30: return 0;
    case 0x7: case 0x6: return 0;
    case 0x20: return 1;        /* WM_SETCURSOR */
    }
    return 0;
}
static void api_DefWindowProcW(void) { R(def_proc(A(0), A(1), A(2), A(3))); }
static void api_GetClientRect(void) { Wnd *w = wnd_get(A(0)); if (!w) { R(0); return; } w32(A(1), 0); w32(A(1) + 4, 0); w32(A(1) + 8, (uint32_t)w->w); w32(A(1) + 12, (uint32_t)w->ht); R(1); }
static void api_GetWindowRect(void) { Wnd *w = wnd_get(A(0)); if (!w) { R(0); return; } int ox, oy; abs_origin(w, &ox, &oy); w32(A(1), (uint32_t)ox); w32(A(1) + 4, (uint32_t)oy); w32(A(1) + 8, (uint32_t)(ox + w->w)); w32(A(1) + 12, (uint32_t)(oy + w->ht)); R(1); }
static void api_MoveWindow(void) { Wnd *w = wnd_get(A(0)); if (!w) { R(0); return; } w->x = (int)A(1); w->y = (int)A(2); w->w = (int)A(3); w->ht = (int)A(4); w->invalid = 1; send_msg(w->h, WM_SIZE, 0, (uint32_t)(w->w | (w->ht << 16))); R(1); }
static void api_SetWindowPos(void) {
    Wnd *w = wnd_get(A(0)); if (!w) { R(0); return; }
    if (!(A(6) & 2)) { w->x = (int)A(2); w->y = (int)A(3); }
    if (!(A(6) & 1)) { w->w = (int)A(4); w->ht = (int)A(5); }
    if (A(6) & 0x40) w->visible = 1; if (A(6) & 0x80) w->visible = 0;
    w->invalid = 1; R(1);
}
static void api_InvalidateRect(void) {
    if (!A(0)) { for (int i = 0; i < nwnds; i++) wnds[i]->invalid = 1; R(1); return; }
    Wnd *w = wnd_get(A(0)); if (w) w->invalid = 1; R(w != NULL);
}
static void api_ValidateRect(void) { Wnd *w = wnd_get(A(0)); if (w) w->invalid = 0; R(1); }
static void api_GetUpdateRect(void) { Wnd *w = wnd_get(A(0)); if (A(1) && w) { w32(A(1), 0); w32(A(1) + 4, 0); w32(A(1) + 8, (uint32_t)w->w); w32(A(1) + 12, (uint32_t)w->ht); } R(w && w->invalid); }
static uint32_t window_dc(Wnd *w) {
    int ox = 0, oy = 0; if (w) abs_origin(w, &ox, &oy);
    int cl = ox, ct = oy, cr = w ? ox + w->w : SCREEN_W, cb = w ? oy + w->ht : SCREEN_H;
    uint32_t h = gdi_screen_dc(ox, oy, cl, ct, cr, cb); DC *d = dc_get(h); if (d) d->hwnd = w ? w->h : 0; return h;
}
static void api_GetDC(void) { R(window_dc(A(0) ? wnd_get(A(0)) : NULL)); }
static void api_ReleaseDC(void) { DC *d = dc_get(A(1)); if (d) { free(d); h_close(A(1)); } R(1); }
static void api_BeginPaint(void) {
    Wnd *w = wnd_get(A(0)); uint32_t ps = A(1); if (!w) { R(0); return; }
    uint32_t hdc = window_dc(w); w->invalid = 0;
    w32(ps, hdc); w32(ps + 4, 0); w32(ps + 8, 0); w32(ps + 12, 0); w32(ps + 16, (uint32_t)w->w); w32(ps + 20, (uint32_t)w->ht);
    R(hdc);
}
static void api_EndPaint(void) { DC *d = dc_get(r32(A(1))); if (d) { uint32_t h = r32(A(1)); free(d); h_close(h); } R(1); }
static void api_SetFocus(void) { uint32_t o = focus_wnd; focus_wnd = A(0); R(o); }
static void api_GetFocus(void) { R(focus_wnd); }
static void api_SetForegroundWindow(void) { Wnd *w = wnd_get(A(0)); if (w) activate(w); active_wnd = A(0); R(1); }
static void api_GetForegroundWindow(void) { R(active_wnd); }
static void api_SetActiveWindow(void) { uint32_t o = active_wnd; Wnd *w = wnd_get(A(0)); if (w) activate(w); active_wnd = A(0); R(o); }
static void api_GetActiveWindow(void) { R(active_wnd); }
static void api_SetCapture(void) { uint32_t o = capture_wnd; capture_wnd = A(0); R(o); }
static void api_ReleaseCapture(void) { capture_wnd = 0; R(1); }
static void api_GetParent(void) { Wnd *w = wnd_get(A(0)); R(w ? w->parent : 0); }
static void api_IsWindow(void) { R(wnd_get(A(0)) != NULL); }
static void api_IsWindowVisible(void) { Wnd *w = wnd_get(A(0)); R(w && w->visible); }
static void api_EnableWindow(void) { Wnd *w = wnd_get(A(0)); if (w) { w->enabled = A(1) != 0; } R(0); }
static void api_IsWindowEnabled(void) { Wnd *w = wnd_get(A(0)); R(w && w->enabled); }
static void api_SetWindowTextW(void) { Wnd *w = wnd_get(A(0)); if (w) gw_read(A(1), w->title, sizeof w->title); R(1); }
static void api_GetWindowTextW(void) { Wnd *w = wnd_get(A(0)); if (!w) { R(0); return; } gw_write(A(1), w->title, (int)A(2)); R((uint32_t)strlen(w->title)); }
static void api_FindWindowW(void) {
    char c[64] = "", t[128] = ""; if (A(0)) gw_read(A(0), c, sizeof c); if (A(1)) gw_read(A(1), t, sizeof t);
    for (int i = 0; i < nwnds; i++) if ((!c[0] || !strcasecmp(wnds[i]->cls, c)) && (!t[0] || !strcasecmp(wnds[i]->title, t))) { R(wnds[i]->h); return; }
    R(0);
}
static void api_ClientToScreen(void) { Wnd *w = wnd_get(A(0)); int ox = 0, oy = 0; if (w) abs_origin(w, &ox, &oy); w32(A(1), r32(A(1)) + (uint32_t)ox); w32(A(1) + 4, r32(A(1) + 4) + (uint32_t)oy); R(1); }
static void api_ScreenToClient(void) { Wnd *w = wnd_get(A(0)); int ox = 0, oy = 0; if (w) abs_origin(w, &ox, &oy); w32(A(1), r32(A(1)) - (uint32_t)ox); w32(A(1) + 4, r32(A(1) + 4) - (uint32_t)oy); R(1); }
static void api_GetCursorPos(void) { w32(A(0), (uint32_t)cursor_x); w32(A(0) + 4, (uint32_t)cursor_y); R(1); }
static void api_GetDesktopWindow(void) { R(desktop_wnd); }
static void api_GetWindowLongW(void) {
    Wnd *w = wnd_get(A(0)); int i = (int)A(1); if (!w) { R(0); return; }
    switch (i) { case -4: R(w->proc); break; case -21: R(w->userdata); break; case -16: R(w->style); break; case -20: R(w->exstyle); break; case -12: R(w->id); break; case -6: R(w->inst); break; default: R(i >= 0 && i < 64 ? w->extra[i / 4] : 0); }
}
static void api_SetWindowLongW(void) {
    Wnd *w = wnd_get(A(0)); int i = (int)A(1); uint32_t v = A(2); if (!w) { R(0); return; }
    switch (i) { case -4: R(w->proc); w->proc = v; break; case -21: R(w->userdata); w->userdata = v; break; case -16: R(w->style); w->style = v; break; case -20: R(w->exstyle); w->exstyle = v; break;
    case -12: R(w->id); w->id = v; break; default: if (i >= 0 && i < 64) { R(w->extra[i / 4]); w->extra[i / 4] = v; } else R(0); }
}
static void api_CallWindowProcW(void) { R(guest_call(A(0), A(1), A(2), A(3), A(4))); }
static void api_MessageBoxW(void) {
    char t[256] = "", c[128] = ""; if (A(1)) gw_read(A(1), t, sizeof t); if (A(2)) gw_read(A(2), c, sizeof c);
    hle_log("MessageBox [%s]: %s", c, t); R(1);
}
static void api_GetSystemMetrics(void) {
    switch (A(0)) { case 0: case 16: R(SCREEN_W); break; case 1: case 17: R(SCREEN_H); break; case 2: R(13); break; case 3: R(13); break; case 4: R(0); break; case 5: case 6: R(1); break;
    case 11: case 12: R(32); break; case 32: case 33: R(1); break; case 43: R(1); break; case 45: case 46: R(0); break; case 49: R(16); break; default: R(0); }
}
static void api_GetAsyncKeyState(void) { uint32_t vk = A(0) & 255; R(key_state[vk] ? 0x8000u : 0); }
static void api_GetKeyboardState(void) { for (int i = 0; i < 256; i++) w8(A(0) + (uint32_t)i, key_state[i] ? 0x80 : 0); R(1); }
static void api_RegisterWindowMessageW(void) { R(next_user_msg++); }
static void api_GetWindowThreadProcessId(void) { Wnd *w = wnd_get(A(0)); if (A(1)) w32(A(1), 0x4000A); R(w ? w->tid : 0); }
static void api_AdjustWindowRectEx(void) { R(1); }
static void api_LoadCursorW(void) { R(h_new(H_MISC, calloc(1, 8))); }
static void api_ShowCursor(void) { R(A(0) ? 1 : 0xFFFFFFFFu); }
static void api_SHFullScreen(void) { R(1); }
static void api_SetScrollInfo(void) { R(0); }

/* ------------------------------------------------------------ host input */
void input_key(int vk, int down) {
    vk &= 255;
    if (key_state[vk] == (uint8_t)(down != 0)) return;
    key_state[vk] = (uint8_t)(down != 0);
    uint32_t target = focus_wnd ? focus_wnd : active_wnd;
    if (target) post(target, down ? WM_KEYDOWN : WM_KEYUP, (uint32_t)vk, down ? 1u : 0xC0000001u);
}
void input_touch(int x, int y, int down) {
    static int was;
    cursor_x = x; cursor_y = y;
    uint32_t target = capture_wnd ? capture_wnd : focus_wnd ? focus_wnd : active_wnd;
    if (!target) return;
    uint32_t lp = (uint32_t)((y & 0xFFFF) << 16) | (uint32_t)(x & 0xFFFF);
    if (down && !was) post(target, WM_LBUTTONDOWN, 1, lp);
    else if (down) post(target, WM_MOUSEMOVE, 1, lp);
    else if (was) post(target, WM_LBUTTONUP, 0, lp);
    was = down;
}

#define E(n, f) { n, f }
static const ApiDef wdefs[] = {
    E("GetMessageW", api_GetMessageW), E("PeekMessageW", api_PeekMessageW), E("PostMessageW", api_PostMessageW), E("PostThreadMessageW", api_PostThreadMessageW),
    E("PostQuitMessage", api_PostQuitMessage), E("SendMessageW", api_SendMessageW), E("DispatchMessageW", api_DispatchMessageW), E("TranslateMessage", api_TranslateMessage),
    E("SetTimer", api_SetTimer), E("KillTimer", api_KillTimer), E("RegisterClassW", api_RegisterClassW), E("UnregisterClassW", api_UnregisterClassW), E("GetClassInfoW", api_GetClassInfoW),
    E("CreateWindowExW", api_CreateWindowExW), E("ShowWindow", api_ShowWindow), E("UpdateWindow", api_UpdateWindow), E("DestroyWindow", api_DestroyWindow), E("DefWindowProcW", api_DefWindowProcW),
    E("GetClientRect", api_GetClientRect), E("GetWindowRect", api_GetWindowRect), E("MoveWindow", api_MoveWindow), E("SetWindowPos", api_SetWindowPos),
    E("InvalidateRect", api_InvalidateRect), E("ValidateRect", api_ValidateRect), E("GetUpdateRect", api_GetUpdateRect), E("GetDC", api_GetDC), E("GetWindowDC", api_GetDC), E("ReleaseDC", api_ReleaseDC),
    E("BeginPaint", api_BeginPaint), E("EndPaint", api_EndPaint), E("SetFocus", api_SetFocus), E("GetFocus", api_GetFocus), E("SetForegroundWindow", api_SetForegroundWindow),
    E("GetForegroundWindow", api_GetForegroundWindow), E("SetActiveWindow", api_SetActiveWindow), E("GetActiveWindow", api_GetActiveWindow), E("SetCapture", api_SetCapture), E("ReleaseCapture", api_ReleaseCapture),
    E("GetParent", api_GetParent), E("IsWindow", api_IsWindow), E("IsWindowVisible", api_IsWindowVisible), E("EnableWindow", api_EnableWindow), E("IsWindowEnabled", api_IsWindowEnabled),
    E("SetWindowTextW", api_SetWindowTextW), E("GetWindowTextW", api_GetWindowTextW), E("FindWindowW", api_FindWindowW), E("ClientToScreen", api_ClientToScreen), E("ScreenToClient", api_ScreenToClient),
    E("GetCursorPos", api_GetCursorPos), E("GetDesktopWindow", api_GetDesktopWindow), E("GetWindowLongW", api_GetWindowLongW), E("SetWindowLongW", api_SetWindowLongW), E("CallWindowProcW", api_CallWindowProcW),
    E("MessageBoxW", api_MessageBoxW), E("GetSystemMetrics", api_GetSystemMetrics), E("GetAsyncKeyState", api_GetAsyncKeyState), E("GetKeyState", api_GetAsyncKeyState), E("GetKeyboardState", api_GetKeyboardState),
    E("RegisterWindowMessageW", api_RegisterWindowMessageW), E("GetWindowThreadProcessId", api_GetWindowThreadProcessId), E("AdjustWindowRectEx", api_AdjustWindowRectEx),
    E("LoadCursorW", api_LoadCursorW), E("ShowCursor", api_ShowCursor), E("SetCursor", api_NoOp0), E("SHFullScreen", api_SHFullScreen), E("SetScrollInfo", api_SetScrollInfo),
    E("SHSipPreference", api_NoOp1), E("SipShowIM", api_NoOp1), E("SetCursorPos", api_NoOp1), E("keybd_event", api_NoOp0), E("mouse_event", api_NoOp0), E("SetWindowsHookExW", api_NoOp0),
    E("UnhookWindowsHookEx", api_NoOp1), E("MessageBeep", api_NoOp1), E("SetSysColors", api_NoOp1), E("SystemIdleTimerReset", api_NoOp0), E("GetMenu", api_NoOp0), E("SetMenu", api_NoOp1),
    E("IsDialogMessageW", api_NoOp0), E("SetWindowRgn", api_NoOp1), E("BringWindowToTop", api_NoOp1), E("EnumWindows", api_NoOp1), E("CloseWindow", api_NoOp1), E("GetLastActivePopup", api_NoOp0),
};
void win_init_api(void) {
    memset(timers, 0, sizeof timers); nclasses = 0; qh = qt = qn = 0; nwnds = 0; focus_wnd = active_wnd = capture_wnd = 0; quit_posted = 0; memset(key_state, 0, sizeof key_state);
    api_register(wdefs, sizeof wdefs / sizeof wdefs[0]);
    Wnd *d = calloc(1, sizeof *d); d->w = SCREEN_W; d->ht = SCREEN_H; d->visible = 1; snprintf(d->cls, sizeof d->cls, "Desktop"); desktop_wnd = h_new(H_WINDOW, d);
}
