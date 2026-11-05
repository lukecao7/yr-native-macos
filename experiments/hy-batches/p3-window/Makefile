CXX := clang++
CXXFLAGS := -std=c++20 -fobjc-arc -Wall -Wextra -Wpedantic -Werror
LDLIBS := -framework AppKit -framework CoreGraphics -framework ImageIO
export TMPDIR := $(CURDIR)/.tmp
export CLANG_MODULE_CACHE_PATH := $(CURDIR)/.tmp/ModuleCache

.PHONY: all check clean
all: present_test

present_test: present.o present_test.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

present.o: present.mm present.h
	@mkdir -p .tmp
	$(CXX) $(CXXFLAGS) -c present.mm -o $@

present_test.o: present_test.mm present.h
	@mkdir -p .tmp
	$(CXX) $(CXXFLAGS) -c present_test.mm -o $@

check: present_test
	@printf '%s\n' 'Build: PASS (0 errors; warnings are errors).' 'Compile commands:' '$(CXX) $(CXXFLAGS) -c present.mm -o present.o' '$(CXX) $(CXXFLAGS) -c present_test.mm -o present_test.o' '$(CXX) $(CXXFLAGS) $(LDFLAGS) present.o present_test.o $(LDLIBS) -o present_test' 'Reproduce: make check' 'Run command: ./present_test' 'Compiler temporary files: .tmp/ inside this directory.' '' > RUN.txt
	@./present_test >> RUN.txt 2>&1
	@printf '\n%s\n' 'Independent dimension command: sips -g pixelWidth -g pixelHeight test_indexed.png test_rgb565.png' >> RUN.txt
	@sips -g pixelWidth -g pixelHeight test_indexed.png test_rgb565.png >> RUN.txt
	@cat RUN.txt

clean:
	rm -f present.o present_test.o present_test test_indexed.png test_rgb565.png RUN.txt
	rm -rf .tmp
