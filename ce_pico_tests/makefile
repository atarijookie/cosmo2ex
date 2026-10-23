# Host (Linux) unit tests for the Pico firmwares ce_pico_hdd, ce_pico_hdd_acsi and ce_pico_hdd_scsi - doctest framework, see ../third_party/doctest
#
# The hardware independent firmware sources (utils, ipc, connection, command_handling) are compiled natively against
# the fakes in fakes/ (Arduino, pico-sdk, EEPROM, Ethernet), the PIO bridge is replaced per project by tests/<project>/bridge_fake.cpp.
# No third party library is compiled in. Each firmware gets its own test binary, as they define the same symbols.
# The merged ce_pico_hdd (ACSI or SCSI by the board variant) runs the acsi and scsi test cases too, plus its own in tests/hdd.
#
# ce_pico_fdd is tested the same way, but against its own pico-sdk fakes in fakes_fdd/ (it uses the sdk directly, not
# arduino-pico), with the tests in tests/fdd/ - see the fdd section below.
#
# ce_pico_display - only its hardware independent sources (protocol, keys), no fakes needed, tests in tests/display/.
#
#   make test                       - build and run all (acsi, scsi, hdd, fdd, display)
#   make test-acsi / test-scsi / test-hdd / test-fdd / test-display - just one of them, extra doctest options via TEST_ARGS, e.g. TEST_ARGS="-tc=*header* -s"
#   make test-asan                  - all, with AddressSanitizer + UndefinedBehaviorSanitizer
#   make test-known-bugs            - also the KNOWN_BUG test cases (skipped otherwise) - these fail until the bugs get fixed

PROJECTS        = acsi scsi hdd

# firmware dir and test case dirs of each project
FWDIR_acsi      = ../ce_pico_hdd_acsi
FWDIR_scsi      = ../ce_pico_hdd_scsi
FWDIR_hdd       = ../ce_pico_hdd
TESTDIRS_acsi   = tests/acsi
TESTDIRS_scsi   = tests/scsi
TESTDIRS_hdd    = tests/hdd tests/acsi tests/scsi

CXX            ?= g++
CXXFLAGS_BASE   = -std=gnu++17 -g -O0 -Wall -Wno-sign-compare -Wno-unused-variable -Wno-unused-function -MMD -MP \
                  -DPICO_NO_HARDWARE=1 -Ifakes -Itests -I../third_party/doctest
CXXFLAGS       ?=
LDFLAGS        ?=

SANITIZE_FLAGS  = -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer

FW_SRCS         = utils.cpp ipc.cpp connection.cpp command_handling.cpp
TEST_COMMON     = $(wildcard tests/*.cpp tests/common/*.cpp) fakes/fakes.cpp

.PHONY: all test test-asan test-known-bugs clean test-fdd test-display $(addprefix test-,$(PROJECTS))

all: $(addprefix run_tests_,$(addsuffix .elf,$(PROJECTS))) run_tests_fdd.elf run_tests_display.elf

test: $(addprefix test-,$(PROJECTS)) test-fdd test-display

# $(1) = project (acsi / scsi / hdd)
define PROJECT_template
OBJDIR_$(1)  = obj/$(1)
FWOBJS_$(1)  = $$(addprefix obj/$(1)/fw/,$$(FW_SRCS:.cpp=.o))
TSRCS_$(1)   = $$(wildcard tests/$(1)/bridge_fake.cpp) $$(filter-out %/bridge_fake.cpp,$$(foreach d,$$(TESTDIRS_$(1)),$$(wildcard $$(d)/*.cpp)))
TOBJS_$(1)   = $$(addprefix obj/$(1)/,$$(patsubst %.cpp,%.o,$$(TEST_COMMON) $$(TSRCS_$(1))))
FLAGS_$(1)   = $$(CXXFLAGS_BASE) -Itests/$(1) -I$$(FWDIR_$(1)) -DTEST_PROJECT_$(shell echo $(1) | tr a-z A-Z)=1 $$(CXXFLAGS)

obj/$(1)/fw/%.o: $$(FWDIR_$(1))/%.cpp
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

#--------------------------------------------------------
# ce_pico_fdd - core1 / write capture / psram / ipc / front panel / captive portal page / beeper sources against fakes_fdd/, connection.h skipped via force_include.h
FDD_FW_SRCS     = core1_main.cpp mfm_write.cpp psram.cpp ipc.cpp utils.cpp written_sectors.cpp host_receive.cpp panel.cpp portal_form.cpp beeper.cpp
FDD_FWOBJS      = $(addprefix obj/fdd/fw/,$(FDD_FW_SRCS:.cpp=.o))
FDD_TOBJS       = $(addprefix obj/fdd/,$(patsubst %.cpp,%.o,tests/test_main.cpp $(wildcard tests/fdd/*.cpp) fakes_fdd/fakes_fdd.cpp))
FLAGS_fdd       = -std=gnu++17 -g -O0 -Wall -Wno-sign-compare -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-variable -MMD -MP \
                  -DPICO_NO_HARDWARE=1 -Ifakes_fdd -Itests -I../third_party/doctest -I../ce_pico_fdd -include fakes_fdd/force_include.h $(CXXFLAGS)

obj/fdd/fw/%.o: ../ce_pico_fdd/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(FLAGS_fdd) -c $< -o $@

obj/fdd/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(FLAGS_fdd) -c $< -o $@

run_tests_fdd.elf: $(FDD_FWOBJS) $(FDD_TOBJS)
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

test-fdd: run_tests_fdd.elf
	./run_tests_fdd.elf $(TEST_ARGS)

-include $(FDD_FWOBJS:.o=.d) $(FDD_TOBJS:.o=.d)

#--------------------------------------------------------
# ce_pico_display - protocol parser and keys, plain C++
DISPLAY_FW_SRCS = protocol.cpp keys.cpp
DISPLAY_FWOBJS  = $(addprefix obj/display/fw/,$(DISPLAY_FW_SRCS:.cpp=.o))
DISPLAY_TOBJS   = $(addprefix obj/display/,$(patsubst %.cpp,%.o,tests/test_main.cpp $(wildcard tests/display/*.cpp)))
FLAGS_display   = $(CXXFLAGS_BASE) -I../ce_pico_display $(CXXFLAGS)

obj/display/fw/%.o: ../ce_pico_display/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(FLAGS_display) -c $< -o $@

obj/display/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(FLAGS_display) -c $< -o $@

run_tests_display.elf: $(DISPLAY_FWOBJS) $(DISPLAY_TOBJS)
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

test-display: run_tests_display.elf
	./run_tests_display.elf $(TEST_ARGS)

-include $(DISPLAY_FWOBJS:.o=.d) $(DISPLAY_TOBJS:.o=.d)

# clean build, because all objects need the sanitizer flags - and clean again (also on failure), so the next normal build does not link them
test-asan:
	$(MAKE) clean
	$(MAKE) test CXXFLAGS="$(SANITIZE_FLAGS)" LDFLAGS="$(SANITIZE_FLAGS)"; status=$$?; $(MAKE) clean; exit $$status

# no sanitizers here - ASan would stop the whole run at the first memory bug; -k to run the other binaries even when one fails
test-known-bugs:
	$(MAKE) -k test TEST_ARGS="--no-skip $(TEST_ARGS)"

clean:
	rm -rf obj run_tests_*.elf
