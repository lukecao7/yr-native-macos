rts_dir := native/rts_core
rts_obj := build/rts_core
rts_sources := world combat economy iso snapshot match worker_pool spatial_index stress
rts_headers := $(wildcard $(rts_dir)/*.h) $(wildcard native/navigation/*.h)
rts_ai_sources := strategy scout industry tactics
rts_ai_obj := $(rts_obj)/ai
rts_3d_sources := camera scene terrain
rts_3d_obj := $(rts_obj)/view3d
rts_headers += $(wildcard native/rts_ai/*.h) $(wildcard native/rts_3d/*.h)
rts_lib := build/libcoastal_rts.a
rts_tests := $(addprefix $(rts_obj)/,isometric_test economy_test combat_test static_steps_test snapshot_test topology_test obstacle_mask_test match_test worker_pool_test spatial_index_test ai_scout_test ai_industry_test ai_tactics_test terrain3d_test camera3d_test scene3d_test)

.PHONY: rts-test
rts-test: $(rts_tests) $(rts_obj)/fixture.stamp
	@set -e; for program in $(filter-out $(rts_obj)/match_test,$(rts_tests)); do ./$$program; done
	$(rts_obj)/match_test $(rts_obj)/fixture

$(rts_obj): | build
	mkdir -p $@

$(rts_obj)/%.o: $(rts_dir)/%.cpp $(rts_headers) | $(rts_obj)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -O2 -Wall -Wextra -Wpedantic -c $< -o $@

$(rts_ai_obj): | $(rts_obj)
	mkdir -p $@

$(rts_ai_obj)/%.o: native/rts_ai/%.cpp $(rts_headers) | $(rts_ai_obj)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -O2 -Wall -Wextra -Wpedantic -c $< -o $@

$(rts_3d_obj): | $(rts_obj)
	mkdir -p $@

$(rts_3d_obj)/%.o: native/rts_3d/%.cpp $(rts_headers) | $(rts_3d_obj)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -O2 -Wall -Wextra -Wpedantic -c $< -o $@

$(rts_lib): $(addprefix $(rts_obj)/,$(addsuffix .o,$(rts_sources))) \
           $(addprefix $(rts_ai_obj)/,$(addsuffix .o,$(rts_ai_sources))) \
           $(addprefix $(rts_3d_obj)/,$(addsuffix .o,$(rts_3d_sources)))
	$(AR) rcs $@ $^

$(rts_obj)/%_test: tests/rts_%_test.cpp $(rts_lib) build/libnavigation.a $(rts_headers) | $(rts_obj)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -O2 -Wall -Wextra -Wpedantic $< $(rts_lib) build/libnavigation.a -o $@

$(rts_obj)/fixture.stamp: tests/rts_fixture_data.py tests/fixtures/coastal_rules.tsv | $(rts_obj)
	mkdir -p $(rts_obj)/fixture
	rm -f $(rts_obj)/fixture/Basin.map $(rts_obj)/fixture/coastal_rules.tsv
	python3 tests/rts_fixture_data.py $(rts_obj)/fixture
	touch $@

.PHONY: rts-stress-bench
rts-stress-bench: $(rts_obj)/stress_bench

$(rts_obj)/stress_bench: tests/rts_stress_bench.cpp $(rts_lib) build/libnavigation.a $(rts_headers) | $(rts_obj)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -O2 -Wall -Wextra -Wpedantic $< $(rts_lib) build/libnavigation.a -o $@

.PHONY: rts-ai-bench
rts-ai-bench: $(rts_obj)/ai_bench $(rts_obj)/fixture.stamp

$(rts_obj)/ai_bench: tests/rts_ai_bench.cpp $(rts_lib) build/libnavigation.a $(rts_headers) | $(rts_obj)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -O2 -Wall -Wextra -Wpedantic $< $(rts_lib) build/libnavigation.a -o $@

.PHONY: rts-worker-dispatch-bench
rts-worker-dispatch-bench: $(rts_obj)/worker_pool_dispatch_bench

$(rts_obj)/worker_pool_dispatch_bench: tests/rts_worker_pool_dispatch_bench.cpp $(rts_obj)/worker_pool.o $(rts_headers) native/profiling/thread_observation.h | $(rts_obj)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -O2 -Wall -Wextra -Wpedantic $< $(rts_obj)/worker_pool.o -o $@
