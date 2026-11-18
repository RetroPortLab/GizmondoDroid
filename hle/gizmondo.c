/* GizmondoDroid - glue: brings up memory, API tables, loads a program and runs it frame by frame */
#include "hle.h"
#include "gizmondo.h"
#include <stdio.h>
#include <stdlib.h>

extern void (*hle_log_sink)(const char *line);
static char g_sd[512], g_sys[512];
static uint32_t frames_run;

void giz_set_log(void (*sink)(const char *)) { hle_log_sink = sink; }
void giz_set_audio_rate(uint32_t hz) { audio_rate = hz ? hz : 44100; }

int giz_launch(const char *exe, const char *sd, const char *sys, const char *args, char *err, int errlen) {
    char dummy[8]; if (!err) { err = dummy; errlen = sizeof dummy; }
    err[0] = 0;
    snprintf(g_sd, sizeof g_sd, "%s", sd ? sd : "."); snprintf(g_sys, sizeof g_sys, "%s", sys ? sys : "./_sys");
    hle_running = 0;
    gm_init();
    api_reset(); pe_reset();
    cpu_reset();
    api_kernel_init();
    kern_init();
    api_crt_init(); fs_init_api(); win_init_api(); gdi_init_api(); gx_init_api(); audio_init_api(); api_misc_init();
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
void giz_key(int vk, int down) { input_key(vk, down); }
void giz_touch(int x, int y, int down) { input_touch(x, y, down); }
int giz_audio_read(int16_t *o, int n) { return audio_read(o, n); }
void giz_stop(void) { hle_running = 0; hle_exec_pending = 0; }
