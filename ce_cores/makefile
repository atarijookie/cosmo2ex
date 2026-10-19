# control verbosity. use make V=1
V = 0

CXX	    = g++
CC	    = gcc
#STRIP	= strip
CFLAGS	= -Wall -g -D_FILE_OFFSET_BITS=64 $(CPU_FLAGS)
CXXFLAGS = $(CFLAGS)
DEPFLAGS = -MM -MT $(patsubst %.d,%.o,$@) -MT $@
# -M : with system headers, -MM : without

VPATH = ./lib
LDFLAGS	= -Llib $(CPU_FLAGS)
# -lpthread: glibc before 2.34 (e.g. Debian bullseye, Raspberry Pi OS bullseye) has pthreads in a separate library
LDLIBS = -ldospath -lpthread
OBJDIR = obj
TARGET = ce_cores.elf

CFLAGS += -I./

# USB keyboard / mouse / joysticks (ikbd/inputdevices.cpp): found with libudev, inspected with libevdev - Linux only.
# Needs libudev-dev + libevdev-dev. libevdev is linked statically (it's not installed on every system), libudev.so.1
# is part of systemd / udev, so it's everywhere where udev is. Headers are taken from the sysroot of the compiler
# (empty for native builds, the Raspbian one for armhf). Not in LDLIBS - that is also a prerequisite list,
# where -l:libevdev.a doesn't work.
ifeq ($(shell uname -s),Linux)
CXX_SYSROOT := $(shell $(CXX) -print-sysroot)
CFLAGS += -I$(CXX_SYSROOT)/usr/include/libevdev-1.0
LIBEVDEV ?= -l:libevdev.a
INPUT_LIBS = -ludev $(LIBEVDEV)
endif

# uncomment following line to get map file (ce.map)
#LDFLAGS += -Wl,-Map,ce.map

ACTUAL_CC := $(CC)
CC_0 = @echo "$(if $(findstring .d,$@),Building dependencies for,Compiling) $<"; $(ACTUAL_CC)
CC_1 = $(ACTUAL_CC)
CC = $(CC_$(V))
ACTUAL_CXX := $(CXX)
CXX_0 = @echo "$(if $(findstring .d,$@),Building dependencies for,Compiling) $<"; $(ACTUAL_CXX)
CXX_1 = $(ACTUAL_CXX)
CXX = $(CXX_$(V))
CXXL_0 = @echo "Linking $@"; $(ACTUAL_CXX)
CXXL_1 = $(ACTUAL_CXX)
CXX_LINK = $(CXXL_$(V))

AT_0 := @
AT_1 :=
AT = $(AT_$(V))

SUBDIR1 = chipinterface
SUBDIR2 = misc
SUBDIR3 = discovery
SUBDIR4 = hdd
SUBDIR5	= hdd/native
SUBDIR6	= hdd/translated
SUBDIR7	= hdd/acsicommand
SUBDIR8 = fdd
SUBDIR9 = ikbd

SRCS  = $(wildcard *.cpp)
SRCS += $(wildcard $(SUBDIR1)/*.cpp)
SRCS += $(wildcard $(SUBDIR2)/*.cpp)
SRCS += $(wildcard $(SUBDIR3)/*.cpp)
SRCS += $(wildcard $(SUBDIR4)/*.cpp)
SRCS += $(wildcard $(SUBDIR5)/*.cpp)
SRCS += $(wildcard $(SUBDIR6)/*.cpp)
SRCS += $(wildcard $(SUBDIR7)/*.cpp)
SRCS += $(wildcard $(SUBDIR8)/*.cpp)
SRCS += $(wildcard $(SUBDIR9)/*.cpp)

HDRS  = $(wildcard *.h)
HDRS += $(wildcard $(SUBDIR1)/*.h)
HDRS += $(wildcard $(SUBDIR2)/*.h)
HDRS += $(wildcard $(SUBDIR3)/*.h)
HDRS += $(wildcard $(SUBDIR4)/*.h)
HDRS += $(wildcard $(SUBDIR5)/*.h)
HDRS += $(wildcard $(SUBDIR6)/*.h)
HDRS += $(wildcard $(SUBDIR7)/*.h)
HDRS += $(wildcard $(SUBDIR8)/*.h)
HDRS += $(wildcard $(SUBDIR9)/*.h)
HDRS += ../libdospath/libdospath.h

OBJS = $(addprefix $(OBJDIR)/, $(patsubst %.cpp,%.o,$(SRCS:.c=.o)))
DEPS = $(patsubst %.o,%.d,$(OBJS))

# unit tests - doctest framework, see ../third_party/doctest
# test binary links all app objects except main.o (tests/test_globals.cpp defines the globals from main.cpp)
TEST_DIR  = tests
TEST_SRCS = $(wildcard $(TEST_DIR)/*.cpp)
TEST_OBJS = $(addprefix $(OBJDIR)/, $(patsubst %.cpp,%.o,$(TEST_SRCS)))
TEST_DEPS = $(patsubst %.o,%.d,$(TEST_OBJS))
TEST_BIN  = $(TEST_DIR)/run_tests.elf
APP_OBJS_NO_MAIN = $(filter-out $(OBJDIR)/main.o, $(OBJS))

$(TEST_OBJS) $(TEST_DEPS): CXXFLAGS += -I../third_party/doctest

all:	$(TARGET)

$(TARGET): $(LDLIBS) $(OBJS) $(HDRS)
	$(CXX_LINK) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@ $(LDLIBS) $(INPUT_LIBS)
#	$(STRIP) -s $@
	@ln -sf $(CURDIR)/$(TARGET) ./core/cosmosex.elf		# create symlink from whatever the output target is to just cosmosex.elf

print-%:	; @echo $* = $($*)

$(TEST_BIN): $(LDLIBS) $(APP_OBJS_NO_MAIN) $(TEST_OBJS)
	$(CXX_LINK) $(CFLAGS) $(APP_OBJS_NO_MAIN) $(TEST_OBJS) $(LDFLAGS) -o $@ $(LDLIBS) $(INPUT_LIBS)

# build and run unit tests, pass extra doctest options via TEST_ARGS, e.g.: make test TEST_ARGS="-tc=*substitution* -s"
test:	$(TEST_BIN)
	./$(TEST_BIN) $(TEST_ARGS)

-ldospath:	lib/libdospath.a

lib/libdospath.a:
	@echo "-> Entering libdospath and making libdospath"
	cd ../libdospath && $(MAKE) all
	mkdir -p lib
	cp ../libdospath/libdospath.a lib/
	@echo "<- Leaving libdospath"

.PHONY:	clean depclean test

clean: depclean
	$(AT)$(RM) $(OBJS) $(TEST_OBJS)
	$(AT)$(RM) *.o *~ $(TARGET) $(TEST_BIN)

depclean:
	$(AT)$(RM) $(DEPS) $(TEST_DEPS)

depend:	$(DEPS)

$(OBJDIR)/%.o:	%.cpp $(OBJDIR)/%.d
	$(COMPILE.cpp) $(OUTPUT_OPTION) $<

$(OBJDIR)/%.d:	%.cpp
	$(AT)mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -o $@ $<

$(OBJDIR)/%.o:	%.c $(OBJDIR)/%.d
	$(COMPILE.c) $(OUTPUT_OPTION) $<

$(OBJDIR)/%.d:	%.c
	$(AT)mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -o $@ $<

ifneq ($(MAKECMDGOALS),clean)
ifneq ($(MAKECMDGOALS),depclean)
-include $(DEPS)
-include $(TEST_DEPS)
endif
endif
