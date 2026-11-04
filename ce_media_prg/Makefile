# CE_VIDEO.TTP and CE_AUDIO.TTP - video / audio players for CosmosEx (the Atari side of ../ce_media and
# ../ce_cores/hdd/media) - and CE_DOOM.PRG, Doom running on the host. All copied to the root of the config drive.

# control verbosity. use make V=1 to show commands
V = 0

AS        = m68k-atari-mint-as
CC        = m68k-atari-mint-gcc
STRIP     = m68k-atari-mint-strip
PRGFLAGS  = m68k-atari-mint-flags
CFLAGS    = -std=gnu99 -O2 -m68000 -fomit-frame-pointer -Wall -Wno-multichar -nostdlib -fno-common -DSLIM
ASFLAGS   = -m68000
LDFLAGS   = -Wl,--traditional-format
LDLIBS    = -L../libacsiscsi -lacsiscsi -lgcc
DEPFLAGS  = -MM -MT $(patsubst %.d,%.o,$@) -MT $@

ACTUAL_CC := $(CC)
CC_0 = @echo "[Compiling] $<"; $(ACTUAL_CC)
CC_1 = $(ACTUAL_CC)
CC = $(CC_$(V))
CCL_0 = @echo "[Linking] $@"; $(ACTUAL_CC)
CCL_1 = $(ACTUAL_CC)
CC_LINK = $(CCL_$(V))

AT_0 := @
AT_1 :=
AT = $(AT_$(V))

COMMON  = app.o dbg.o med.o player.o ui.o vbl.o version.o ymisr.o
STARTUP = startup.s
OBJS030 = ../libacsiscsi/cache030.o
DEPS    = $(patsubst %.c,%.d,$(wildcard *.c))
TARGETS = ce_video.ttp ce_audio.ttp ce_doom.prg

CONFIGDRIVE = ../ce_cores/configdrive

all: $(TARGETS) $(addprefix $(CONFIGDRIVE)/,$(TARGETS))

$(CONFIGDRIVE)/%: %
	$(AT)mkdir -p $(dir $@)
	$(AT)cp $< $@.tmp && mv -f $@.tmp $@
	@echo "[Copied to config drive] $@"

ce_doom.prg: $(STARTUP) ce_doom.o keys.o $(COMMON) $(OBJS030) ../libacsiscsi/libacsiscsi.a
	$(CC_LINK) $(CFLAGS) $(LDFLAGS) -o $@ $(STARTUP) ce_doom.o keys.o $(COMMON) $(OBJS030) $(LDLIBS)
	$(AT)$(STRIP) -s $@
	$(AT)$(PRGFLAGS) $@ --mno-fastram --mno-fastalloc --mno-fastload

# TT RAM: the program may load there, but its DMA buffers, screens and sound buffer are taken from ST RAM explicitly;
# the VBL routine and timer interrupt are in the program - keep it in ST RAM too (simpler, and the TT has plenty)
%.ttp: $(STARTUP) %.o $(COMMON) $(OBJS030) ../libacsiscsi/libacsiscsi.a
	$(CC_LINK) $(CFLAGS) $(LDFLAGS) -o $@ $(STARTUP) $*.o $(COMMON) $(OBJS030) $(LDLIBS)
	$(AT)$(STRIP) -s $@
	$(AT)$(PRGFLAGS) $@ --mno-fastram --mno-fastalloc --mno-fastload

../libacsiscsi/libacsiscsi.a ../libacsiscsi/cache030.o:
	$(MAKE) -C ../libacsiscsi

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# build stamp: date and time of this build, compiled again every time
version.o: FORCE
	$(CC) $(CFLAGS) -DBUILD_STAMP='"$(shell date '+%Y-%m-%d %H:%M:%S')"' -c version.c -o $@

%.o: %.s
	$(AT)$(AS) $(ASFLAGS) $< -o $@

%.d: %.c
	$(AT)$(ACTUAL_CC) $(CFLAGS) $(DEPFLAGS) -o $@ $<

clean:
	$(AT)rm -f $(TARGETS) *.o *.d

.PHONY: all clean FORCE
.SECONDARY:

ifneq ($(MAKECMDGOALS),clean)
-include $(DEPS)
endif
