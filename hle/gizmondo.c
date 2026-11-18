/* GizmondoDroid - glue: brings up memory, API tables, loads a program and runs it frame by frame */
#include "hle.h"
#include "gizmondo.h"
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <strings.h>

extern void (*hle_log_sink)(const char *line);
static char g_sd[512], g_sys[512];
static uint32_t frames_run;

void giz_set_log(void (*sink)(const char *)) { hle_log_sink = sink; }
void giz_set_audio_rate(uint32_t hz) { audio_rate = hz ? hz : 44100; }

static void alias_load(const char *exe);
int giz_launch(const char *exe, const char *sd, const char *sys, const char *args, char *err, int errlen) {
    char dummy[8]; if (!err) { err = dummy; errlen = sizeof dummy; }
    err[0] = 0;
    char rp[PATH_MAX], exe_abs[PATH_MAX];
    if (sd && realpath(sd, rp)) snprintf(g_sd, sizeof g_sd, "%s", rp); else snprintf(g_sd, sizeof g_sd, "%s", sd ? sd : ".");
    snprintf(g_sys, sizeof g_sys, "%s", sys ? sys : "./_sys");
    if (g_sys[0] != '/' && getcwd(rp, sizeof rp)) { char t[PATH_MAX + 600]; snprintf(t, sizeof t, "%s/%s", rp, g_sys); snprintf(g_sys, sizeof g_sys, "%s", t); }
    if (realpath(exe, exe_abs)) exe = exe_abs;
    hle_running = 0;
    gm_init();
    alias_load(exe);
    api_reset(); pe_reset();
    cpu_reset();
    api_kernel_init();
    kern_init();
    api_crt_init(); fs_init_api(); win_init_api(); gdi_init_api(); gx_init_api(); gles_init_api(); audio_init_api(); api_misc_init(); api_user_init();
    fs_set_roots(g_sd, g_sys);
    snprintf(fs_cwd, sizeof fs_cwd, "\\");
    Module *m = pe_load_exe(exe, err, errlen);
    if (!m) { if (!err[0]) snprintf(err, (size_t)errlen, "could not load %s", exe); return -1; }
    char cl[600]; snprintf(cl, sizeof cl, "%s", args ? args : "");
    if (!kern_start_main(m, cl)) { snprintf(err, (size_t)errlen, "could not start main thread"); return -1; }
    frames_run = 0;
    return 0;
}

void giz_frame(void) {
    if (hle_exec_pending) {
        char p[512], a[512]; snprintf(p, sizeof p, "%s", hle_exec_path); snprintf(a, sizeof a, "%s", hle_exec_args);
        char sd[512], sys[512]; snprintf(sd, sizeof sd, "%s", g_sd); snprintf(sys, sizeof sys, "%s", g_sys);
        char err[256];
        if (giz_launch(p, sd, sys, a, err, sizeof err)) hle_log("launch failed: %s", err);
        return;
    }
    if (!hle_running) return;
    kern_run_frame();
    audio_frame((int)(audio_rate / 60));
    frames_run++;
}
const uint16_t *giz_screen(void) { return fb_host(); }
int giz_running(void) { return hle_running || hle_exec_pending; }
int giz_exit_code(void) { return hle_exit_code; }
/* Key aliases: some games ignore the GAPI buttons and listen for ordinary keys instead. A profile can map each
 * Gizmondo button to up to two virtual keys. Built-in profiles exist for known games; a file named
 * gizmondodroid-keys.txt in the game's folder (lines like "A=0x0D,0x11") overrides them. */
static int g_alias[256][2];
static int key_vk_by_name(const char *n) {
    static const struct { const char *n; int vk; } t[] = { {"UP",0x26},{"DOWN",0x28},{"LEFT",0x25},{"RIGHT",0x27},{"A",0xC1},{"B",0xC2},{"C",0xC3},{"START",0x0D} };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcasecmp(n, t[i].n)) return t[i].vk;
    return -1;
}
static void alias_reset(void) { for (int i = 0; i < 256; i++) { g_alias[i][0] = i; g_alias[i][1] = 0; } }
static void alias_set(int from, int a, int b) { if (from >= 0 && from < 256) { g_alias[from][0] = a; g_alias[from][1] = b; } }
static void alias_load(const char *exe) {
    alias_reset();
    const char *base = strrchr(exe, '/'); base = base ? base + 1 : exe;
    if (!strcasecmp(base, "carmageddon.exe")) { alias_set(0xC1, 0x0D, 0); alias_set(0xC2, 0x20, 0); alias_set(0xC3, 0x1B, 0); }
    char path[PATH_MAX + 64]; snprintf(path, sizeof path, "%s/gizmondodroid-keys.txt", g_sd);
    FILE *f = fopen(path, "r"); if (!f) return;
    char line[160];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='); if (!eq) continue; *eq = 0;
        char nm[32]; if (sscanf(line, " %31s", nm) != 1) continue;
        int from = key_vk_by_name(nm); unsigned a = 0, b = 0;
        int n = sscanf(eq + 1, " %i , %i", &a, &b);
        if (from >= 0 && n >= 1) alias_set(from, (int)a, n >= 2 ? (int)b : 0);
    }
    fclose(f);
}
void giz_key(int vk, int down) {
    if (vk < 0 || vk > 255) return;
    for (int i = 0; i < 2; i++) if (g_alias[vk][i]) input_key(g_alias[vk][i], down);
}
void giz_touch(int x, int y, int down) { input_touch(x, y, down); }
int giz_audio_read(int16_t *o, int n) { return audio_read(o, n); }
void giz_stop(void) { hle_running = 0; hle_exec_pending = 0; }
