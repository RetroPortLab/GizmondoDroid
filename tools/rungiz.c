/* Headless desktop runner: rungiz [-s N] [-k frame:vk:down ...] [-t frame:x:y:down] exe sdroot frames out.ppm */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../hle/gizmondo.h"

static void save_ppm(const char *path) {
    const uint16_t *fb = giz_screen(); FILE *f = fopen(path, "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", GIZ_W, GIZ_H);
    for (int i = 0; i < GIZ_W * GIZ_H; i++) {
        uint16_t p = fb[i]; uint8_t c[3] = { (uint8_t)(((p >> 11) & 31) * 255 / 31), (uint8_t)(((p >> 5) & 63) * 255 / 63), (uint8_t)((p & 31) * 255 / 31) };
        fwrite(c, 1, 3, f);
    }
    fclose(f);
}
int main(int argc, char **argv) {
    struct { int frame, vk, x, y, down, touch; } ev[256]; int nev = 0, shot_every = 0;
    int a = 1;
    for (; a < argc && argv[a][0] == '-'; a++) {
        if (!strcmp(argv[a], "-H")) { extern void hle_heapcheck(int); hle_heapcheck(1); continue; }
        if (!strcmp(argv[a], "-v")) { extern int hle_verbose; hle_verbose = 2; continue; }
        if (!strcmp(argv[a], "-s") && a + 1 < argc) shot_every = atoi(argv[++a]);
        else if (!strcmp(argv[a], "-k") && a + 1 < argc && nev < 256) { int f, v, d; sscanf(argv[++a], "%d:%i:%d", &f, &v, &d); ev[nev].frame = f; ev[nev].vk = v; ev[nev].down = d; ev[nev].touch = 0; nev++; }
        else if (!strcmp(argv[a], "-t") && a + 1 < argc && nev < 256) { int f, x, y, d; sscanf(argv[++a], "%d:%d:%d:%d", &f, &x, &y, &d); ev[nev].frame = f; ev[nev].x = x; ev[nev].y = y; ev[nev].down = d; ev[nev].touch = 1; nev++; }
    }
    if (argc - a < 4) { fprintf(stderr, "usage: rungiz [-s N] [-k frame:vk:down] [-t frame:x:y:down] exe sdroot frames out.ppm\n"); return 2; }
    const char *exe = argv[a], *sd = argv[a + 1]; int frames = atoi(argv[a + 2]); const char *out = argv[a + 3];
    char sys[600]; snprintf(sys, sizeof sys, "%s/../_giz_sys", sd);
    char err[256];
    if (giz_launch(exe, sd, sys, "", err, sizeof err)) { fprintf(stderr, "launch failed: %s\n", err); return 1; }
    int i;
    for (i = 0; i < frames && giz_running(); i++) {
        for (int e = 0; e < nev; e++) if (ev[e].frame == i) { if (ev[e].touch) giz_touch(ev[e].x, ev[e].y, ev[e].down); else giz_key(ev[e].vk, ev[e].down); }
        giz_frame();
        if (shot_every && i % shot_every == 0 && i) { char p[700]; snprintf(p, sizeof p, "%s.%05d.ppm", out, i); save_ppm(p); }
    }
    save_ppm(out);
    fprintf(stderr, "ran %d frames, running=%d exit=%d\n", i, giz_running(), giz_exit_code());
    return 0;
}
