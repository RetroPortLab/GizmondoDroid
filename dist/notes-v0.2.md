GizmondoDroid v0.2

- New: software OpenGL ES 1.x + EGL, so 3D Gizmondo games load. Carmageddon (Gizmondo beta) now boots to its language menu and the Gizmondo logo screens.
- Texture support: RGBA/RGB/565/4444 uploads, OES paletted and S3TC (DXT1/3/5) compressed textures.
- Scheduler now runs other threads while a window callback is waiting (fixes startup hangs).
- Fixes: wide printf %hs, large fread/fwrite, strtok/wcstok, message queues, volume device probing, directory creation errors.
- Known: some textures (e.g. flag icons in Carmageddon's menu) still render black; lighting isn't applied; no save states.
