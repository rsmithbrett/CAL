CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O2

.PHONY: test demo clean

test: build/schedule_core_test build/layout_test
	./build/schedule_core_test
	./build/layout_test

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

clean:
	rm -rf build
