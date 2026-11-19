## GizmondoDroid v0.5

- Fixed black speckles on textures (alpha test compared at full float precision; now 8-bit like real hardware)
- Frame skipping: when the device falls behind real time, rasterising is skipped for up to 2 frames in a row so game speed and audio stay steady (fixes jumping graphics)
- Audio: refills ~2 frames before resuming after an underrun (less crackle / cut-outs)
- Faster texture mapping (perspective divide every 8 pixels)
