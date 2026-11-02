CXX ?= c++
CPPFLAGS += -Inative
CXXFLAGS += -std=c++20 -pthread
AR ?= ar

sources := native/navigation/flow_cache.cpp native/navigation/nearest_walkable.cpp native/navigation/reachability.cpp
tests := flow_cache_test flow_repair_test navigation_reachability_test nearest_walkable_test profiling_obstacle_probe_test profiling_recorder_test
objects := $(patsubst native/navigation/%.cpp,build/%.o,$(sources))
test_bins := $(addprefix build/,$(tests))
headers := $(wildcard native/navigation/*.h)

.PHONY: all check test
all: build/libnavigation.a

build:
	mkdir -p $@

build/%.o: native/navigation/%.cpp $(headers) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

build/libnavigation.a: $(objects)
	$(AR) rcs $@ $^

build/%_test: tests/%_test.cpp build/libnavigation.a $(headers) $(wildcard native/profiling/*.h) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< build/libnavigation.a -o $@

check:
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fsyntax-only $(sources) $(addprefix tests/,$(addsuffix .cpp,$(tests)))

test: $(test_bins)
	@set -e; for program in $(test_bins); do ./$$program; done
