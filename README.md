# GizmondoDroid

A from-scratch Gizmondo emulator for Android (ARM920T interpreter/JIT + Windows CE high-level emulation), in the style of JagDroid / 3DODroid / ZodiacDroid.

- Runs Gizmondo Windows CE ARM `.exe` homebrew/games directly (HLE of the CE API, GDI, GAPI, waveOut, file system).
- Put programs in `/sdcard/GizmondoDroid/` (the guest sees it as `\Storage Card`).
- Desktop runner: `make && ./rungiz [-s N] exe sdroot frames out.ppm`.
- Android app in `android/`; releases are published from `dist/` by `.github/workflows/release.yml`.
- Not yet: save states; booting the original firmware (needs a user-supplied key).
