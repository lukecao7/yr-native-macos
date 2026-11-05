CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror
.PHONY: all check clean
all: shp_selftest
shp_selftest: shp_decode.cpp shp_decode.hpp test_self_authored.cpp
	$(CXX) $(CXXFLAGS) shp_decode.cpp test_self_authored.cpp -o $@
check: shp_selftest
	./shp_selftest
clean:
	rm -f shp_selftest
	rm -rf .build
