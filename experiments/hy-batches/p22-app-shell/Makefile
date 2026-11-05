# P22 app-shell — Makefile
# 编译：/usr/bin/clang++ -std=c++17 -fobjc-arc -O2 -Wall -Wextra -Wpedantic
# 链接：AppKit / CoreGraphics / ImageIO / CoreFoundation / Foundation
# 判据命令：make clean all && make check   (必须 exit 0)
#
# 安全约束：check 只跑 headless 自检与静态 grep，绝不启动 GUI / 抢焦点。
CXX      := /usr/bin/clang++
CXXFLAGS := -std=c++17 -fobjc-arc -O2 -Wall -Wextra -Wpedantic -I../shared
AR       := /usr/bin/ar
FW       := -framework AppKit -framework CoreGraphics -framework ImageIO \
            -framework CoreFoundation -framework Foundation
PY       := python3

APP  := YuriSlice.app
EXE  := $(APP)/Contents/MacOS/YuriSlice
LIB  := libyrshell.a

.PHONY: all check clean

all: $(EXE) offscreen_test event_capture_test
	@mkdir -p $(APP)/Contents/MacOS $(APP)/Contents/Resources
	@cp Info.plist $(APP)/Contents/Info.plist
	@cp NOTICE.txt $(APP)/Contents/Resources/NOTICE.txt
	@echo "== [1/4] offscreen self-test (SELF_AUTHORED_MOCK) =="
	./offscreen_test
	@echo "== [2/4] offscreen write failures (headless child processes) =="
	$(PY) test_export_io.py $(EXE)
	@echo "== [3/4] event capture self-test (SELF_AUTHORED_MOCK) =="
	./event_capture_test
	@echo "== [4/4] generate shell_contract.json (nm -gU measured) =="
	$(PY) verify_shell.py --gen
	@echo "ALL_DONE"

%.o: %.mm shell_internal.h ../shared/yr_slice.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(LIB): shell.o window_path.o
	$(AR) rcs $@ $^

$(EXE): main.o $(LIB)
	@mkdir -p $(APP)/Contents/MacOS $(APP)/Contents/Resources
	$(CXX) $(CXXFLAGS) main.o $(LIB) $(FW) -o $@

offscreen_test: offscreen_test.o $(LIB)
	$(CXX) $(CXXFLAGS) offscreen_test.o $(LIB) $(FW) -o $@

event_capture_test: event_capture_test.o $(LIB)
	$(CXX) $(CXXFLAGS) event_capture_test.o $(LIB) $(FW) -o $@

check: all
	@bash do_check.sh

clean:
	rm -f *.o $(LIB) offscreen_test event_capture_test
	rm -f frame_a.png frame_b.png frame_default.png
	rm -f event_capture.json shell_contract.json frame_meta.json check-results.txt
	rm -rf $(APP)
