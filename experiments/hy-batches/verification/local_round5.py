#!/usr/bin/env python3
"""第五批统一元核验：P19–P22 四个独立子对话的产物独立复跑 + 结构化读回 + 负面测试。

原则（延续前四轮）：不采信子对话自报值。每条 check 都要有独立来源：
重跑命令 / 独立读文件并重算 / 主对话自造篡改输入。
本脚本由主对话编写，不由任何子对话修改。
"""
from __future__ import annotations

import base64
import csv
import hashlib
import json
import re
import struct
import shutil
import tempfile
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "verify"))
try:
    from oracle import read_png  # 独立 PNG 解码器（filter 0-4，纯标准库）
except Exception:  # pragma: no cover
    read_png = None

rows: list[tuple[str, bool, str]] = []


def check(name: str, ok: bool, detail: str = "") -> None:
    rows.append((name, bool(ok), detail))
    print(f"{'PASS' if ok else 'FAIL'} {name}{(' — ' + detail) if detail else ''}", flush=True)


def run(cmd: list[str], cwd: Path, timeout: int = 900) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as e:
        out, err = e.stdout or "", e.stderr or ""
        if isinstance(out, bytes):
            out = out.decode(errors="replace")
        if isinstance(err, bytes):
            err = err.decode(errors="replace")
        return subprocess.CompletedProcess(cmd, -9, out, err + " [TIMEOUT]")
    except Exception as e:  # pragma: no cover
        return subprocess.CompletedProcess(cmd, -8, "", repr(e))


def jload(p: Path):
    return json.loads(p.read_text(encoding="utf-8"))


def txt(p: Path) -> str:
    return p.read_text(encoding="utf-8") if p.is_file() else ""


def png_size(p: Path) -> tuple[int, int]:
    d = p.read_bytes()
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not png")
    return struct.unpack(">II", d[16:24])


def declared_no_codex(s: str) -> bool:
    return bool(re.search(r"(未|禁止|不|没有)[^。\n]{0,16}codex\s*exec", s, re.I)) or bool(
        re.search(r"codex\s*exec[^。\n]{0,12}(未|禁止|不用|不参与)", s, re.I)
    )


def rejected(p, marker, code=1):
    """业务校验必须正常执行并明确拒绝；argparse/崩溃/超时不算检出。"""
    return p.returncode == code and marker in (p.stdout + p.stderr)


def p20_negative(p20, mode):
    artifacts = ["snapshot_1t.json", "snapshot_4t.json", "trace_1t.json", "trace_4t.json",
                 "consistency.json", "map_digest.json", "cells_baseline.bin", "frame.png",
                 "frame_rgba_stride.bin", "frame_meta.json", "threads_guard.json",
                 "order_guard.json", "init_determinism.json", "timing_1t.json",
                 "timing_4t.json", "tiles_ref.json", "unittem-ra2.pal"]
    with tempfile.TemporaryDirectory(prefix="yr5-negative-") as td:
        dest = Path(td)
        for name in artifacts:
            shutil.copyfile(p20 / name, dest / name)
        def mutate(name, fn):
            data = jload(dest / name)
            fn(data)
            (dest / name).write_text(json.dumps(data), encoding="utf-8")
        if mode == "coordinate":
            def coordinate(data):
                raw = bytearray(base64.b64decode(data["positions_le_u16_base64"]))
                raw[0] ^= 1
                data["positions_le_u16_base64"] = base64.b64encode(raw).decode()
            mutate("trace_1t.json", coordinate)
        elif mode == "tile":
            mutate("tiles_ref.json", lambda d: d["tiles"][0].update(tile=d["tiles"][0]["tile"]+1))
        elif mode == "conclusion":
            mutate("consistency.json", lambda d: d.update(identical=False))
        elif mode == "snapshot":
            mutate("snapshot_1t.json", lambda d: d["units"][0].update(x=(d["units"][0]["x"]+1)%159))
        elif mode == "threads":
            mutate("threads_guard.json", lambda d: d["cases"][0].update(return_code=0))
        elif mode == "no_step":
            # case7 由 make check 真实跳过 step 产生，保留正常 4t 对照。
            for name in artifacts:
                shutil.copyfile(p20 / "neg/case7" / name, dest / name)
        n = run([sys.executable, str(p20 / "verify_core.py"), "--dir", str(dest)], p20)
        return rejected(n, "RESULT: FAIL"), n


