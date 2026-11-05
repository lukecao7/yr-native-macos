#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""run_check.py — make check 的编排器（纯 Python）。

真实重跑 core_host 两次（1t/4t 同 seed 同输入）+ compare；
真实生成篡改产物并逐个让 verify_core.py 返非 0；
读回结构化产物写 check-results.txt（命令 + 退出码 + 实测数字）。
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
os.chdir(HERE)
PY = sys.executable

NEG_CASES = [
    ("case1", ("step=", "unit="), "篡改 trace 中 1 个整数坐标"),
    ("case2", ("cell=",), "篡改 1 个 cell tile（地表/通行性校验）"),
    ("case3", ("step=",), "结论改 true 但篡改 per_step hash"),
    ("case4", ("unit=",), "篡改 snapshot 1 个单位坐标"),
    ("case5", ("threads=2",), "threads=2 假成功（应 -3）"),
    ("case6", ("padding",), "覆写 1 个 stride padding 字节"),
    ("case7", ("steps", "tick"), "宿主真实跳过 step"),
]


def run(cmd):
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True)
    return p.returncode, (p.stdout or "")


def read_json(name):
    with open(name, "r", encoding="utf-8") as f:
        return json.load(f)


def main():
    log = []
    ok = True

    # ---------- 1) 真实跑 core_host 两次 + compare ----------
    steps = []
    for args in (["1t", "1"], ["4t", "4"]):
        rc, out = run(["./core_host"] + args)
        steps.append(("./core_host " + " ".join(args), rc, out))
        if rc != 0:
            ok = False
    rc, out = run(["./core_host", "compare"])
    steps.append(("./core_host compare", rc, out))
    if rc != 0:
        ok = False

    # ---------- 2) 生成篡改产物 ----------
    rc, out = run([PY, "make_negative.py"])
    steps.append(("python3 make_negative.py", rc, out))
    if rc != 0:
        ok = False

    # ---------- 3) 正常核验（独立读回 + 重算） ----------
    rc, out = run([PY, "verify_core.py", "--dir", "."])
    steps.append(("python3 verify_core.py --dir .", rc, out))
    if rc != 0:
        ok = False
    normal_out = out

    # ---------- 4) 6 个负面测试：核验器必须返非 0 且定位 ----------
    neg_results = []
    neg_pass = 0
    for name, toks, desc in NEG_CASES:
        rc, out = run([PY, "verify_core.py", "--dir", "neg/" + name])
        located = all(t in out for t in toks)
        good = (rc == 1) and ("RESULT: FAIL" in out) and located
        neg_results.append((name, desc, rc, located, out))
        if good:
            neg_pass += 1
        else:
            ok = False

    # ---------- 5) 读回实测数字 ----------
    cons = read_json("consistency.json")
    fm = read_json("frame_meta.json")
    md = read_json("map_digest.json")
    tm1 = read_json("timing_1t.json")
    tm4 = read_json("timing_4t.json")
    w1 = float(tm1["wall_ms"])
    w4 = float(tm4["wall_ms"])
    speedup = (w1 / w4) if w4 > 0 else 0.0

    # ---------- 6) 写 check-results.txt ----------
    L = []
    L.append("P20 world-core — make check 结果（可独立复跑）")
    L.append("=" * 72)
    L.append("")
    L.append("## 实际命令与退出码")
    for cmd, rc, out in steps:
        L.append("  %-42s exit=%d" % (cmd, rc))
        for line in out.strip().splitlines():
            L.append("      | " + line)
    L.append("")
    L.append("## 实测数字")
    L.append("  unit_count            = %d" % int(cons["unit_count"]))
    L.append("  steps                 = %d" % int(cons["steps"]))
    L.append("  compared_pairs        = 360*1200 = %d" % int(cons["compared_pairs"]))
    L.append("  positions_byte_equal  = %s" % cons["positions_byte_equal"])
    L.append("  per_step_hashes_equal = %s" % cons["per_step_hashes_equal"])
    L.append("  identical (1t vs 4t)  = %s" % cons["identical"])
    L.append("  frame pixels          = %d (width=%d height=%d)" %
             (int(fm["pixels"]), int(fm["width"]), int(fm["height"])))
    L.append("  stride / padding      = %d / %d bytes per row" %
             (int(fm["stride"]), int(fm["pad_bytes"])))
    L.append("  passable/impassable   = %d / %d" %
             (int(md["passable_count"]), int(md["impassable_count"])))
    L.append("  unique_tiles          = %d" % int(md["unique_tiles"]))
    L.append("  wall_ms 1t / 4t       = %.3f / %.3f" % (w1, w4))
    L.append("  加速比 (1t/4t)         = %.3f  (4 线程此处因每步建线程开销反而更慢，不作性能宣称)" % speedup)
    L.append("")
    for line in normal_out.strip().splitlines():
        if line.startswith("METRICS"):
            L.append("  " + line)
    L.append("")
    L.append("## 负面测试（每个都必须让核验器返非 0 并定位 step/unit/cell/threads/row）")
    L.append("  通过数 = %d / %d" % (neg_pass, len(NEG_CASES)))
    for name, desc, rc, located, out in neg_results:
        marks = []
        for line in out.strip().splitlines():
            if line.startswith("FAIL:"):
                marks.append(line)
        L.append("  [%s] %s" % (name, desc))
        L.append("      verify_core.py exit=%d (要求非 0), 定位信息=%s" %
                 (rc, "有" if located else "缺失"))
        for m in marks[:3]:
            L.append("      " + m)
        if len(marks) > 3:
            L.append("      ... 共 %d 条 FAIL" % len(marks))
    L.append("")
    L.append("## 出处（ sha256 由 verify_core.py 独立重算 ）")
    L.append("  ../p5-isomap/tiles.json  sha256=%s" % md["p5_tiles_sha256"])
    L.append("  unittem-ra2.pal          sha256=%s" % md["pal768_sha256"])
    L.append("  shared/yr_slice.h        只读引用 -I../shared，未复制未修改")
    L.append("")
    L.append("## 结论")
    if ok and cons["identical"]:
        L.append("  PASS：1/4 线程 360 步 × 1200 单位逐步整数坐标逐字节一致，")
        L.append("        compared_pairs = 360*1200 = %d；%d 个负面测试全部被核验器检出。" %
                 (int(cons["compared_pairs"]), neg_pass))
    else:
        L.append("  NOT_ACHIEVED / FAIL：1/4 线程轨迹存在差异或负面测试未通过，详见上文。")
    L.append("")
    with open("check-results.txt", "w", encoding="utf-8") as f:
        f.write("\n".join(L) + "\n")

    # ---------- 7) 标准输出摘要 ----------
    print("make check: core_host 1t/4t + compare 完成")
    print("verify_core.py --dir . : exit=%d" % steps[3][1])
    for name, desc, rc, located, out in neg_results:
        print("  neg/%-6s exit=%-3d located=%s  %s" % (name, rc, located, desc))
    print("compared_pairs = %d, pixels = %d, neg_pass = %d/%d" %
          (int(cons["compared_pairs"]), int(fm["pixels"]), neg_pass, len(NEG_CASES)))
    print("RESULT: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
