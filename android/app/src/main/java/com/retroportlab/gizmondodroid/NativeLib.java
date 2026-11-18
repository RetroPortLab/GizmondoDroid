package com.retroportlab.gizmondodroid;

import android.view.Surface;

/** The HLE emulator core (core/ and hle/ at the repository root) and its thread, in libgizmondodroid.so. */
final class NativeLib {
    static { System.loadLibrary("gizmondodroid"); }

    static native boolean launch(String exe, String sdRoot, String sysRoot, String args);
    static native String lastError();
    static native void setSurface(Surface s);
    static native void start();
    static native void stop();
    static native void setPaused(boolean p);
    static native void setFilter(int mode);
    static native float getFps();
    static native boolean isRunning();
    static native void setTouch(int x, int y, boolean down);   // 320x240 screen coordinates
    static native void setKeys(int mask);

    // bit order matches key_vk[] in jni_bridge.c
    static final int UP = 1, DOWN = 2, LEFT = 4, RIGHT = 8, A = 16, B = 32, C = 64, START = 128;

    static final int SCREEN_W = 320, SCREEN_H = 240;
}
