# Host (Linux) unit tests for the Pico firmwares ce_pico_hdd_acsi and ce_pico_hdd_scsi - doctest framework, see ../third_party/doctest
#
# The hardware independent firmware sources (utils, ipc, connection, command_handling) are compiled natively against
# the fakes in fakes/ (Arduino, pico-sdk, EEPROM, Ethernet), the PIO bridge is replaced per project by tests/<project>/bridge_fake.cpp.
# No third party library is compiled in. Each firmware gets its own test binary, as both define the same symbols.
#
#   make test                       - build and run both
#   make test-acsi / test-scsi      - just one of them, extra doctest options via TEST_ARGS, e.g. TEST_ARGS="-tc=*header* -s"
#   make test-asan                  - both, with AddressSanitizer + UndefinedBehaviorSanitizer
#   make test-known-bugs            - also the KNOWN_BUG test cases (skipped otherwise) - these fail until the bugs get fixed

PROJECTS        = acsi scsi

CXX            ?= g++
CXXFLAGS_BASE   = -std=gnu++17 -g -O0 -Wall -Wno-sign-compare -Wno-unused-variable -Wno-unused-function -MMD -MP \
                  -DPICO_NO_HARDWARE=1 -Ifakes -Itests -I../third_party/doctest
CXXFLAGS       ?=
LDFLAGS        ?=

SANITIZE_FLAGS  = -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer

FW_SRCS         = utils.cpp ipc.cpp connection.cpp command_handling.cpp
TEST_COMMON     = $(wildcard tests/*.cpp tests/common/*.cpp) fakes/fakes.cpp

.PHONY: all test test-asan test-known-bugs clean $(addprefix test-,$(PROJECTS))

all: $(addprefix run_tests_,$(addsuffix .elf,$(PROJECTS)))

test: $(addprefix test-,$(PROJECTS))

# $(1) = project (acsi / scsi)
define PROJECT_template
OBJDIR_$(1)  = obj/$(1)
FWOBJS_$(1)  = $$(addprefix obj/$(1)/fw/,$$(FW_SRCS:.cpp=.o))
TOBJS_$(1)   = $$(addprefix obj/$(1)/,$$(patsubst %.cpp,%.o,$$(TEST_COMMON) $$(wildcard tests/$(1)/*.cpp)))
FLAGS_$(1)   = $$(CXXFLAGS_BASE) -I../ce_pico_hdd_$(1) -DTEST_PROJECT_$(shell echo $(1) | tr a-z A-Z)=1 $$(CXXFLAGS)

obj/$(1)/fw/%.o: ../ce_pico_hdd_$(1)/%.cpp
	@mkdir -p $$(dir $$@)
	$$(CXX) $$(FLAGS_$(1)) -c $$< -o $$@

obj/$(1)/%.o: %.cpp
	@mkdir -p $$(dir $$@)
	$$(CXX) $$(FLAGS_$(1)) -c $$< -o $$@

run_tests_$(1).elf: $$(FWOBJS_$(1)) $$(TOBJS_$(1))
	$$(CXX) $$(CXXFLAGS) $$^ $$(LDFLAGS) -o $$@

test-$(1): run_tests_$(1).elf
	./run_tests_$(1).elf $$(TEST_ARGS)

-include $$(FWOBJS_$(1):.o=.d) $$(TOBJS_$(1):.o=.d)
endef

$(foreach p,$(PROJECTS),$(eval $(call PROJECT_template,$(p))))

# clean build, because all objects need the sanitizer flags - and clean again (also on failure), so the next normal build does not link them
test-asan:
	$(MAKE) clean
	$(MAKE) test CXXFLAGS="$(SANITIZE_FLAGS)" LDFLAGS="$(SANITIZE_FLAGS)"; status=$$?; $(MAKE) clean; exit $$status

# no sanitizers here - ASan would stop the whole run at the first memory bug; -k to run the 2nd binary even when the 1st fails
test-known-bugs:
	$(MAKE) -k test TEST_ARGS="--no-skip $(TEST_ARGS)"

clean:
	rm -rf obj run_tests_*.elf
