# CE_BROWS.PRG - web browser for CosmosEx (the Atari side of ../ce_browser and ../ce_cores/hdd/browser)
TARGET	= ce_brows.prg

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

CSRC    = $(wildcard *.c)
STARTUP = startup.s
OBJS    = $(CSRC:.c=.o)
DEPS    = $(CSRC:.c=.d)
OBJS030 = ../libacsiscsi/cache030.o

# in the root of the config drive
CONFIGDRIVE_PRG = ../ce_cores/configdrive/$(TARGET)

all: $(TARGET) $(CONFIGDRIVE_PRG)

$(CONFIGDRIVE_PRG): $(TARGET)
	$(AT)mkdir -p $(dir $@)
	$(AT)cp $< $@.tmp && mv -f $@.tmp $@
	@echo "[Copied to config drive] $@"

# TT RAM: the program may load there, its DMA and screen buffers are taken from ST RAM explicitly
$(TARGET): $(STARTUP) $(OBJS) $(OBJS030) ../libacsiscsi/libacsiscsi.a
	$(CC_LINK) $(CFLAGS) $(LDFLAGS) -o $@ $(STARTUP) $(OBJS) $(OBJS030) $(LDLIBS)
	$(AT)$(STRIP) -s $@
	$(AT)$(PRGFLAGS) $@ --mno-fastram --mno-fastalloc --mno-fastload

../libacsiscsi/libacsiscsi.a ../libacsiscsi/cache030.o:
	$(MAKE) -C ../libacsiscsi

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.d: %.c
	$(AT)$(ACTUAL_CC) $(CFLAGS) $(DEPFLAGS) -o $@ $<

clean:
	$(AT)rm -f $(TARGET) *.o *.d

.PHONY: all clean

ifneq ($(MAKECMDGOALS),clean)
-include $(DEPS)
endif
