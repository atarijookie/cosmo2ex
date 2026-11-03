TARGET	= ce_sting.prg

# control verbosity. use make V=1 to show commands
V = 0

AS        = m68k-atari-mint-as
CC        = m68k-atari-mint-gcc
STRIP     = m68k-atari-mint-strip
PRGFLAGS  = m68k-atari-mint-flags
CFLAGS    = -O2 -m68000 -fomit-frame-pointer -Wall -Wno-multichar -nostdlib -DSLIM # -DDEBUG_STRING
CFLAGS030 = -O2 -m68030 -fomit-frame-pointer -Wall -Wno-multichar -nostdlib -DSLIM
ASFLAGS   = -m68000
ASFLAGS030= -m68030
LDFLAGS   = -Wl,--traditional-format
LDLIBS    = -L../libacsiscsi -lacsiscsi -lgcc
DEPFLAGS  = -MM -MT $(patsubst %.d,%.o,$@) -MT $@

ACTUAL_CC := $(CC)
CC_0 = @echo "[$(if $(findstring .d,$@),Building dependencies for,Compiling $(if $(findstring 030,$@),030,68k) code)] $<"; $(ACTUAL_CC)
CC_1 = $(ACTUAL_CC)
CC = $(CC_$(V))
CCL_0 = @echo "[Linking final app] $@"; $(ACTUAL_CC)
CCL_1 = $(ACTUAL_CC)
CC_LINK = $(CCL_$(V))
ACTUAL_AS := $(AS)
AS_0 = @echo "[Assembling $(if $(findstring 030,$@),030,68k) code] $<"; $(ACTUAL_AS)
AS_1 = $(ACTUAL_AS)
AS = $(AS_$(V))

AT_0 := @
AT_1 :=
AT = $(AT_$(V))

HDR     = $(wildcard *.h)
CSRC    = $(wildcard *.c)
SSRC    = tpl_middle_jump.s vbl.s
STARTUP = startup.s

DEPS = $(CSRC:.c=.d)
OBJS = $(CSRC:.c=.o) $(SSRC:.s=.o)
OBJS030 = ../libacsiscsi/cache030.o

# next to ce_dd.prg and ce_eth.stx on the config drive in the source tree, see ../ce_sting_stx_driver/Makefile
CONFIGDRIVE_PRG = ../ce_cores/configdrive/drivers/$(TARGET)

all: $(TARGET) $(CONFIGDRIVE_PRG)

$(CONFIGDRIVE_PRG): $(TARGET)
	$(AT)mkdir -p $(dir $@)
	$(AT)cp $< $@.tmp && mv -f $@.tmp $@
	@echo "[Copied to config drive] $@"

.PHONY: all

$(TARGET): $(STARTUP) $(OBJS) $(OBJS030) ../libacsiscsi/libacsiscsi.a
	$(CC_LINK) $(CFLAGS) $(LDFLAGS) -o $@ $(STARTUP) $(OBJS) $(OBJS030) $(LDLIBS)
	$(AT)$(STRIP) -s $@
	$(AT)$(PRGFLAGS) $@ --mno-fastram --mno-fastalloc --mno-fastload

../libacsiscsi/libacsiscsi.a ../libacsiscsi/cache030.o:
	$(MAKE) -C ../libacsiscsi

%030.o: %030.s
	$(AS) $(ASFLAGS030) -c -o $@ $<

depend:	$(DEPS)

%.o: %.c %.d

%.d:	%.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -o $@ $<

clean:	depclean
	$(AT)$(RM) $(TARGET) *~ *.o

depclean:
	$(AT)$(RM) $(DEPS)

ifneq ($(MAKECMDGOALS),clean)
ifneq ($(MAKECMDGOALS),depclean)
-include $(DEPS)
endif
endif
