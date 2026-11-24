CXX ?= c++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -I../shared
AR ?= ar
.PHONY: all check clean world-library
all: libyrinput.a input_regression replay_host
yr_input.o: yr_input.cpp ../shared/yr_slice.h
	$(CXX) $(CXXFLAGS) -c $< -o $@
libyrinput.a: yr_input.o
	$(AR) rcs $@ $^
input_regression: input_regression.cpp mock_world.cpp libyrinput.a
	$(CXX) $(CXXFLAGS) input_regression.cpp mock_world.cpp libyrinput.a -o $@
replay_host: replay_host.cpp mock_world.cpp mini_json.hpp libyrinput.a
	$(CXX) $(CXXFLAGS) replay_host.cpp mock_world.cpp libyrinput.a -o $@
world-library:
	$(MAKE) -C ../p20-world-core libyrworld.a
world_input_regression: world_input_regression.cpp libyrinput.a world-library
	$(CXX) $(CXXFLAGS) -I../p20-world-core world_input_regression.cpp libyrinput.a ../p20-world-core/libyrworld.a -pthread -o $@
check: all world_input_regression
	./input_regression
	./world_input_regression
	./replay_host --replay replay_SELF_AUTHORED.json --out call_log.json
clean:
	rm -f yr_input.o libyrinput.a input_regression world_input_regression replay_host call_log.json