def main() -> int:
    # ---------------- P19：收口 47/51 ----------------
    p19 = ROOT / "p19-verify-fix"
    if "--reuse-round4" in sys.argv:
        v4 = subprocess.CompletedProcess([], 0, "读取第四轮已有报告；本次未重跑第四轮", "")
    else:
        v4 = run(["/usr/bin/python3", "verify_round4.py"], ROOT, timeout=900)
    # verify_round4.py 全通过时 exit 0；FAIL 时 exit 1，但报告仍写出
    r4 = txt(ROOT / "VERIFY-ROUND4.md")
    m = re.search(r"\*\*(\d+)/(\d+) 通过\*\*", r4)
    check("P19 历史第四轮报告读取" if "--reuse-round4" in sys.argv else "P19 verify_round4.py 重跑 exit=0", v4.returncode == 0,
          f"rc={v4.returncode} tail={(v4.stdout + v4.stderr)[-160:]}")
    check("P19 VERIFY-ROUND4.md 显示 51/51", bool(m) and m.group(1) == m.group(2) and m.group(1) == "51",
          m.group(0) if m else "no summary")
    check("P19 报告无 FAIL 行", "| FAIL |" not in r4,
          f"fail_rows={r4.count('| FAIL |')}")
    check("P19 DIFFS.md 存在且列出改动", (p19 / "DIFFS.md").is_file() and len(txt(p19 / "DIFFS.md")) > 200)
    check("P19 RUN.txt 记录 51/51", "51/51" in txt(p19 / "RUN.txt"))
    # 独立确认 P17 产物未被改动（不得为适配核验器改被核验数据）
    pb = ROOT / "p17-interactive-perf" / "perf_bench.json"
    if pb.is_file():
        nt = jload(pb).get("negative_tests")
        shape = "dict(cases)" if isinstance(nt, dict) else ("list" if isinstance(nt, list) else type(nt).__name__)
        check("P19 未改 P17 perf_bench.json 的 negative_tests 形状", shape == "dict(cases)", f"shape={shape}")
    # 独立确认核验器仍保留原有严格判据
    src = txt(ROOT / "verify_round4.py")
    check("P19 核验器保留 CSV=173 / 四类统计 / 逐像素等硬判据",
          all(k in src for k in ("len(recs) == 173", "GODOT_BOUND", "0/1440000", "PNG")))
    check("P19 核验器含 negative_cases 自检", "negative_cases" in src and "SELFTEST" in src.upper() or
          ("negative_cases" in src and "自检" in src))

    # ---------------- P20：世界核心 ----------------
    p20 = ROOT / "p20-world-core"
    b = run(["bash", "-c", "make clean all && make check"], p20, timeout=1200)
    check("P20 make clean all && make check exit=0", b.returncode == 0, (b.stdout + b.stderr)[-200:])
    check("P20 libyrworld.a 存在", (p20 / "libyrworld.a").is_file())
    t1, t4 = p20 / "trace_1t.json", p20 / "trace_4t.json"
    if t1.is_file() and t4.is_file():
        a, c = jload(t1), jload(t4)
        pa, pc = a.get("positions_le_u16_base64"), c.get("positions_le_u16_base64")
        check("P20 两份 trace 坐标串逐字节一致", pa is not None and pa == pc,
              f"len1={len(pa or '')} len4={len(pc or '')}")
        ha, hc = a.get("per_step_sha256"), c.get("per_step_sha256")
        check("P20 per_step_sha256 全等且非空", bool(ha) and ha == hc, f"steps={len(ha or [])}")
        # 独立重算：把 base64 解出来，按 step 切块重算 sha256 并与报告逐项比对
        try:
            raw = base64.b64decode(pa, validate=True)
            u, s = a.get("unit_count"), a.get("steps")
            ok = (len(raw) == u * s * 4)
            recalc = []
            for k in range(s):
                recalc.append(hashlib.sha256(raw[k * u * 4:(k + 1) * u * 4]).hexdigest())
            check("P20 独立重算坐标字节数 = steps*units*4", ok, f"bytes={len(raw)} expected={u*s*4}")
            check("P20 独立重算每步 hash 值全部一致", recalc == ha,
                  f"recalc={len(recalc)} reported={len(ha or [])}")
            pairs = u * s
            check("P20 比较对数 ≥ 1200*360", pairs >= 1200 * 360, f"pairs={pairs}")
        except Exception as e:
            check("P20 独立重算坐标", False, repr(e))
    else:
        check("P20 trace_1t/trace_4t 存在", False)
    s1, s4 = p20 / "snapshot_1t.json", p20 / "snapshot_4t.json"
    if s1.is_file() and s4.is_file():
        d, e = jload(s1), jload(s4)
        us = d.get("units", [])
        check("P20 snapshot 单位数 = 1200", len(us) == 1200, f"n={len(us)}")
        check("P20 snapshot id 严格升序", all(us[i]["id"] < us[i + 1]["id"] for i in range(len(us) - 1)))
        mw = d.get("map", {}).get("width")
        mh = d.get("map", {}).get("height")
        check("P20 snapshot 地图尺寸 = 真实 P5 159x80", (mw, mh) == (159, 80), f"{mw}x{mh}")
        check("P20 独立读回 1/4 线程 snapshot 坐标一致",
              [(x["id"], x["x"], x["y"]) for x in us] ==
              [(x["id"], x["x"], x["y"]) for x in e.get("units", [])])
    else:
        check("P20 snapshot_1t/snapshot_4t 存在", False)
    # 独立读 PNG 像素
    frame = p20 / "frame.png"
    if frame.is_file():
        try:
            w, h = png_size(frame)
            fm = jload(p20 / "frame_meta.json")
            check("P20 frame.png 与地图及 cell_scale 一致", (w, h) ==
                  (159 * fm["cell_scale"], 80 * fm["cell_scale"]) and fm["cell_scale"] > 0, f"{w}x{h}")
            if read_png:
                _, _, rgba = read_png(frame)
                nonzero = sum(1 for i in range(0, len(rgba), 4) if any(rgba[i:i + 3]))
                check("P20 独立解码 frame.png 有非黑像素", nonzero > 1000, f"nonzero={nonzero}")
        except Exception as ex:
            check("P20 frame.png 可读", False, repr(ex))
    # 主对话自造篡改输入（复制到 /tmp，不改子对话目录）
    for mode in ("coordinate", "tile", "conclusion", "snapshot", "threads"):
        if (p20 / "verify_core.py").is_file():
            good, n = p20_negative(p20, mode)
            check(f"P20 真实篡改 {mode} 被核验器拒绝", good, f"rc={n.returncode}")
    check("P20 RUN.txt 标 SELF_AUTHORED_MAPPING/ART",
          "SELF_AUTHORED" in txt(p20 / "RUN.txt"))
    check("P20 明确非原版世界/订单链", "非原版" in txt(p20 / "LIMITATIONS.md") or
          "不是原版" in txt(p20 / "LIMITATIONS.md"))

    # ---------------- P21：输入桥 ----------------
    p21 = ROOT / "p21-input-bridge"
    b = run(["bash", "-c", "make clean all && make check"], p21, timeout=600)
    check("P21 make clean all && make check exit=0", b.returncode == 0, (b.stdout + b.stderr)[-200:])
    check("P21 libyrinput.a 存在", (p21 / "libyrinput.a").is_file())
    em = p21 / "event_map.json"
    if em.is_file():
        d = jload(em)
        acts = d.get("actions", d if isinstance(d, list) else [])
        p18 = jload(ROOT / "p18-input-contract" / "input_contract.json")
        check("P21 event_map 条数 = P18 的 12", len(acts) == 12, f"n={len(acts)}")
        st = Counter(a.get("status") for a in acts)
        src_st = Counter(a.get("status") for a in p18["actions"])
        check("P21 status 分布与 P18 一致且无 IMPLEMENTED",
              st == src_st and st.get("IMPLEMENTED", 0) == 0, str(dict(st)))
        check("P21 每条含 bridge_support 且值合法", all(
            a.get("bridge_support") in ("mapped", "unsupported") for a in acts))
        check("P21 明确 mapped ≠ 原版语义对齐", "不表示" in txt(p21 / "LIMITATIONS.md") or
              "不代表" in txt(p21 / "LIMITATIONS.md"))
    else:
        check("P21 event_map.json 存在", False)
    check("P21 自检使用 SELF_AUTHORED_MOCK 并写明未链接真实核心",
          "SELF_AUTHORED_MOCK" in txt(p21 / "RUN.txt") and
          ("未与真实世界核心链接" in txt(p21 / "LIMITATIONS.md") or
           "未与真实世界核心" in txt(p21 / "RUN.txt")))
    for mode in ("type", "modifiers", "coords", "call_log", "status"):
        if (p21 / "verify_input.py").is_file():
            n = run(["/usr/bin/python3", "verify_input.py", "--tamper", mode], p21, timeout=300)
            check(f"P21 负面测试 --tamper {mode} 被拒绝", rejected(n, "verify_input: RESULT=FAIL"), f"rc={n.returncode}")

    # ---------------- P22：AppKit 宿主 ----------------
    p22 = ROOT / "p22-app-shell"
    b = run(["bash", "-c", "make clean all && make check"], p22, timeout=600)
    check("P22 make clean all && make check exit=0", b.returncode == 0, (b.stdout + b.stderr)[-200:])
    app = p22 / "YuriSlice.app"
    exe = app / "Contents/MacOS/YuriSlice"
    plist = app / "Contents/Info.plist"
    check("P22 bundle 三文件齐全且可执行位", exe.is_file() and (exe.stat().st_mode & 0o111) != 0
          and plist.is_file() and (app / "Contents/Resources/NOTICE.txt").is_file())
    pl = run(["plutil", "-lint", str(plist)], p22)
    check("P22 plutil -lint OK", pl.returncode == 0 and "OK" in (pl.stdout + pl.stderr))
    fa, fb = p22 / "frame_a.png", p22 / "frame_b.png"
    if fa.is_file() and fb.is_file() and read_png:
        try:
            wa, ha_, ra = read_png(fa)
            wb, hb, rb = read_png(fb)
            diff = sum(1 for i in range(0, min(len(ra), len(rb)), 4) if ra[i:i + 4] != rb[i:i + 4])
            check("P22 两帧尺寸一致且 ≥1280×720", (wa, ha_) == (wb, hb) and wa >= 1280 and ha_ >= 720,
                  f"{wa}x{ha_} vs {wb}x{hb}")
            check("P22 独立逐像素比较 A/B 确有差异", diff > 0, f"diff_pixels={diff}")
        except Exception as ex:
            check("P22 独立解码两帧", False, repr(ex))
    else:
        check("P22 frame_a/frame_b 存在且解码器可用", False)
    sc = p22 / "shell_contract.json"
    if sc.is_file() and exe.is_file():
        d = jload(sc)
        syms = d.get("exported_symbols", d.get("symbols", []))
        nm = run(["nm", "-gU", str(exe)], p22)
        blob = nm.stdout + nm.stderr
        check("P22 shell_contract 导出符号与 nm -gU 实测一致",
              bool(syms) and all(s in blob for s in syms), f"declared={syms}")
    else:
        check("P22 shell_contract.json 存在", False)
    mm = p22 / "main.mm"
    if mm.is_file():
        src = txt(mm)
        check("P22 离屏路径未使用抢焦点 API",
              "activateIgnoringOtherApps" not in src and "orderFrontRegardless" not in src)
    # 模式名按 P22 实际实现（png/symbols/modifiers/focus/exe），不改产物去适配核验器
    for mode in ("png", "symbols", "modifiers", "focus", "exe"):
        if (p22 / "verify_shell.py").is_file():
            n = run(["/usr/bin/python3", "verify_shell.py", "--tamper", mode], p22, timeout=300)
            check(f"P22 负面测试 --tamper {mode} 被拒绝", rejected(n, f"TAMPER_{mode}: DETECTED", 3), f"rc={n.returncode}")
    check("P22 明确真实窗口未经人工双击验证",
          "人工" in txt(p22 / "LIMITATIONS.md") or "未验证" in txt(p22 / "LIMITATIONS.md"))

    # ---------------- Codex 第 4 次评审要求的新增证据 ----------------
    ABI_SHA = "917a84ec8902a51038285a32344970a7aa2c831e31ca794c700e4a38b8365660"
    actual_sha = hashlib.sha256((ROOT / "shared" / "yr_slice.h").read_bytes()).hexdigest()
    check("共享 ABI 头未被子对话改动", actual_sha == ABI_SHA, f"sha={actual_sha[:16]}…")
    hdr = txt(ROOT / "shared" / "yr_slice.h")
    check("共享 ABI 头含版本宏与语义说明", "YR_SLICE_ABI_VERSION 1" in hdr and
          "逻辑 tick" in hdr and "缓冲区归属调用方" in hdr)
    for name in ("P20", "P21", "P22"):
        d = {"P20": p20, "P21": p21, "P22": p22}[name]
        r = txt(d / "RUN.txt")
        check(f"{name} RUN.txt 记录 ABI 版本与定稿哈希", ABI_SHA in r and
              ("YR_SLICE_ABI_VERSION=1" in r or "ABI_VERSION 1" in r),
              f"hash_in_run={ABI_SHA in r}")

    # P19：更正后的收口要求
    check("P19 P14 LIMITATIONS.md 补真实执行声明",
          declared_no_codex(txt(ROOT / "p14-app-host" / "LIMITATIONS.md")))
    check("P19 DIFFS 明确「补记录≠独立合规证明」",
          "补记录" in txt(p19 / "DIFFS.md") and
          ("不是独立合规证明" in txt(p19 / "DIFFS.md") or "不是独立" in txt(p19 / "DIFFS.md")))
    check("P19 核验器收紧抢焦点判据为源码 grep",
          "activateIgnoringOtherApps" in txt(ROOT / "verify_round4.py"))
    check("P19 报告区分四类证据", "共用算法复查" in r4 or "四类证据" in r4)
    check("P19 报告 IsoMapPack5 措辞准确", "未解码建世界" in r4)

    # P20：地图证据 / 实体注册 / 停止测试
    me = p20 / "map_evidence.json"
    if me.is_file():
        e = jload(me)
        check("P20 map_evidence 含输入 sha256 与 12720 cell 0 不一致",
              len(str(e.get("input_sha256", ""))) == 64 and
              ("12720" in json.dumps(e) or e.get("cell_count") == 12720) and
              ("0" == str(e.get("inconsistent_cells", "?")) or e.get("inconsistent_cells") == 0),
              f"keys={list(e)[:8]}")
        check("P20 map_evidence 显式 SELF_AUTHORED_MAPPING 非原版规则",
              "SELF_AUTHORED_MAPPING" in json.dumps(e))
    else:
        check("P20 map_evidence.json 存在", False)
    check("P20 有实体注册证据", (p20 / "entity_registry.json").is_file() or
          "entity_registry" in txt(p20 / "RUN.txt"))
    if (p20 / "verify_core.py").is_file():
        good, n = p20_negative(p20, "no_step")
        check("P20 真实停止 step 被检出", good, f"rc={n.returncode}")

    # P21：Windows 对照缺口 / 停止测试
    if em.is_file():
        e = jload(em)
        check("P21 顶层 windows_reference_available=false", e.get("windows_reference_available") is False)
        check("P21 顶层 mapping_is_self_authored=true", e.get("mapping_is_self_authored") is True)
    if (p21 / "verify_input.py").is_file():
        n = run(["/usr/bin/python3", "verify_input.py", "--tamper", "no_dispatch"], p21, timeout=300)
        check("P21 停止测试 --tamper no_dispatch 被拒绝", rejected(n, "verify_input: RESULT=FAIL"), f"rc={n.returncode}")

    # P22：帧关联钩子 / 抢焦点源码级证据
    if sc.is_file():
        d = jload(sc)
        fc = d.get("frame_correlation", {})
        need = ("frame_index", "tick", "event_seq", "entity_ids", "order_seq")
        check("P22 frame_correlation 覆盖 5 个关联字段",
              all(k in str(fc) for k in need), f"keys={list(fc) if isinstance(fc, dict) else type(fc).__name__}")
    check("P22 frame_meta.json 存在并标注 mock 值",
          (p22 / "frame_meta.json").is_file() and
          ("mock" in txt(p22 / "frame_meta.json").lower() or "MOCK" in txt(p22 / "frame_meta.json")))
    check("P22 check-results.txt 有抢焦点 API 命中数", "命中" in txt(p22 / "check-results.txt") or
          "focus" in txt(p22 / "check-results.txt").lower())
    if (p22 / "verify_shell.py").is_file():
        n = run(["/usr/bin/python3", "verify_shell.py", "--tamper", "focus"], p22, timeout=300)
        check("P22 抢焦点注入负面测试 --tamper focus 被拒绝", rejected(n, "TAMPER_focus: DETECTED", 3), f"rc={n.returncode}")

    # ---------------- 措辞与纪律合规（P19–P22） ----------------
    for name, d in [("P19", p19), ("P20", p20), ("P21", p21), ("P22", p22)]:
        lim = txt(d / "LIMITATIONS.md")
        check(f"{name} 声明目标 1/2 未达成", ("目标 1" in lim and "目标 2" in lim))
        check(f"{name} 有 NOT_ACHIEVED 清单", "NOT_ACHIEVED" in lim)
        check(f"{name} RUN.txt 声明未用 Codex CLI 开发", declared_no_codex(txt(d / "RUN.txt")))

    passed = sum(1 for _, ok, _ in rows if ok)
    out = ["# 第五批统一元核验报告", "",
           "四个独立子对话（P19 收口 / P20 世界核心 / P21 输入桥 / P22 AppKit 宿主）。",
           "不采信自报值：主对话重跑各路 check、独立读结构化产物并重算、自造篡改输入验证负面测试。", "",
           "| 检查 | 结果 | 说明 |", "|---|---|---|"]
    for n, ok, detail in rows:
        out.append(f"| {n} | {'PASS' if ok else 'FAIL'} | {str(detail).replace('|', '/')[:280]} |")
    out += ["", f"**{passed}/{len(rows)} 通过**", "",
            "边界：四路均为局部组件验证，**目标 1 / 目标 2 仍未达成**；",
            "整套源码是「重建 + 取证校验 + 独立模拟」，非链接原版 exe；",
            "本批尚未把三组件链接进同一可执行，也不含长期帧间隔实测（属第六批）。"]
    (ROOT / "VERIFY-ROUND5.md").write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"\n{passed}/{len(rows)} 通过 → {ROOT / 'VERIFY-ROUND5.md'}", flush=True)
    return 0 if passed == len(rows) else 1


if __name__ == "__main__":
    sys.exit(main())
