CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O2

.PHONY: test clean

test: build/schedule_core_test
	./build/schedule_core_test

build/schedule_core_test: tests/schedule_core_test.cpp src/ScheduleCore.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) -Isrc tests/schedule_core_test.cpp -o $@

clean:
	rm -rf build
