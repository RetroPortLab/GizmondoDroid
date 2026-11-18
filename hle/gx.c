/* GizmondoDroid - GAPI (gx.dll): direct framebuffer access, display properties, default keys */
#include "hle.h"

/* Virtual key codes the Gizmondo hardware reports (also used by the front-end). */
#define VK_GZ_UP 0x26
#define VK_GZ_DOWN 0x28
#define VK_GZ_LEFT 0x25
#define VK_GZ_RIGHT 0x27
#define VK_GZ_A 0xC1
#define VK_GZ_B 0xC2
#define VK_GZ_C 0xC3
#define VK_GZ_START 0x0D

static int gx_open, gx_input, gx_drawing;

static void api_GXOpenDisplay(void) { gx_open = 1; R(1); }
static void api_GXCloseDisplay(void) { gx_open = 0; R(1); }
static void api_GXBeginDraw(void) { gx_drawing = 1; R(FB_ADDR); }
static void api_GXEndDraw(void) { gx_drawing = 0; frame_dirty = 1; R(1); }
static void api_GXOpenInput(void) { gx_input = 1; R(1); }
static void api_GXCloseInput(void) { gx_input = 0; R(1); }
static void api_GXSuspend(void) { R(1); }
static void api_GXResume(void) { R(1); }
static void api_GXSetViewport(void) { R(1); }
static void api_GXIsDisplayDRAMBuffer(void) { R(0); }
static void api_GXGetDisplayProperties(void) {   /* returns a struct: hidden pointer in r0 */
    uint32_t p = cpu.r[0];
    w32(p, SCREEN_W); w32(p + 4, SCREEN_H); w32(p + 8, 2); w32(p + 12, SCREEN_W * 2); w32(p + 16, 16);
    w32(p + 20, 0x80 | 0x20);        /* kfDirect565 | kfDirect */
    R(p);
}
static void api_GXGetDefaultKeys(void) {         /* hidden pointer in r0, option in r1 */
    uint32_t p = cpu.r[0];
    static const int vk[8] = { VK_GZ_UP, VK_GZ_DOWN, VK_GZ_LEFT, VK_GZ_RIGHT, VK_GZ_A, VK_GZ_B, VK_GZ_C, VK_GZ_START };
    for (int i = 0; i < 8; i++) { w16(p + 12 * i, (uint16_t)vk[i]); w16(p + 12 * i + 2, 0); w32(p + 12 * i + 4, 0); w32(p + 12 * i + 8, 0); }
    R(p);
}

#define E(n, f) { n, f }
static const ApiDef xdefs[] = {
    E("?GXOpenDisplay@@YAHPAUHWND__@@K@Z", api_GXOpenDisplay), E("?GXCloseDisplay@@YAHXZ", api_GXCloseDisplay), E("?GXBeginDraw@@YAPAXXZ", api_GXBeginDraw),
    E("?GXEndDraw@@YAHXZ", api_GXEndDraw), E("?GXOpenInput@@YAHXZ", api_GXOpenInput), E("?GXCloseInput@@YAHXZ", api_GXCloseInput), E("?GXSuspend@@YAHXZ", api_GXSuspend),
    E("?GXResume@@YAHXZ", api_GXResume), E("?GXSetViewport@@YAHKKKK@Z", api_GXSetViewport), E("?GXIsDisplayDRAMBuffer@@YAHXZ", api_GXIsDisplayDRAMBuffer),
    E("?GXGetDisplayProperties@@YA?AUGXDisplayProperties@@XZ", api_GXGetDisplayProperties), E("?GXGetDefaultKeys@@YA?AUGXKeyList@@H@Z", api_GXGetDefaultKeys),
};
void gx_init_api(void) { gx_open = gx_input = gx_drawing = 0; api_register(xdefs, sizeof xdefs / sizeof xdefs[0]); }
