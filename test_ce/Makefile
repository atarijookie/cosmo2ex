TARGET	= ce_tests.tos

# control verbosity. use make V=1 to show commands
V = 0

CC			= m68k-atari-mint-gcc
AS			= m68k-atari-mint-as
LD			= m68k-atari-mint-ld
OBJCOPY		= m68k-atari-mint-objcopy
STRIP		= m68k-atari-mint-strip
PRGFLAGS	= m68k-atari-mint-flags
ASFLAGS		= -m68000
CFLAGS		= -O2 -m68000 -fomit-frame-pointer -Wall -Wno-multichar -nostdlib -fno-common -DSLIM
LDFLAGS		= -Wl,--traditional-format
LIBDIR		= ../libacsiscsi
LDLIBS		= -L$(LIBDIR) -lacsiscsi -lgcc

ACTUAL_CC := $(CC)
CC_0 = @echo "[Compiling 68k code] $<"; $(ACTUAL_CC)
CC_1 = $(ACTUAL_CC)
CC = $(CC_$(V))
CCL_0 = @echo "[Linking final app] $@"; $(ACTUAL_CC)
CCL_1 = $(ACTUAL_CC)
CC_LINK = $(CCL_$(V))
ACTUAL_AS := $(AS)
AS_0 = @echo "[Assembling 68k code] $<"; $(ACTUAL_AS)
AS_1 = $(ACTUAL_AS)
AS = $(AS_$(V))
ACTUAL_LD := $(LD)
LD_0 = @echo "[Linking sub-test] $@"; $(ACTUAL_LD)
LD_1 = $(ACTUAL_LD)
LD = $(LD_$(V))

AT_0 := @
AT_1 :=
AT = $(AT_$(V))

# Each sub-test (sub-folder) is linked into a single object, in which only its entry point
# <subtest>_main() stays global. Everything else the sub-test defines becomes local, so
# sub-tests can't clash with each other on names like main(), showMenu() or deviceID.
# Whatever a sub-test doesn't define itself comes from the shared code (common/ and libacsiscsi).
SUBTESTS = test_acsi test_filesystem test_floppy test_floppy_read test_ikbd test_manual test_manual2 test_rw test_rw_user

BUILD	= build
STARTUP	= startup.s
OBJS	= main.o $(patsubst %.c,%.o,$(wildcard common/*.c)) $(patsubst %.s,%.o,$(wildcard common/*.s))
SUBOBJS	= $(patsubst %,$(BUILD)/%.o,$(SUBTESTS))
OBJS030	= $(LIBDIR)/cache030.o

# the app is also shipped on the CosmosEx config drive, in its TESTS\ folder
CONFIGDRIVE_TOS	= ../ce_cores/configdrive/tests/$(TARGET)

.PHONY: all clean FORCE

all: $(TARGET) $(CONFIGDRIVE_TOS)

define SUBTEST_template
$(1)_OBJS := $$(patsubst %.c,%.o,$$(wildcard $(1)/*.c $(1)/*/*.c)) $$(patsubst %.s,%.o,$$(wildcard $(1)/*.s $(1)/*/*.s))
ALL_SUBTEST_OBJS += $$($(1)_OBJS)

$(BUILD)/$(1).o: $$($(1)_OBJS) | $(BUILD)
	$$(LD) -r -o $$@.tmp $$^
	$(AT)$(OBJCOPY) --keep-global-symbol=_$(1)_main $$@.tmp $$@
	$(AT)$(RM) $$@.tmp
endef

$(foreach t,$(SUBTESTS),$(eval $(call SUBTEST_template,$(t))))

DEPS	= $(patsubst %.o,%.d,$(filter-out %.s,$(OBJS) $(ALL_SUBTEST_OBJS)))

$(TARGET): $(STARTUP) $(OBJS) $(SUBOBJS) $(LIBDIR)/libacsiscsi.a
	$(CC_LINK) $(CFLAGS) $(LDFLAGS) -o $@ $(STARTUP) $(OBJS) $(SUBOBJS) $(OBJS030) $(LDLIBS)
	$(AT)$(STRIP) -s $@
	$(AT)$(PRGFLAGS) $@ --mno-fastram --mno-fastalloc --mno-fastload

$(CONFIGDRIVE_TOS): $(TARGET)
	$(AT)mkdir -p $(dir $@)
	$(AT)cp $< $@
	@echo "[Copied to config drive] $@"

# the shared HDD IF / stdlib library, let its own makefile decide if it needs rebuilding
$(LIBDIR)/libacsiscsi.a: FORCE
	$(AT)$(MAKE) -C $(LIBDIR)

$(BUILD):
	$(AT)mkdir -p $@

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

%.o: %.s
	$(AS) $(ASFLAGS) -o $@ $<

clean:
	$(AT)$(RM) $(TARGET) $(CONFIGDRIVE_TOS) $(OBJS) $(ALL_SUBTEST_OBJS) $(DEPS)

	$(AT)$(RM) -r $(BUILD)

-include $(DEPS)
