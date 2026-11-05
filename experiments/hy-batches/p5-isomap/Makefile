CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
LDLIBS ?= -lz
SANITIZERS ?= undefined,bounds

.PHONY: all run check sanitize clean

all: isomap_probe

isomap_probe: main.cpp isomap.cpp isomap.h Makefile
	$(CXX) $(CXXFLAGS) main.cpp isomap.cpp $(LDFLAGS) $(LDLIBS) -o $@

run: isomap_probe
	./isomap_probe

check: isomap_probe
	./isomap_probe --self-test

isomap_probe_san: main.cpp isomap.cpp isomap.h Makefile
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -fno-omit-frame-pointer -fsanitize=$(SANITIZERS) main.cpp isomap.cpp $(LDLIBS) -o $@

sanitize: isomap_probe_san
	./isomap_probe_san --self-test

clean:
	rm -f isomap_probe isomap_probe_san
