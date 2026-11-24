# Build from the repository root with the Xcode Command Line Tools.
ifeq ($(shell uname -s),Darwin)

mac_host_dir := native/macos_host
mac_host_obj := build/macos_host
mac_host_headers := $(wildcard $(mac_host_dir)/*.h)
mac_host_flags := -O1 -fno-fast-math -frounding-math -ffp-contract=off -Wall -Wextra -Wpedantic
mac_host_frameworks := -framework AppKit -framework CoreGraphics -framework CoreFoundation -framework Foundation -framework Metal -framework QuartzCore -framework CoreVideo
mac_host_common := $(addprefix $(mac_host_obj)/,application.o data_directory.o frame_delivery.o frame_image.o frame_loop.o input_view.o launch_options.o metal_presenter.o metal_scene.o mix_archive.o presented_session.o)
mac_host_app := build/CoastalHost.app
mac_host_fixture_common := $(filter-out $(mac_host_obj)/application.o,$(mac_host_common)) $(mac_host_obj)/application_fixture.o $(mac_host_obj)/script_replay.o
mac_host_fixture_app := build/CoastalHostFixture.app
mac_host_executable := Contents/MacOS/CoastalMacHost
mac_host_tests := $(addprefix $(mac_host_obj)/,data_directory_test frame_delivery_test frame_image_test frame_loop_test input_view_test launch_options_test mix_archive_test presented_session_test main_signal_test)

.PHONY: macos-host macos-host-fixture macos-host-test macos-host-script-replay-test
macos-host: $(mac_host_app)/$(mac_host_executable)
macos-host-fixture: $(mac_host_fixture_app)/$(mac_host_executable)
macos-host-script-replay-test: macos-host-fixture
	python3 tests/macos_host_script_replay_test.py $(mac_host_fixture_app)/$(mac_host_executable)
macos-host-test: macos-host $(mac_host_tests)
	@set -e; for program in $(mac_host_tests); do ./$$program; done
	$(mac_host_app)/$(mac_host_executable) --help

$(mac_host_obj): | build
	mkdir -p $@

$(mac_host_obj)/%.o: $(mac_host_dir)/%.cpp $(mac_host_headers) | $(mac_host_obj)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(mac_host_flags) -c $< -o $@

$(mac_host_obj)/%.o: $(mac_host_dir)/%.mm $(mac_host_headers) | $(mac_host_obj)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(mac_host_flags) -fobjc-arc -c $< -o $@

$(mac_host_obj)/engine_session.o $(mac_host_obj)/metal_scene.o $(mac_host_obj)/metal_presenter.o $(mac_host_obj)/frame_delivery.o $(mac_host_obj)/presented_session.o: $(rts_headers)

$(mac_host_obj)/application_fixture.o: $(mac_host_dir)/application.mm $(mac_host_headers) | $(mac_host_obj)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(mac_host_flags) -DCOASTAL_FIXTURE_REPLAY -fobjc-arc -c $< -o $@

$(mac_host_obj)/fixture.o: tests/macos_host_fixture.cpp $(mac_host_headers) | $(mac_host_obj)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(mac_host_flags) -c $< -o $@

$(mac_host_obj)/%_test: tests/macos_host_%_test.cpp $(mac_host_obj)/%.o $(mac_host_headers) | $(mac_host_obj)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(mac_host_flags) $(filter %.cpp %.o,$^) $(LDLIBS) -o $@

$(mac_host_obj)/frame_image_test: LDLIBS += -framework CoreGraphics -framework CoreFoundation

$(mac_host_obj)/frame_delivery_test: tests/macos_host_frame_delivery_test.cpp $(mac_host_obj)/frame_delivery.o $(mac_host_obj)/frame_loop.o $(mac_host_headers) $(rts_headers) | $(mac_host_obj)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(mac_host_flags) $(filter %.cpp %.o,$^) $(LDLIBS) -o $@

$(mac_host_obj)/data_directory_test.plist: $(mac_host_dir)/Info.plist | $(mac_host_obj)
	cp $< $@
	/usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier org.coastalcommand.host-data-directory-test.$$(uuidgen)" $@

$(mac_host_obj)/data_directory_test: tests/macos_host_data_directory_test.mm $(mac_host_obj)/data_directory.o $(mac_host_obj)/launch_options.o $(mac_host_headers) $(mac_host_obj)/data_directory_test.plist | $(mac_host_obj)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(mac_host_flags) -fobjc-arc $(filter %.mm %.o,$^) \
		-Wl,-sectcreate,__TEXT,__info_plist,$(filter %.plist,$^) $(mac_host_frameworks) -o $@

$(mac_host_obj)/input_view_test: tests/macos_host_input_view_test.mm $(mac_host_obj)/input_view.o $(mac_host_headers) | $(mac_host_obj)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(mac_host_flags) -fobjc-arc $(filter %.mm %.o,$^) $(mac_host_frameworks) -o $@

$(mac_host_app)/$(mac_host_executable): $(mac_host_common) $(mac_host_obj)/engine_session.o $(rts_lib) build/libnavigation.a $(mac_host_dir)/Info.plist
	mkdir -p $(mac_host_app)/Contents/MacOS
	cp $(mac_host_dir)/Info.plist $(mac_host_app)/Contents/Info.plist
	$(CXX) $(CXXFLAGS) $(filter %.o %.a,$^) $(mac_host_frameworks) -o $@
	/usr/bin/codesign --force --sign - $(mac_host_app)

$(mac_host_fixture_app)/$(mac_host_executable): $(mac_host_fixture_common) $(mac_host_obj)/fixture.o $(mac_host_dir)/Info.plist
	mkdir -p $(mac_host_fixture_app)/Contents/MacOS
	cp $(mac_host_dir)/Info.plist $(mac_host_fixture_app)/Contents/Info.plist
	/usr/libexec/PlistBuddy -c 'Set CFBundleIdentifier org.coastalcommand.host-fixture' $(mac_host_fixture_app)/Contents/Info.plist
	$(CXX) $(CXXFLAGS) $(filter %.o,$^) $(mac_host_frameworks) -framework ImageIO -o $@
	/usr/bin/codesign --force --sign - $(mac_host_fixture_app)

$(mac_host_obj)/main_signal_test: tests/macos_host_main_signal_test.mm $(mac_host_dir)/application.mm $(filter-out $(mac_host_obj)/application.o,$(mac_host_common)) $(mac_host_obj)/engine_session.o $(rts_lib) build/libnavigation.a $(mac_host_headers) $(rts_headers) | $(mac_host_obj)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(mac_host_flags) -fobjc-arc $< $(filter %.o %.a,$^) $(mac_host_frameworks) -o $@

endif
