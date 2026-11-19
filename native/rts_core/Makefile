rts_dir := native/rts_core
rts_obj := build/rts_core
rts_sources := world combat economy iso snapshot match worker_pool spatial_index stress
rts_headers := $(wildcard $(rts_dir)/*.h) $(wildcard native/navigation/*.h)
rts_lib := build/libcoastal_rts.a
rts_tests := $(addprefix $(rts_obj)/,isometric_test economy_test combat_test snapshot_test match_test worker_pool_test spatial_index_test)

.PHONY: rts-test
rts-test: $(rts_tests) $(rts_obj)/fixture.stamp
	@set -e; for program in $(filter-out $(rts_obj)/match_test,$(rts_tests)); do ./$$program; done
	$(rts_obj)/match_test $(rts_obj)/fixture

$(rts_obj): | build
	mkdir -p $@

$(rts_obj)/%.o: $(rts_dir)/%.cpp $(rts_headers) | $(rts_obj)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -O2 -Wall -Wextra -Wpedantic -c $< -o $@

$(rts_lib): $(addprefix $(rts_obj)/,$(addsuffix .o,$(rts_sources)))
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
