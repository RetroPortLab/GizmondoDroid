# Desktop build of the GizmondoDroid core + headless runner (for testing; the Android app uses CMake)
CC ?= gcc
CFLAGS ?= -O2 -g -std=gnu11 -Wall -Wno-unused-function -Wno-misleading-indentation -Wno-format-truncation
SRC = core/cpu.c core/state.c hle/mem.c hle/api.c hle/pe.c hle/kern.c hle/crt.c hle/fs.c hle/gdi.c hle/win.c hle/gx.c hle/audio.c hle/misc.c hle/user.c hle/gizmondo.c
all: rungiz
rungiz: $(SRC) tools/rungiz.c
	$(CC) $(CFLAGS) -o $@ $^ -lm
clean:
	rm -f rungiz
