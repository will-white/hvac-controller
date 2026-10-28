# Native (host) test build: the firmware's control logic, the on-device test
# suite and the native-only scenario tests, compiled with g++ against the
# shims in shim/. Networking modules are replaced by stubs.cpp.
#
#   make run      build and run (what scripts/native-test.sh and CI call)
#   make clean
#
# Built with AddressSanitizer + UBSan: memory errors and undefined behaviour in
# the logic fail the run.

SRC := ../../src
BUILD := build
FW := console events health hvac_logic hvac_tests inputs learning main \
      netconfig ota_util relays sensors setpoints status_led
TESTS := native_shim stubs scenarios test_main

CXX ?= g++
SAN ?= -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
# The -Wno- flags silence style warnings in existing firmware code, not bugs.
CXXFLAGS ?= -std=gnu++17 -O1 -g -Wall -Wno-unused-function -Wno-unused-variable \
            -Wno-class-memaccess -Wno-misleading-indentation
CPPFLAGS := -Ishim -I$(SRC) -DNATIVE_TEST -include shim/native_compat.h

OBJS := $(FW:%=$(BUILD)/fw_%.o) $(TESTS:%=$(BUILD)/%.o)
BIN := $(BUILD)/hvac_native_tests

all: $(BIN)

run: $(BIN)
	ASAN_OPTIONS=detect_leaks=0 ./$(BIN)

$(BIN): $(OBJS)
	$(CXX) $(CXXFLAGS) $(SAN) -o $@ $^

$(BUILD)/fw_%.o: $(SRC)/%.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SAN) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SAN) -MMD -MP -c $< -o $@

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
-include $(OBJS:.o=.d)
