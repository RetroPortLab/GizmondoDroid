## GizmondoDroid v0.9

- Fixed the broken sound: Carmageddon streams its audio through one looping ring buffer (waveOut loop flags with an infinite loop count). The emulator played it once and stopped, so the game kept resetting it every few seconds. Looping buffers are now supported, so sound is continuous.
