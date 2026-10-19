CXX	= g++
CC	= gcc

CFLAGS	= -Wall -g -D_FILE_OFFSET_BITS=64
CXXFLAGS	= $(CFLAGS) -std=gnu++17	# not c++17 - in that strict mode Cygwin's headers hide e.g. DT_DIR (see ce_cores/makefile)

TARGET	= libdospath.a
OBJS	= dirtranslator.o filenameshortener.o libexports.o utilslib.o

# platform switch, like in ce_cores/makefile - the library is the same everywhere, only the test binary name differs
UNAME_S ?= $(shell uname -s)

ifeq ($(UNAME_S),Linux)
BIN_EXT	= .elf
else ifeq ($(UNAME_S),Darwin)
BIN_EXT	=
else ifneq ($(filter CYGWIN_NT%,$(UNAME_S)),)
BIN_EXT	= .exe
CFLAGS	+= -D_GNU_SOURCE	# like g++ on Linux - see ce_cores/makefile
else
$(error Unsupported platform '$(UNAME_S)' - expected Linux, Darwin or CYGWIN_NT*)
endif

# unit tests - doctest framework, see ../third_party/doctest
TEST_DIR	= tests
TEST_SRCS	= $(wildcard $(TEST_DIR)/*.cpp)
TEST_OBJS	= $(TEST_SRCS:.cpp=.o)
TEST_BIN	= $(TEST_DIR)/run_tests$(BIN_EXT)
TEST_CXXFLAGS	= $(CXXFLAGS) -I. -I../third_party/doctest

.PHONY: all lib test test-asan clean

all: libdospath.a

libdospath.a: $(OBJS)
	$(AR) rcs libdospath.a $(OBJS)

$(OBJS): *.h		# rebuild when a header changes (built by the implicit %.o: %.cpp rule)

lib: libdospath.a

$(TEST_DIR)/%.o: $(TEST_DIR)/%.cpp $(wildcard $(TEST_DIR)/*.h) *.h
	$(CXX) $(TEST_CXXFLAGS) -c $< -o $@

$(TEST_BIN): libdospath.a $(TEST_OBJS)
	$(CXX) $(TEST_CXXFLAGS) $(TEST_OBJS) libdospath.a -o $(TEST_BIN)

# build and run unit tests, pass extra doctest options via TEST_ARGS, e.g.: make test TEST_ARGS="-tc=*symlink* -s"
test: $(TEST_BIN)
	./$(TEST_BIN) $(TEST_ARGS)

# same tests, built with AddressSanitizer + UndefinedBehaviorSanitizer - fails on memory errors, leaks and UB
# (does a clean build, because the library objects need to be rebuilt with sanitizers too)
SANITIZE_FLAGS	= -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer

test-asan:
ifneq ($(filter CYGWIN_NT%,$(UNAME_S)),)
	$(error AddressSanitizer is not available on Cygwin, use 'make test')
endif
	$(MAKE) clean
	$(MAKE) test CFLAGS="$(CFLAGS) $(SANITIZE_FLAGS)"
	$(MAKE) clean

clean:
	rm -f libtest *.o *.a $(TEST_DIR)/*.o $(TEST_BIN)
