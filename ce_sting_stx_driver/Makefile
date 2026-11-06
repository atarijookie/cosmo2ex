TARGET  = ce_eth.stx

# control verbosity. use make V=1 to show commands
V = 0

AS       = m68k-atari-mint-as
CC       = m68k-atari-mint-gcc
STRIP    = m68k-atari-mint-strip
PRGFLAGS = m68k-atari-mint-flags
ASFLAGS  = -m68000
CFLAGS   = -std=gnu99 -O2 -m68000 -fomit-frame-pointer -Wall -Wno-multichar -fno-common -nostdlib -DSLIM -I../libacsiscsi
LDFLAGS  = -Wl,--traditional-format
LDLIBS   = -L../libacsiscsi -lacsiscsi -lgcc
DEPFLAGS = -MM -MT $(patsubst %.d,%.o,$@) -MT $@

ACTUAL_CC := $(CC)
CC_0 = @echo "[$(if $(findstring .d,$@),Building dependencies for,Compiling) code] $<"; $(ACTUAL_CC)
CC_1 = $(ACTUAL_CC)
CC = $(CC_$(V))
CCL_0 = @echo "[Linking final app] $@"; $(ACTUAL_CC)
CCL_1 = $(ACTUAL_CC)
CC_LINK = $(CCL_$(V))
ACTUAL_AS := $(AS)
AS_0 = @echo "[Assembling code] $<"; $(ACTUAL_AS)
AS_1 = $(ACTUAL_AS)
AS = $(AS_$(V))

AT_0 := @
AT_1 :=
AT = $(AT_$(V))

CSRC    = main.c cenet.c
SSRC    = purec.s purecall.s
STARTUP = startup.s

DEPS = $(CSRC:.c=.d)
OBJS = $(CSRC:.c=.o) $(SSRC:.s=.o)

# next to ce_dd.prg on the config drive in the source tree, see ce_dd_prg/Makefile
CONFIGDRIVE_STX = ../ce_cores/configdrive/drivers/$(TARGET)

all: $(TARGET) $(CONFIGDRIVE_STX)

$(CONFIGDRIVE_STX): $(TARGET)
	$(AT)mkdir -p $(dir $@)
	$(AT)cp $< $@.tmp && mv -f $@.tmp $@
	@echo "[Copied to config drive] $@"

.PHONY: all

$(TARGET): $(STARTUP) $(OBJS) ../libacsiscsi/libacsiscsi.a
	$(CC_LINK) $(CFLAGS) $(LDFLAGS) -o $@ $(STARTUP) $(OBJS) $(LDLIBS)
	$(AT)$(STRIP) -s $@
	$(AT)$(PRGFLAGS) $@ --mno-fastram --mno-fastalloc --mno-fastload

../libacsiscsi/libacsiscsi.a:
	$(MAKE) -C ../libacsiscsi

depend:	$(DEPS)

%.o: %.c %.d

%.d:	%.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -o $@ $<

clean:	depclean
	$(AT)$(RM) $(TARGET) *~ $(OBJS)

depclean:
	$(AT)$(RM) $(DEPS)

ifneq ($(MAKECMDGOALS),clean)
ifneq ($(MAKECMDGOALS),depclean)
-include $(DEPS)
endif
endif
