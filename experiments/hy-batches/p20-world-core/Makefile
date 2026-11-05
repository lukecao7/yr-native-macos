# P20 world-core — 用 /usr/bin/clang++ -std=c++20 -O2 -pthread 编译，/usr/bin/ar rcs 建静态库。
# 共享头文件只读引用 -I../shared，不复制、不修改。
CXX := /usr/bin/clang++
AR := /usr/bin/ar
CXXFLAGS := -std=c++20 -O2 -pthread -I../shared -Wall -Wextra
LDFLAGS := -pthread

.PHONY: all clean check

all: libyrworld.a core_host

yr_util.o: yr_util.cpp yr_util.h
	$(CXX) $(CXXFLAGS) -c yr_util.cpp -o yr_util.o

yr_world.o: yr_world.cpp yr_util.h ../shared/yr_slice.h
	$(CXX) $(CXXFLAGS) -c yr_world.cpp -o yr_world.o

core_host.o: core_host.cpp yr_util.h ../shared/yr_slice.h
	$(CXX) $(CXXFLAGS) -c core_host.cpp -o core_host.o

libyrworld.a: yr_world.o yr_util.o
	$(AR) rcs $@ yr_world.o yr_util.o

core_host: core_host.o libyrworld.a
	$(CXX) $(CXXFLAGS) core_host.o libyrworld.a -o core_host $(LDFLAGS)

world_regression: world_regression.cpp libyrworld.a ../shared/yr_slice.h
	$(CXX) $(CXXFLAGS) world_regression.cpp libyrworld.a -o $@ $(LDFLAGS)

check: all world_regression
	./world_regression
	@/usr/bin/python3 run_check.py

clean:
	rm -f world_regression regression_grid.json
	rm -f yr_util.o yr_world.o core_host.o libyrworld.a core_host
	rm -f snapshot_1t.json snapshot_4t.json trace_1t.json trace_4t.json
	rm -f consistency.json map_digest.json cells_baseline.bin
	rm -f frame.png frame_rgba_stride.bin frame_meta.json
	rm -f threads_guard.json order_guard.json init_determinism.json
	rm -f timing_1t.json timing_4t.json tiles_ref.json check-results.txt
	rm -rf neg
