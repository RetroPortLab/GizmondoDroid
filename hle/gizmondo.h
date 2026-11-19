/* GizmondoDroid - public interface of the emulator core (used by the Android JNI bridge and the desktop runner) */
#pragma once
#include <stdint.h>

#define GIZ_W 320
#define GIZ_H 240

/* Gizmondo virtual-key codes (what GAPI's GXGetDefaultKeys reports) */
#define GIZ_VK_UP    0x26
#define GIZ_VK_DOWN  0x28
#define GIZ_VK_LEFT  0x25
#define GIZ_VK_RIGHT 0x27
#define GIZ_VK_A     0xC1
#define GIZ_VK_B     0xC2
#define GIZ_VK_C     0xC3
#define GIZ_VK_START 0x0D

/* Start a Windows CE ARM program. sd_root is what the guest sees as \Storage Card (and \SD Card),
 * sys_root backs the rest of the guest file system (\Windows, \My Documents ...). Returns 0 on success. */
int  giz_launch(const char *exe_path, const char *sd_root, const char *sys_root, const char *args, char *err, int errlen);
void giz_set_frameskip(int on);               /* host is behind real time: skip rasterising upcoming frames */
void giz_frame(void);                         /* advance 1/60 s of emulated time (CPU + audio) */
const uint16_t *giz_screen(void);             /* 320x240 RGB565 */
int  giz_running(void);
int  giz_exit_code(void);
void giz_key(int vk, int down);
void giz_touch(int x, int y, int down);
int  giz_audio_read(int16_t *stereo, int frames);
void giz_set_audio_rate(uint32_t hz);
void giz_set_log(void (*sink)(const char *line));
void giz_stop(void);
