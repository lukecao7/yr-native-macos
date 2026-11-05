#!/bin/bash
# do_check.sh — make check 的实体：全部 headless，不启动 GUI / 不抢焦点。
# 退出码 0 表示所有断言通过；写入 check-results.txt 供取证。
set -u
cd "$(dirname "$0")" || exit 1
OUT=check-results.txt
: > "$OUT"
FAIL=0
say() { echo "$@" | tee -a "$OUT"; }

say "=== P22 app-shell make check (headless; no GUI, no focus steal) ==="

# 1) plutil -lint
plutil -lint YuriSlice.app/Contents/Info.plist >>"$OUT" 2>&1
PL=$?
say "PLUTIL_LINT_EXIT=$PL (expect 0)"
[ "$PL" -eq 0 ] || FAIL=1

# 2) 抢焦点 API 源码命中数：全部编译源码 + 【离屏路径源码】独立统计（Codex 要求）
HITS=$(grep -h -E -o 'activateIgnoringOtherApps|orderFrontRegardless|makeKeyAndOrderFront' \
        *.mm *.h 2>/dev/null | wc -l | tr -d ' ')
say "FOCUS_STEAL_API_HITS=$HITS (expect 0; scope=top-level *.mm *.h)"
[ "$HITS" -eq 0 ] || FAIL=1
OHITS=$(grep -h -E -o 'activateIgnoringOtherApps|orderFrontRegardless|makeKeyAndOrderFront' \
        shell.mm shell_internal.h main.mm offscreen_test.mm 2>/dev/null | wc -l | tr -d ' ')
say "FOCUS_OFFSCREEN_PATH_API_HITS=$OHITS (expect 0; scope=shell.mm+shell_internal.h+main.mm+offscreen_test.mm)"
[ "$OHITS" -eq 0 ] || FAIL=1

# 3) nm -gU 实测导出符号（留档）
say "--- nm -gU measured yr_shell_* symbols ---"
nm -gU YuriSlice.app/Contents/MacOS/YuriSlice | grep 'yr_shell_' >>"$OUT" 2>&1

# 4) 正向核验（全部断言：三文件/可执行位/plutil/PNG 尺寸+diff/符号/sha/事件映射/抢焦点）
python3 verify_shell.py >>"$OUT" 2>&1
PV=$?
say "VERIFY_POSITIVE_EXIT=$PV (expect 0)"
[ "$PV" -eq 0 ] || FAIL=1

# 5) 负面测试：每个 --tamper 必须返非 0（被核验器真实重读/重解码拒绝）
NEG=0
NEG_DETECTED=0
for m in png symbols modifiers focus exe focus_api; do
    NEG=$((NEG + 1))
    python3 verify_shell.py --tamper "$m" >>"$OUT" 2>&1
    RC=$?
    say "TAMPER_${m}_EXIT=$RC (expect non-zero)"
    if [ "$RC" -eq 3 ] && grep -q "^TAMPER_${m}: DETECTED" "$OUT"; then
        NEG_DETECTED=$((NEG_DETECTED + 1))
    else
        FAIL=1
    fi
done
say "NEGATIVE_TESTS_TOTAL=$NEG NEGATIVE_TESTS_DETECTED=$NEG_DETECTED (expect $NEG)"
[ "$NEG_DETECTED" -eq "$NEG" ] || FAIL=1

# 6) 实测数字（从 shell_contract.json 读回，供 RUN.txt 引用）
python3 - >>"$OUT" 2>&1 <<'PY'
import json
c = json.load(open("shell_contract.json", encoding="utf-8"))
fe = c["frame_evidence"]
print("EXPORTED_SYMBOL_COUNT=%d" % len(c["exported_symbols"]))
print("DIFF_PIXEL_COUNT=%d" % fe["diff_pixel_count"])
print("PNG_A=%dx%d PNG_B=%dx%d" % (fe["width"], fe["height"], fe["width_b"], fe["height_b"]))
print("EVENT_CASE_COUNT=%d" % c["event_capture"]["case_count"])
print("FOCUS_RECORDED_HITS=%d" % c["focus"]["forbidden_api_hits"])
print("FOCUS_OFFSCREEN_Path_RECORDED_HITS=%d" % c["focus"]["offscreen_path_api_hits"])
print("ABI_HEADER_SHA256=%s" % c["abi"]["header_sha256"])
print("YR_SLICE_ABI_VERSION=%s" % c["abi"]["YR_SLICE_ABI_VERSION"])
print("FRAME_META_FRAMES=%d" % c["frame_meta"]["frame_count"])
PY

say "CHECK_FAIL=$FAIL (0 = all checks passed)"
exit "$FAIL"
