CXX ?= clang++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror
.PHONY: all check clean
all: probe
probe: mix_probe.cpp
	$(CXX) $(CXXFLAGS) $< -o $@
check: probe
	python3 verify_probe.py --probe ./probe --self-authored-only
clean:
	rm -f probe
