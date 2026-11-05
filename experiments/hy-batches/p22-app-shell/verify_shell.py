#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_shell.py — P22 app-shell 独立核验器（纯标准库）

三种模式：
  python3 verify_shell.py                  : 正向核验真实产物，全部通过返回 0
  python3 verify_shell.py --gen            : 由真实产物生成 shell_contract.json
  python3 verify_shell.py --tamper <mode> : 复制全部产物到临时目录、施加篡改、
                                            重新核验；核验器【真实重读文件 / 重解码 PNG】
                                            并拒绝被篡改数据 -> 返回非 0

--tamper 模式（每个都必须返非 0）：
  png        解码 frame_b.png、改 1 个像素、重编码（真实 PNG 解码/编码）
  symbols    篡改 shell_contract.json 的导出符号
  modifiers  篡改 event_capture.json 的 modifiers
  focus      源码注入被禁抢焦点 API 且伪造「未抢焦点」结论
  focus_api  向临时副本的【离屏路径源码】注入抢焦点调用（独立源码级 grep 必须检出）
  exe        删除 bundle 里的可执行文件

核验器不采信任何「自报结论」：PNG 用自写纯标准库解码器逐像素重解码，
符号用 nm -gU 实测重读，plist 用 plutil 重读；
抢焦点 API 用【独立源码级 grep】实际统计全部编译源码 + 离屏路径源码的命中数
（不靠「文档里写了没用」当证据）；共享 ABI 头真实重读 sha256 / 版本宏；
frame_correlation / frame_meta.json 结构化重读；事件映射用独立映射表重算。
任何一项造假都会被真实复算定位并拒绝。
"""
import hashlib
import json
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import zlib
import glob

HERE = os.path.dirname(os.path.abspath(__file__))
FORBIDDEN = ["activateIgnoringOtherApps", "orderFrontRegardless", "makeKeyAndOrderFront"]
TAMPER_MODES = ["png", "symbols", "modifiers", "focus", "exe", "focus_api"]

# 离屏核验路径源码集合（Codex 要求：独立源码级 grep，不采信文档自述）。
# 这些文件构成 yr_shell_render_offscreen_png / 合成器 / 驱动自检的离屏路径。
OFFSCREEN_PATH_SOURCES = ["shell.mm", "shell_internal.h", "main.mm", "offscreen_test.mm"]

# 共享 ABI 头由主对话维护；用绝对路径只读引用，绝不复制 / 改动。
SHARED_HDR = os.path.normpath(os.path.join(HERE, "..", "shared", "yr_slice.h"))

NS_TYPE_TO_YR = {
    "LeftMouseDown": 1, "LeftMouseUp": 2, "LeftMouseDragged": 3,
    "RightMouseDown": 4, "RightMouseUp": 5, "KeyDown": 6, "KeyUp": 7,
}
MOD_BITS = {"shift": 1, "control": 2, "alt": 4}

PLIST_KEYS = ["CFBundleExecutable", "CFBundleIdentifier", "CFBundleName",
              "CFBundlePackageType", "CFBundleShortVersionString", "CFBundleVersion",
              "LSMinimumSystemVersion", "NSPrincipalClass", "NSHighResolutionCapable"]

BUNDLE_FILES = ["Contents/Info.plist", "Contents/MacOS/YuriSlice",
                "Contents/Resources/NOTICE.txt"]

# 帧关联预留证据路径（Codex 评审：事件 / 实体 ID / 订单序号 / tick / 显示帧须可关联）。
# 本批为 SELF_AUTHORED_MOCK，这些是第六批集成的预留字段，不是已证实的真实关联。
FRAME_CORRELATION = {
    "status": "RESERVED_FOR_ROUND6_SELF_AUTHORED_MOCK",
    "verified_real_correlation": False,
    "note": ("本批为 SELF_AUTHORED_MOCK；下列字段是第六批集成的预留证据路径，不是已证实的真实关联。"
             "本批没有一次真实运行把事件→实体→订单→tick→显示帧串起来（NOT_ACHIEVED，属第六批 P23）。"),
    "fields": {
        "frame_index": {
            "source": "YRShellComposeFrame 每次成功调用单调递增；本批由 frame_meta.json 记录",
            "round6_path": "shell 帧计数器 → frame_meta.json → 与 provider 调用序号对齐",
        },
        "tick": {
            "source": "yr_world_tick(w) / snapshot.tick（逻辑 tick，非墙钟毫秒）",
            "round6_path": "集成器注入的 yr_world_render provider 读世界 tick 并随帧写入 frame_meta",
        },
        "event_seq": {
            "source": "event_sink 收到的 yr_event 序号（输入桥）",
            "round6_path": "yr_event 序号 → 集成器记录到 frame_meta",
        },
        "entity_ids": {
            "source": "yr_world_snapshot_json().units[].id / 选中单位 ID",
            "round6_path": "受影响 / 选中实体 ID 列表随帧写入 frame_meta",
        },
        "order_seq": {
            "source": "yr_world_order_move 调用序号 / 输入桥订单计数",
            "round6_path": "订单序号随帧写入 frame_meta",
        },
    },
    "mock_values_file": "frame_meta.json",
    "round6_requirement": ("同一真实运行内：输入事件 → 世界状态变化 → 帧像素变化，且 "
                           "event_seq/order_seq/tick/entity_ids/frame_index 可关联"
                           "（snapshot diff + 逐像素 diff 成对证据）"),
}


# --------------------------------------------------------------------------
# 基础工具
# --------------------------------------------------------------------------
def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def abi_info():
    """真实读取 ../shared/yr_slice.h，返回 sha256 与版本宏（不采信任何自报值）。"""
    with open(SHARED_HDR, "rb") as f:
        raw = f.read()
    txt = raw.decode("utf-8", "replace")
    m = re.search(r"#define\s+YR_SLICE_ABI_VERSION\s+(\d+)", txt)
    ms = re.search(r'#define\s+YR_SLICE_ABI_VERSION_STRING\s+"([^"]*)"', txt)
    return {
        "sha256": hashlib.sha256(raw).hexdigest(),
        "version": int(m.group(1)) if m else None,
        "version_string": ms.group(1) if ms else None,
    }


def nm_symbols(exe):
    """nm -gU 实测：返回可执行里定义的 yr_shell_* 全局符号名集合。"""
    try:
        out = subprocess.check_output(["nm", "-gU", exe], text=True,
                                      stderr=subprocess.DEVNULL)
    except Exception:
        return set()
    syms = set()
    for line in out.splitlines():
        parts = line.split()
        if not parts:
            continue
        name = parts[-1]
        if "yr_shell_" in name:
            syms.add(name)
    return syms


def plutil_json(path):
    out = subprocess.check_output(["plutil", "-convert", "json", "-o", "-", path],
                                  text=True, stderr=subprocess.DEVNULL)
    return json.loads(out)


def _count_in_files(paths):
    hits = 0
    for fp in paths:
        try:
            with open(fp, "r", encoding="utf-8", errors="replace") as f:
                txt = f.read()
        except OSError:
            continue
        for pat in FORBIDDEN:
            hits += txt.count(pat)
    return hits


def count_forbidden(root):
    """独立重 grep：扫描全部编译源码（顶层 *.mm / *.h）命中数。"""
    files = sorted(glob.glob(os.path.join(root, "*.mm")) +
                   glob.glob(os.path.join(root, "*.h")))
    return _count_in_files(files)


def count_forbidden_offscreen(root):
    """独立重 grep：仅【离屏核验路径源码】命中数（Codex 要求）。"""
    paths = [os.path.join(root, n) for n in OFFSCREEN_PATH_SOURCES]
    return _count_in_files(paths)


# --------------------------------------------------------------------------
# 纯标准库 PNG 解码器 / 编码器（zlib + PNG filter 0..4）
# --------------------------------------------------------------------------
def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def decode_png(path):
    """返回 (width, height, rows)；rows 为每行 RGBA(width*4) 的 bytes 列表。"""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("bad PNG signature")
    pos = 8
    ihdr = None
    idat = bytearray()
    while pos + 8 <= len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        ctype = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", chunk)
        elif ctype == b"IDAT":
            idat.extend(chunk)
        elif ctype == b"IEND":
            break
    if ihdr is None:
        raise ValueError("missing IHDR")
    width, height, bit_depth, color_type, comp, filt, interlace = ihdr
    if bit_depth != 8 or color_type not in (2, 6) or interlace != 0 or comp != 0 or filt != 0:
        raise ValueError("unsupported PNG (need 8-bit RGB/RGBA, non-interlaced)")
    bpp = 3 if color_type == 2 else 4
    row_bytes = width * bpp
    raw = zlib.decompress(bytes(idat))
    if len(raw) < height * (row_bytes + 1):
        raise ValueError("short IDAT")
    prev = bytearray(row_bytes)
    rows = []
    p = 0
    for _ in range(height):
        ft = raw[p]
        p += 1
        line = bytearray(raw[p:p + row_bytes])
        p += row_bytes
        for i in range(row_bytes):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ft == 0:
                v = line[i]
            elif ft == 1:
                v = line[i] + a
            elif ft == 2:
                v = line[i] + b
            elif ft == 3:
                v = line[i] + ((a + b) >> 1)
            elif ft == 4:
                v = line[i] + _paeth(a, b, c)
            else:
                raise ValueError("bad filter %d" % ft)
            line[i] = v & 0xFF
        prev = line
        rows.append(bytes(line))
    if color_type == 2:  # RGB -> RGBA 归一化
        rows = [b"".join(r[i:i + 3] + b"\xff" for i in range(0, len(r), 3))
                for r in rows]
    return width, height, rows


def png_encode(width, height, rows):
    """rows: 每行 RGBA(width*4) bytes；输出 filter=0 / RGBA / 8-bit 的合法 PNG。"""
    raw = bytearray()
    for r in rows:
        raw.append(0)
        raw.extend(r)

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xffffffff))

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def pixel_diff(rows_a, rows_b):
    """逐像素（RGBA 4 字节）比较，返回不同像素数。"""
    if len(rows_a) != len(rows_b):
        return -1
    diff = 0
    for ra, rb in zip(rows_a, rows_b):
        if len(ra) != len(rb):
            return -1
        for i in range(0, len(ra), 4):
            if (ra[i] != rb[i] or ra[i + 1] != rb[i + 1] or
                    ra[i + 2] != rb[i + 2] or ra[i + 3] != rb[i + 3]):
                diff += 1
    return diff


def distinct_colors(rows):
    seen = set()
    for r in rows[:40]:
        for i in range(0, len(r), 4):
            seen.add(r[i:i + 4])
            if len(seen) > 1:
                return len(seen)
    return len(seen)


# --------------------------------------------------------------------------
# --gen：由真实产物生成 shell_contract.json
# --------------------------------------------------------------------------
def cmd_gen():
    app = os.path.join(HERE, "YuriSlice.app")
    exe = os.path.join(app, "Contents", "MacOS", "YuriSlice")
    info = os.path.join(app, "Contents", "Info.plist")
    syms = sorted(nm_symbols(exe))

    bundle_files = []
    for rel in BUNDLE_FILES:
        ap = os.path.join(app, rel)
        bundle_files.append({
            "path": rel,
            "sha256": sha256_file(ap),
            "executable": bool(os.stat(ap).st_mode & stat.S_IXUSR),
            "size": os.path.getsize(ap),
        })

    pl = plutil_json(info)
    info_plist = {k: pl.get(k) for k in PLIST_KEYS}

    fa = os.path.join(HERE, "frame_a.png")
    fb = os.path.join(HERE, "frame_b.png")
    wa, ha, ra = decode_png(fa)
    wb, hb, rb = decode_png(fb)
    diff = pixel_diff(ra, rb)
    hits = count_forbidden(HERE)
    off_hits = count_forbidden_offscreen(HERE)
    abi = abi_info()

    with open(os.path.join(HERE, "event_capture.json"), encoding="utf-8") as f:
        ec = json.load(f)
    with open(os.path.join(HERE, "frame_meta.json"), encoding="utf-8") as f:
        fm = json.load(f)

    contract = {
        "component": "p22-app-shell",
        "bundle": "YuriSlice.app",
        "abi": {
            "header": "../shared/yr_slice.h",
            "header_sha256": abi["sha256"],
            "YR_SLICE_ABI_VERSION": abi["version"],
            "YR_SLICE_ABI_VERSION_STRING": abi["version_string"],
            "reference": "-I../shared 只读引用；不复制进本目录、不改动",
        },
        "notice": ("SELF_AUTHORED_MOCK 仅用于自检；未与真实世界核心 / 输入桥链接，将在第六批集成；"
                   "整套源码是重建 + 取证校验 + 独立模拟，不是链接原版 exe"),
        "frame_correlation": FRAME_CORRELATION,
        "frame_meta": {"file": "frame_meta.json", "frame_count": len(fm["frames"])},
        "exported_symbols": syms,
        "exported_symbol_count": len(syms),
        "nm_command": "nm -gU YuriSlice.app/Contents/MacOS/YuriSlice",
        "bundle_files": bundle_files,
        "info_plist": info_plist,
        "frame_evidence": {
            "frame_a": "frame_a.png",
            "frame_b": "frame_b.png",
            "frame_a_sha256": sha256_file(fa),
            "frame_b_sha256": sha256_file(fb),
            "width": wa, "height": ha, "width_b": wb, "height_b": hb,
            "diff_pixel_count": diff,
            "decoder": "pure-stdlib PNG (zlib + unfilter), RGBA normalize",
            "note": "frame_a / frame_b 来自注入的 SELF_AUTHORED_MOCK provider 的两个不同状态",
        },
        "focus": {
            "forbidden_apis": FORBIDDEN,
            "forbidden_api_hits": hits,
            "offscreen_path_api_hits": off_hits,
            "offscreen_path_sources": OFFSCREEN_PATH_SOURCES,
            "conclusion": "clean" if (hits == 0 and off_hits == 0) else "dirty",
            "offscreen_entry": "yr_shell_render_offscreen_png",
            "scan_scope": "all top-level *.mm *.h + explicit offscreen-path sources (independent re-grep)",
            "note": "离屏路径未调用抢焦点 API；窗口路径仅构建 NSWindow，不运行/激活/前置",
        },
        "event_capture": {
            "file": "event_capture.json",
            "case_count": ec["case_count"],
            "selftest_failures": ec["selftest_failures"],
        },
    }
    with open(os.path.join(HERE, "shell_contract.json"), "w", encoding="utf-8") as f:
        json.dump(contract, f, ensure_ascii=False, indent=2)
        f.write("\n")
    print("GEN: exported_symbols=%d diff_pixel_count=%d focus_api_hits=%d "
          "focus_offscreen_path_api_hits=%d frame_a=%dx%d frame_b=%dx%d "
          "case_count=%d abi_sha256=%s abi_version=%s" %
          (len(syms), diff, hits, off_hits, wa, ha, wb, hb,
           ec["case_count"], abi["sha256"], abi["version"]))
    return 0


# --------------------------------------------------------------------------
# 核心核验：返回 (ok, report, fail_names)
# --------------------------------------------------------------------------
def verify(root):
    report = []
    fails = []

    def need(name, cond, detail=""):
        st = "PASS" if cond else "FAIL"
        report.append((st, name, detail))
        if not cond:
            fails.append(name)
        return cond

    def P(*parts):
        return os.path.join(root, *parts)

    app = P("YuriSlice.app")
    exe = P("YuriSlice.app", "Contents", "MacOS", "YuriSlice")
    info = P("YuriSlice.app", "Contents", "Info.plist")
    notice = P("YuriSlice.app", "Contents", "Resources", "NOTICE.txt")

    # --- 1. bundle 三文件 + 可执行位 + plutil ---
    need("bundle_three_files",
         os.path.isfile(info) and os.path.isfile(exe) and os.path.isfile(notice))
    need("exe_executable_bit",
         os.path.isfile(exe) and (os.stat(exe).st_mode & 0o111) != 0)
    try:
        lint_rc = subprocess.call(["plutil", "-lint", info],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        lint_rc = -1
    need("plutil_lint", lint_rc == 0, "rc=%d" % lint_rc)

    # --- 2. shell_contract.json vs nm -gU / sha256 / plist ---
    try:
        with open(P("shell_contract.json"), encoding="utf-8") as f:
            c = json.load(f)
    except Exception as ex:
        need("contract_readable", False, str(ex))
        return (False, report, fails)

    measured = nm_symbols(exe)
    contract_syms = sorted(set(c.get("exported_symbols", [])))
    need("symbols_match_nm", sorted(measured) == contract_syms,
         "nm=%s contract=%s" % (sorted(measured), contract_syms))
    need("symbol_count_ge3", len(measured) >= 3, "count=%d" % len(measured))
    for bf in c.get("bundle_files", []):
        bp = os.path.join(app, bf["path"])
        need("bundle_sha:" + bf["path"],
             os.path.isfile(bp) and sha256_file(bp) == bf["sha256"])
    try:
        pl = plutil_json(info)
    except Exception:
        pl = {}
    for k, v in c.get("info_plist", {}).items():
        need("plist:" + k, pl.get(k) == v, "want=%r got=%r" % (v, pl.get(k)))

    # --- 3. 抢焦点 API（独立重 grep 源码，不采信结论文件） ---
    hits = count_forbidden(root)
    need("focus_api_hits_zero", hits == 0, "hits=%d" % hits)
    need("focus_recorded_matches", c["focus"]["forbidden_api_hits"] == hits,
         "recorded=%d computed=%d" % (c["focus"]["forbidden_api_hits"], hits))
    need("focus_conclusion_clean", c["focus"]["conclusion"] == "clean")
    need("focus_offscreen_entry", c["focus"]["offscreen_entry"] ==
         "yr_shell_render_offscreen_png")
    # Codex 要求：独立统计【离屏路径源码】命中数（不是文档里写「未用」就算数）。
    off_hits = count_forbidden_offscreen(root)
    need("focus_offscreen_path_api_hits_zero", off_hits == 0,
         "offscreen_hits=%d" % off_hits)
    need("focus_offscreen_recorded_matches",
         c["focus"]["offscreen_path_api_hits"] == off_hits,
         "recorded=%d computed=%d" % (c["focus"]["offscreen_path_api_hits"], off_hits))
    need("focus_offscreen_sources_listed",
         c["focus"]["offscreen_path_sources"] == OFFSCREEN_PATH_SOURCES)

    # --- 3b. 共享 ABI 头（真实重读 ../shared/yr_slice.h，不采信自报哈希） ---
    abi = abi_info()
    need("abi_header_sha_matches", c["abi"]["header_sha256"] == abi["sha256"],
         "contract=%s actual=%s" % (c["abi"]["header_sha256"], abi["sha256"]))
    need("abi_version_matches", c["abi"]["YR_SLICE_ABI_VERSION"] == abi["version"] == 1)
    need("abi_version_string_matches",
         c["abi"]["YR_SLICE_ABI_VERSION_STRING"] == abi["version_string"] == "1")

    # --- 3c. frame_correlation 预留字段（结构化，不靠关键词） ---
    fc = c.get("frame_correlation", {})
    need("frame_correlation_present", bool(fc))
    need("frame_correlation_not_verified",
         fc.get("verified_real_correlation") is False)
    need("frame_correlation_fields",
         all(k in fc.get("fields", {}) for k in
             ("frame_index", "tick", "event_seq", "entity_ids", "order_seq")))
    need("frame_correlation_field_paths",
         all("source" in v and "round6_path" in v
             for v in fc.get("fields", {}).values()))

    # --- 3d. frame_meta.json（真实重读文件，mock 占位值须显式标注） ---
    try:
        with open(P("frame_meta.json"), encoding="utf-8") as f:
            fm = json.load(f)
        need("frame_meta_marker", fm.get("marker") == "SELF_AUTHORED_MOCK")
        need("frame_meta_value_kind", fm.get("value_kind") == "MOCK_PLACEHOLDER")
        need("frame_meta_frames_2", len(fm.get("frames", [])) == 2)
        need("frame_meta_contract_frames",
             c["frame_meta"]["frame_count"] == len(fm.get("frames", [])))
        for fr in fm.get("frames", []):
            i = fr.get("frame_index")
            need("frame_meta_%s_fields" % i,
                 all(k in fr for k in ("frame_index", "provider_call_seq", "tick",
                                       "event_seq", "entity_ids", "order_seq")))
            need("frame_meta_%s_all_mock" % i,
                 all(fr.get(k) is True for k in
                     ("tick_is_mock", "event_seq_is_mock",
                      "entity_ids_is_mock", "order_seq_is_mock")))
        seqs = [fr.get("provider_call_seq") for fr in fm.get("frames", [])]
        need("frame_meta_call_seq_distinct", len(set(seqs)) == len(seqs))
    except Exception as ex:
        need("frame_meta_readable", False, str(ex))

    # --- 4. PNG 逐像素证据（真实重解码） ---
    fe = c["frame_evidence"]
    try:
        wa, ha, ra = decode_png(P("frame_a.png"))
        wb, hb, rb = decode_png(P("frame_b.png"))
        dec_ok = True
    except Exception as ex:
        dec_ok = False
        wa = ha = wb = hb = 0
        ra = rb = None
        need("png_decode_ok", False, str(ex))
    if dec_ok:
        need("png_decode_ok", True)
        need("png_a_dims_ge_1280x720", wa >= 1280 and ha >= 720, "%dx%d" % (wa, ha))
        need("png_b_dims_ge_1280x720", wb >= 1280 and hb >= 720, "%dx%d" % (wb, hb))
        need("png_dims_recorded",
             fe["width"] == wa and fe["height"] == ha and
             fe["width_b"] == wb and fe["height_b"] == hb)
        need("png_a_sha", sha256_file(P("frame_a.png")) == fe["frame_a_sha256"])
        need("png_b_sha", sha256_file(P("frame_b.png")) == fe["frame_b_sha256"])
        diff = pixel_diff(ra, rb)
        need("png_diff_gt0", diff > 0, "diff=%d" % diff)
        need("png_diff_matches_record", diff == fe["diff_pixel_count"],
             "diff=%d recorded=%d" % (diff, fe["diff_pixel_count"]))
        need("png_a_not_blank", distinct_colors(ra) > 1)
        need("png_b_not_blank", distinct_colors(rb) > 1)

    # --- 5. event_capture.json 映射（独立映射表重算） ---
    try:
        with open(P("event_capture.json"), encoding="utf-8") as f:
            ec = json.load(f)
        need("event_case_count", len(ec["cases"]) == ec["case_count"])
        need("event_selftest_failures_zero", ec["selftest_failures"] == 0)
        for case in ec["cases"]:
            i = case["id"]
            rt = NS_TYPE_TO_YR[case["nsevent_type"]]
            x = case["loc_x"] // case["cell_px"]
            y = (case["view_h"] - case["loc_y"]) // case["cell_px"]
            mods = sum(MOD_BITS[n] for n in case["modifier_flags"])
            rec = {"type": rt, "x": x, "y": y, "modifiers": mods,
                   "click_count": case["click_count"],
                   "key_code": case["key_code"] if case["is_key"] else -1}
            need("case%d_expected_recompute" % i, case["expected"] == rec,
                 "expected=%s recomputed=%s" % (case["expected"], rec))
            need("case%d_actual_eq_expected" % i, case["actual"] == case["expected"])
            need("case%d_type" % i, case["actual"]["type"] == rt)
            need("case%d_modifiers" % i, case["actual"]["modifiers"] == mods)
            need("case%d_flags" % i,
                 bool(case["pass"]) and bool(case["sink_delivered"]) and
                 bool(case["sink_matches"]))
    except Exception as ex:
        need("event_capture_parse", False, str(ex))

    return (len(fails) == 0, report, fails)


# --------------------------------------------------------------------------
# 正向核验
# --------------------------------------------------------------------------
def cmd_verify_positive():
    ok, report, fails = verify(HERE)
    for st, name, detail in report:
        print("[%s] %s%s" % (st, name, ("  - " + detail) if detail else ""))
    print("VERIFY_RESULT=%s CHECKS=%d FAILS=%d" %
          ("PASS" if ok else "FAIL", len(report), len(fails)))
    return 0 if ok else 1


# --------------------------------------------------------------------------
# 篡改（负面测试）
# --------------------------------------------------------------------------
def apply_tamper(root, mode):
    if mode == "png":
        # 真实解码 -> 改 1 个像素（把 frame_b 的 (1,1) 设为 frame_a 的 (1,1)）-> 重编码
        fa = os.path.join(root, "frame_a.png")
        fb = os.path.join(root, "frame_b.png")
        _, _, ra = decode_png(fa)
        w, h, rb = decode_png(fb)
        rows = [bytearray(r) for r in rb]
        rows[1][4:8] = ra[1][4:8]  # 恰好 1 个像素的 RGBA 被改写
        with open(fb, "wb") as f:
            f.write(png_encode(w, h, [bytes(r) for r in rows]))
    elif mode == "symbols":
        p = os.path.join(root, "shell_contract.json")
        with open(p, encoding="utf-8") as f:
            c = json.load(f)
        c["exported_symbols"] = [s for s in c["exported_symbols"]
                                 if "set_event_sink" not in s] + ["_yr_shell_bogus_symbol"]
        c["exported_symbol_count"] = len(c["exported_symbols"])
        with open(p, "w", encoding="utf-8") as f:
            json.dump(c, f, ensure_ascii=False, indent=2)
    elif mode == "modifiers":
        p = os.path.join(root, "event_capture.json")
        with open(p, encoding="utf-8") as f:
            e = json.load(f)
        e["cases"][2]["actual"]["modifiers"] = 5  # 原为 control=2
        with open(p, "w", encoding="utf-8") as f:
            json.dump(e, f, ensure_ascii=False, indent=2)
    elif mode == "focus":
        # 源码注入被禁 API + 伪造「未抢焦点」结论
        p = os.path.join(root, "shell.mm")
        with open(p, "a", encoding="utf-8") as f:
            f.write("\n/* tamper injection: activateIgnoringOtherApps */\n")
        cp = os.path.join(root, "shell_contract.json")
        with open(cp, encoding="utf-8") as f:
            c = json.load(f)
        c["focus"]["forbidden_api_hits"] = 0
        c["focus"]["conclusion"] = "clean"
        with open(cp, "w", encoding="utf-8") as f:
            json.dump(c, f, ensure_ascii=False, indent=2)
    elif mode == "focus_api":
        # 向临时副本的【离屏路径源码】注入真实抢焦点调用（仅源码级检出，不编译）。
        # 核验器 count_forbidden_offscreen 独立重 grep 必须命中并拒绝。
        p = os.path.join(root, "shell.mm")
        with open(p, "a", encoding="utf-8") as f:
            f.write("\n/* tamper focus_api: focus-stealing calls injected into OFFSCREEN path source */\n")
            f.write("void tamper_focus_probe(void) {\n")
            f.write("  [NSApp activateIgnoringOtherApps:YES];\n")
            f.write("  [win orderFrontRegardless];\n")
            f.write("  [win makeKeyAndOrderFront:nil];\n")
            f.write("}\n")
    elif mode == "exe":
        os.remove(os.path.join(root, "YuriSlice.app", "Contents", "MacOS", "YuriSlice"))
    else:
        raise ValueError("unknown tamper mode: %s" % mode)


def cmd_tamper(mode):
    if mode not in TAMPER_MODES:
        print("unknown tamper mode: %s (choices: %s)" % (mode, ",".join(TAMPER_MODES)))
        return 2
    with tempfile.TemporaryDirectory(prefix="p22_tamper_") as td:
        root = os.path.join(td, "work")
        shutil.copytree(HERE, root)
        apply_tamper(root, mode)
        ok, report, fails = verify(root)
        if ok:
            # 篡改没被发现 = 核验器有漏洞，这是负面测试的失败
            print("TAMPER_%s: NOT DETECTED (verification unexpectedly PASSED) -> BUG" % mode)
            return 0
        print("TAMPER_%s: DETECTED (verification FAILED, %d/%d checks rejected; "
              "first fails: %s)" % (mode, len(fails), len(report), ",".join(fails[:8])))
        return 3  # 非 0：检测到篡改


def main():
    args = sys.argv[1:]
    if args and args[0] == "--gen":
        return cmd_gen()
    if args and args[0] == "--tamper":
        if len(args) < 2:
            print("usage: verify_shell.py --tamper <mode>")
            return 2
        return cmd_tamper(args[1])
    return cmd_verify_positive()


if __name__ == "__main__":
    sys.exit(main())
