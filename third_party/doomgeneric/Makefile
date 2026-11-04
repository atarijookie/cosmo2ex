# ce_doomgeneric - Doom for CosmosEx (CE_DOOM.PRG): doomgeneric with the port in ce/, frames to stdout, keys from stdin.
# Plain C, no libraries - builds with the same compilers as ce_cores (CC=... for cross builds, e.g. from
# release/builder/build_linux.sh). GPL-2, a separate program - ce_media starts it.
#
#   make                    ce_doomgeneric (ce_doomgeneric.exe on Cygwin)
#   make CC=aarch64-linux-gnu-gcc

CC      ?= cc
STRIP   ?= strip
CFLAGS  ?= -O2
CFLAGS  += -std=gnu99 -fcommon -w -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE \
           -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DCMAP256 -DFEATURE_SOUND -DCE_DOOMGENERIC -Isrc
LIBS    = -lm

UNAME_S ?= $(shell uname -s)
EXE     =
ifneq (,$(findstring CYGWIN,$(UNAME_S)))
EXE     = .exe
endif

OUT     = ce_doomgeneric$(EXE)
SRCS    = $(wildcard src/*.c) ce/doomgeneric_ce.c
OBJDIR  = obj
OBJS    = $(patsubst %.c,$(OBJDIR)/%.o,$(SRCS))

all: $(OUT)

$(OUT): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJDIR) ce_doomgeneric ce_doomgeneric.exe

.PHONY: all clean
