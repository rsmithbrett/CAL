CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O2
ARM_CXX ?= arm-linux-gnueabi-g++
ARM_FLAGS = -marm -march=armv6 -mfloat-abi=soft
QEMU_ARM ?= qemu-arm
ARM_SYSROOT ?= /usr/arm-linux-gnueabi

.PHONY: test test-armv6 test-checkin-transport demo checkin clean

test: build/schedule_core_test build/layout_test build/installation_config_test build/policy_state_test build/card_runtime_test
	./build/schedule_core_test
	./build/layout_test
	./build/installation_config_test
	./build/policy_state_test
	./build/card_runtime_test

# Original Pi Zero (ARM1176, 32-bit). Run the same source on emulated ARM,
# rather than assuming the host's x86 test proves target compatibility.
test-armv6: build/armv6/schedule_core_test build/armv6/layout_test build/armv6/installation_config_test build/armv6/policy_state_test build/armv6/card_runtime_test
	$(QEMU_ARM) -cpu arm1176 -L $(ARM_SYSROOT) ./build/armv6/schedule_core_test
	$(QEMU_ARM) -cpu arm1176 -L $(ARM_SYSROOT) ./build/armv6/layout_test
	$(QEMU_ARM) -cpu arm1176 -L $(ARM_SYSROOT) ./build/armv6/installation_config_test
	$(QEMU_ARM) -cpu arm1176 -L $(ARM_SYSROOT) ./build/armv6/policy_state_test
	$(QEMU_ARM) -cpu arm1176 -L $(ARM_SYSROOT) ./build/armv6/card_runtime_test

demo: build/pi-card

checkin: build/pi-checkin

test-checkin-transport: build/pi-checkin-test build/pi-checkin
	python3 tests/checkin_transport_test.py ./build/pi-checkin-test ./build/pi-checkin

build/pi-checkin-test: src/checkin.cpp src/CheckInTransport.h src/InstallationConfig.h src/PolicyState.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) -DPI_CHECKIN_TEST_LOOPBACK -Isrc src/checkin.cpp -o $@ -lcurl

build/pi-checkin: src/checkin.cpp src/CheckInTransport.h src/InstallationConfig.h src/PolicyState.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) -Isrc src/checkin.cpp -o $@ -lcurl

build/pi-card: src/main.cpp src/Layout.h src/CardRuntime.h src/LiveClient.h src/CheckInTransport.h src/PolicyState.h src/ScheduleCore.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags sdl2 SDL2_ttf) \
		src/main.cpp -o $@ $(shell pkg-config --libs sdl2 SDL2_ttf) -lcurl -pthread

build/schedule_core_test: tests/schedule_core_test.cpp src/ScheduleCore.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) -Isrc tests/schedule_core_test.cpp -o $@

build/layout_test: tests/layout_test.cpp src/Layout.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) -Isrc tests/layout_test.cpp -o $@

build/installation_config_test: tests/installation_config_test.cpp src/InstallationConfig.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) -Isrc tests/installation_config_test.cpp -o $@

build/armv6/schedule_core_test: tests/schedule_core_test.cpp src/ScheduleCore.h
	mkdir -p build/armv6
	$(ARM_CXX) $(CXXFLAGS) $(ARM_FLAGS) -Isrc tests/schedule_core_test.cpp -o $@

build/armv6/layout_test: tests/layout_test.cpp src/Layout.h
	mkdir -p build/armv6
	$(ARM_CXX) $(CXXFLAGS) $(ARM_FLAGS) -Isrc tests/layout_test.cpp -o $@

build/armv6/installation_config_test: tests/installation_config_test.cpp src/InstallationConfig.h
	mkdir -p build/armv6
	$(ARM_CXX) $(CXXFLAGS) $(ARM_FLAGS) -Isrc tests/installation_config_test.cpp -o $@

build/policy_state_test: tests/policy_state_test.cpp src/PolicyState.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) -Isrc tests/policy_state_test.cpp -o $@

build/armv6/policy_state_test: tests/policy_state_test.cpp src/PolicyState.h
	mkdir -p build/armv6
	$(ARM_CXX) $(CXXFLAGS) $(ARM_FLAGS) -Isrc tests/policy_state_test.cpp -o $@

clean:
	rm -rf build

build/card_runtime_test: tests/card_runtime_test.cpp src/CardRuntime.h src/PolicyState.h src/ScheduleCore.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) -Isrc $< -o $@

build/armv6/card_runtime_test: tests/card_runtime_test.cpp src/CardRuntime.h src/PolicyState.h src/ScheduleCore.h
	mkdir -p build/armv6
	$(ARM_CXX) $(CXXFLAGS) $(ARM_FLAGS) -Isrc $< -o $@

build/pi-card-test: src/main.cpp src/Layout.h src/CardRuntime.h src/LiveClient.h src/CheckInTransport.h src/PolicyState.h src/ScheduleCore.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) -DPI_CHECKIN_TEST_LOOPBACK $(shell pkg-config --cflags sdl2 SDL2_ttf) src/main.cpp -o $@ $(shell pkg-config --libs sdl2 SDL2_ttf) -lcurl -pthread

test-live-display: build/pi-card-test build/pi-card build/live_refresh_test
	python3 tests/live_display_test.py ./build/pi-card-test ./build/pi-card ./build/live_refresh_test

build/live_refresh_test: tests/live_refresh_test.cpp src/LiveClient.h src/CardRuntime.h src/CheckInTransport.h src/PolicyState.h src/InstallationConfig.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) -DPI_CHECKIN_TEST_LOOPBACK -Isrc $< -o $@ -lcurl
