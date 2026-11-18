## GizmondoDroid v0.4

- Carmageddon (Gizmondo beta) now loads into the race and is playable-rendered
- Fixed `sscanf` `%[...]` / `%*` support (was breaking level/accessory parsing)
- Much faster software GL ES renderer (span rasterizer), vsync frame pacing and virtual-time clocks
- BGRA texture support (fixes black icons/flags)
- Key alias profile for Carmageddon (A=Enter, B=Space, C=Esc style); override with `gizmondodroid-keys.txt`

Known: Android on-device performance and ARM64 JIT path are still lightly tested.
