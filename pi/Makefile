CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O2
ARM_CXX ?= arm-linux-gnueabi-g++
ARM_FLAGS = -marm -march=armv6 -mfloat-abi=soft
QEMU_ARM ?= qemu-arm
ARM_SYSROOT ?= /usr/arm-linux-gnueabi

.PHONY: test test-armv6 demo clean

test: build/schedule_core_test build/layout_test
	./build/schedule_core_test
	./build/layout_test

# Original Pi Zero (ARM1176, 32-bit). Run the same source on emulated ARM,
# rather than assuming the host's x86 test proves target compatibility.
test-armv6: build/armv6/schedule_core_test build/armv6/layout_test
	$(QEMU_ARM) -cpu arm1176 -L $(ARM_SYSROOT) ./build/armv6/schedule_core_test
	$(QEMU_ARM) -cpu arm1176 -L $(ARM_SYSROOT) ./build/armv6/layout_test

demo: build/pi-card

build/pi-card: src/main.cpp src/Layout.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags sdl2 SDL2_ttf) \
		src/main.cpp -o $@ $(shell pkg-config --libs sdl2 SDL2_ttf)

build/schedule_core_test: tests/schedule_core_test.cpp src/ScheduleCore.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) -Isrc tests/schedule_core_test.cpp -o $@

build/layout_test: tests/layout_test.cpp src/Layout.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) -Isrc tests/layout_test.cpp -o $@

build/armv6/schedule_core_test: tests/schedule_core_test.cpp src/ScheduleCore.h
	mkdir -p build/armv6
	$(ARM_CXX) $(CXXFLAGS) $(ARM_FLAGS) -Isrc tests/schedule_core_test.cpp -o $@

build/armv6/layout_test: tests/layout_test.cpp src/Layout.h
	mkdir -p build/armv6
	$(ARM_CXX) $(CXXFLAGS) $(ARM_FLAGS) -Isrc tests/layout_test.cpp -o $@

clean:
	rm -rf build
