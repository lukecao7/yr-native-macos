# Build from the repository root with the Xcode Command Line Tools.
ifeq ($(shell uname -s),Darwin)

mac_host_dir := native/macos_host
mac_host_obj := build/macos_host
mac_host_headers := $(wildcard $(mac_host_dir)/*.h)
mac_host_flags := -O1 -fno-fast-math -frounding-math -ffp-contract=off -Wall -Wextra -Wpedantic
mac_host_frameworks := -framework AppKit -framework CoreGraphics -framework CoreFoundation -framework Foundation
mac_host_common := $(addprefix $(mac_host_obj)/,application.o data_directory.o frame_loop.o launch_options.o mix_archive.o)
mac_host_app := build/CoastalHost.app
mac_host_fixture_app := build/CoastalHostFixture.app
mac_host_executable := Contents/MacOS/CoastalMacHost
mac_host_tests := $(addprefix $(mac_host_obj)/,frame_loop_test launch_options_test mix_archive_test)

.PHONY: macos-host macos-host-fixture macos-host-test
macos-host: $(mac_host_app)/$(mac_host_executable)
macos-host-fixture: $(mac_host_fixture_app)/$(mac_host_executable)
macos-host-test: macos-host $(mac_host_tests)
	@set -e; for program in $(mac_host_tests); do ./$$program; done
	$(mac_host_app)/$(mac_host_executable) --help

$(mac_host_obj): | build
	mkdir -p $@

$(mac_host_obj)/%.o: $(mac_host_dir)/%.cpp $(mac_host_headers) | $(mac_host_obj)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(mac_host_flags) -c $< -o $@

$(mac_host_obj)/%.o: $(mac_host_dir)/%.mm $(mac_host_headers) | $(mac_host_obj)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(mac_host_flags) -fobjc-arc -c $< -o $@

$(mac_host_obj)/fixture.o: tests/macos_host_fixture.cpp $(mac_host_headers) | $(mac_host_obj)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(mac_host_flags) -c $< -o $@

$(mac_host_obj)/%_test: tests/macos_host_%_test.cpp $(mac_host_obj)/%.o $(mac_host_headers) | $(mac_host_obj)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(mac_host_flags) $(filter %.cpp %.o,$^) -o $@

$(mac_host_app)/$(mac_host_executable): $(mac_host_common) $(mac_host_obj)/unavailable_session.o $(mac_host_dir)/Info.plist
	mkdir -p $(mac_host_app)/Contents/MacOS
	cp $(mac_host_dir)/Info.plist $(mac_host_app)/Contents/Info.plist
	$(CXX) $(CXXFLAGS) $(filter %.o,$^) $(mac_host_frameworks) -o $@
	/usr/bin/codesign --force --sign - $(mac_host_app)

$(mac_host_fixture_app)/$(mac_host_executable): $(mac_host_common) $(mac_host_obj)/fixture.o $(mac_host_dir)/Info.plist
	mkdir -p $(mac_host_fixture_app)/Contents/MacOS
	cp $(mac_host_dir)/Info.plist $(mac_host_fixture_app)/Contents/Info.plist
	/usr/libexec/PlistBuddy -c 'Set CFBundleIdentifier org.coastalcommand.host-fixture' $(mac_host_fixture_app)/Contents/Info.plist
	$(CXX) $(CXXFLAGS) $(filter %.o,$^) $(mac_host_frameworks) -o $@
	/usr/bin/codesign --force --sign - $(mac_host_fixture_app)

endif
