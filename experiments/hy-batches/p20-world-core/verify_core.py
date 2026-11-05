#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_core.py — P20 独立核验器（纯 Python，仅用标准库）。

严格「只读产物 + 重算」：不调用本路任何 C++ 代码做判定，不链接 libyrworld.a，
不读取 core_host 的运行时内存。所有结论都由本文件独立重算得到：
  * SHA-256 重算（每步坐标 / 文件 / cell 序列），与产物记录值比对；
  * base64 解码为 steps*unit_count*2 个 int16 小端坐标，逐步/逐单位重算；
  * 1/4 线程 base64 逐字节比较、per_step_sha256 全等比较；
  * 通行性映射重算（SELF_AUTHORED_MAPPING 规则在 Python 独立实现）；
  * 地表色重算（SELF_AUTHORED_ART 规则在 Python 独立实现）与 frame.png 像素比对；
  * frame_rgba_stride.bin 每行 padding 哨兵重算，断言未被覆写；
  * snapshot id 升序 / 坐标范围 / 与 trace 末步交叉比对；
  * 一致性结论（consistency.json）与重算结果交叉验证（防「把结论改成 true」）。

用法： verify_core.py --dir DIR     (DIR 默认 ".")
退出码：0 = 全部通过；非 0 = 检出不一致（每条错误含 step/unit/cell/threads/row 定位）。
"""
import argparse
import base64
import hashlib
import json
import os
import struct
import sys
import zlib

SEED = 20261104
UNIT_COUNT = 1200
TICKS = 360
EXPECTED_COMPARED_PAIRS = 432000
MAGENTA = (255, 0, 255)


def sha256_hex(b):
    return hashlib.sha256(b).hexdigest()


def err(errors, msg):
    errors.append("FAIL: " + msg)


def load_json(d, name, errors):
    p = os.path.join(d, name)
    try:
        with open(p, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception as e:
        err(errors, f"cannot read/parse {name}: {e}")
        return None


def load_bytes(d, name, errors):
    p = os.path.join(d, name)
    try:
        with open(p, "rb") as f:
            return f.read()
    except Exception as e:
        err(errors, f"cannot read {name}: {e}")
        return None


def passable(tile, level, ice):
    """SELF_AUTHORED_MAPPING 规则的独立 Python 实现（与 C++ 同源规则，独立编码）。"""
    if level != 0 or ice != 0:
        return False
    return ((tile * 2654435761) % 100) < 80


def tile_color(pal, tile):
    """SELF_AUTHORED_ART 诊断着色的独立 Python 实现。"""
    idx = (tile & 0xffff) % 256
    return (pal[3 * idx] << 2, pal[3 * idx + 1] << 2, pal[3 * idx + 2] << 2)


def png_decode_rgba(data, errors):
    """纯标准库 PNG 解码（zlib + 手写 chunk 解析 + filter 0），返回 (w, h, pixels)。"""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        err(errors, "frame.png PNG signature invalid")
        return None
    pos = 8
    idat = b""
    w = h = None
    while pos + 12 <= len(data):
        ln = struct.unpack(">I", data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        ch = data[pos + 8:pos + 8 + ln]
        crc = struct.unpack(">I", data[pos + 8 + ln:pos + 12 + ln])[0]
        if (zlib.crc32(typ + ch) & 0xffffffff) != crc:
            err(errors, f"frame.png chunk {typ!r} CRC mismatch")
            return None
        if typ == b"IHDR":
            w, h, bd, ct, comp, filt, inter = struct.unpack(">IIBBBBB", ch)
            if bd != 8 or ct != 6 or inter != 0:
                err(errors, f"frame.png unsupported IHDR bd={bd} ct={ct} inter={inter}")
                return None
        elif typ == b"IDAT":
            idat += ch
        elif typ == b"IEND":
            break
        pos += 12 + ln
    if w is None or h is None:
        err(errors, "frame.png missing IHDR")
        return None
    try:
        raw = zlib.decompress(idat)
    except Exception as e:
        err(errors, f"frame.png zlib decompress failed: {e}")
        return None
    rowbytes = w * 4
    if len(raw) != h * (1 + rowbytes):
        err(errors, f"frame.png raw length {len(raw)} != {h * (1 + rowbytes)}")
        return None
    out = bytearray(h * rowbytes)
    for y in range(h):
        fb = raw[y * (1 + rowbytes)]
        if fb != 0:
            err(errors, f"frame.png row={y} filter={fb} unsupported (only 0 supported)")
            return None
        out[y * rowbytes:(y + 1) * rowbytes] = \
            raw[y * (1 + rowbytes) + 1:(y + 1) * (1 + rowbytes)]
    return w, h, bytes(out)


def first_step_unit_diff(raw1, raw4, N):
    """返回首个不一致 (step, unit, axis, coord_1t, coord_4t)。"""
    n = min(len(raw1), len(raw4)) // 2
    for j in range(n):
        if raw1[2 * j:2 * j + 2] != raw4[2 * j:2 * j + 2]:
            step = j // (N * 2)
            within = j % (N * 2)
            unit = within // 2
            axis = "x" if within % 2 == 0 else "y"
            v1 = struct.unpack_from("<h", raw1, j * 2)[0]
            v4 = struct.unpack_from("<h", raw4, j * 2)[0]
            return step, unit, axis, v1, v4
    return None


def verify(d):
    errors = []
    info = {}

    md = load_json(d, "map_digest.json", errors)
    fm = load_json(d, "frame_meta.json", errors)
    cons = load_json(d, "consistency.json", errors)
    tg = load_json(d, "threads_guard.json", errors)
    og = load_json(d, "order_guard.json", errors)
    idet = load_json(d, "init_determinism.json", errors)
    t1 = load_json(d, "trace_1t.json", errors)
    t4 = load_json(d, "trace_4t.json", errors)
    s1 = load_json(d, "snapshot_1t.json", errors)
    s4 = load_json(d, "snapshot_4t.json", errors)
    tiles = load_json(d, "tiles_ref.json", errors)
    tm1 = load_json(d, "timing_1t.json", errors)
    tm4 = load_json(d, "timing_4t.json", errors)
    pal = load_bytes(d, "unittem-ra2.pal", errors)
    cells = load_bytes(d, "cells_baseline.bin", errors)
    binbuf = load_bytes(d, "frame_rgba_stride.bin", errors)
    png = load_bytes(d, "frame.png", errors)

    if any(x is None for x in (md, fm, cons, tg, og, idet, t1, t4, s1, s4,
                               tiles, tm1, tm4, pal, cells, binbuf, png)):
        for e in errors:
            print(e)
        print("RESULT: FAIL (missing/corrupt artifacts)")
        return False, info, errors

    # ---------------- 地图出处与 cell 网格重算 ----------------
    ref_sha = sha256_hex(open(os.path.join(d, "tiles_ref.json"), "rb").read())
    if ref_sha != md.get("tiles_ref_sha256"):
        err(errors, f"tiles_ref.json sha256 mismatch: recomputed={ref_sha} recorded={md.get('tiles_ref_sha256')}")
    if md.get("p5_tiles_sha256") != md.get("tiles_ref_sha256"):
        err(errors, "map_digest p5_tiles_sha256 != tiles_ref_sha256 (copy not faithful)")
    if len(pal) != 768 or sha256_hex(pal) != md.get("pal768_sha256"):
        err(errors, f"unittem-ra2.pal sha256/size mismatch: len={len(pal)} sha={sha256_hex(pal)}")

    w = int(tiles["width"])
    h = int(tiles["height"])
    if w != md.get("width") or h != md.get("height"):
        err(errors, f"map width/height mismatch: tiles=({w},{h}) digest=({md.get('width')},{md.get('height')})")
    if int(tiles["cell_count"]) != w * h:
        err(errors, f"cell_count mismatch: {tiles['cell_count']} != {w * h}")
    if tiles.get("theater") != "TEMPERATE":
        err(errors, f"theater mismatch: {tiles.get('theater')}")

    grid = [0] * (w * h)
    lvl = [0] * (w * h)
    ice = [0] * (w * h)
    for c in tiles["tiles"]:
        x, y = int(c["x"]), int(c["y"])
        if 0 <= x < w and 0 <= y < h:
            k = y * w + x
            grid[k] = int(c["tile"])
            lvl[k] = int(c.get("level", 0))
            ice[k] = int(c.get("ice", 0))
    cells_ser = struct.pack("<%dH" % (w * h), *[t & 0xffff for t in grid])
    if sha256_hex(cells_ser) != md.get("cells_sha256"):
        err(errors, f"cells_sha256 mismatch: recomputed={sha256_hex(cells_ser)} recorded={md.get('cells_sha256')}")
    # 与 cells_baseline.bin 逐 cell 比对，定位被改动的 cell
    if cells_ser != cells:
        m = min(len(cells_ser), len(cells))
        k = next((i for i in range(0, m, 2) if cells_ser[i:i + 2] != cells[i:i + 2]), m)
        cx, cy = (k // 2) % w, (k // 2) // w
        old = struct.unpack_from("<H", cells, k)[0] if k + 2 <= len(cells) else -1
        new = struct.unpack_from("<H", cells_ser, k)[0] if k + 2 <= len(cells_ser) else -1
        err(errors, f"cell=({cx},{cy}) tile differs from cells_baseline.bin: baseline={old} now={new}")
    pc = sum(1 for k in range(w * h) if passable(grid[k], lvl[k], ice[k]))
    ic = w * h - pc
    if pc != md.get("passable_count") or ic != md.get("impassable_count"):
        err(errors, f"passable_count mismatch: recomputed=({pc},{ic}) recorded=({md.get('passable_count')},{md.get('impassable_count')})")
    uniq = len(set(grid))
    if uniq != md.get("unique_tiles"):
        err(errors, f"unique_tiles mismatch: recomputed={uniq} recorded={md.get('unique_tiles')}")
    info["passable_count"] = pc
    info["unique_tiles"] = uniq

    # ---------------- 帧缓冲 / PNG / padding / 地表 / 单位 ----------------
    width = int(fm["width"])
    height = int(fm["height"])
    stride = int(fm["stride"])
    pad = int(fm["pad_bytes"])
    scale = int(fm["cell_scale"])
    mpp = int(fm["marker_pixels_per_unit"])
    if width != w * scale or height != h * scale:
        err(errors, f"frame size mismatch: ({width},{height}) != ({w},{h})*{scale}")
    if stride != width * 4 + pad:
        err(errors, f"stride mismatch: {stride} != {width}*4+{pad}")
    if len(binbuf) != height * stride:
        err(errors, f"frame_rgba_stride.bin length {len(binbuf)} != {height}*{stride}")
    dec = png_decode_rgba(png, errors)
    if dec is None:
        for e in errors:
            print(e)
        print("RESULT: FAIL (PNG decode)")
        return False, info, errors
    pw, ph, px = dec
    if pw != width or ph != height:
        err(errors, f"frame.png size ({pw},{ph}) != ({width},{height})")
    # 像素区：bin == PNG（证明 PNG 忠实于帧缓冲像素区）
    if len(binbuf) == height * stride and pw == width and ph == height:
        for r in range(height):
            a = binbuf[r * stride:r * stride + width * 4]
            b = px[r * width * 4:(r + 1) * width * 4]
            if a != b:
                err(errors, f"row={r} pixel area of frame_rgba_stride.bin differs from frame.png")
                break
    # padding 哨兵重算：(i*13+7)&0xff，i 为绝对字节索引
    pad_bad = 0
    for r in range(height):
        for j in range(width * 4, stride):
            i = r * stride + j
            exp = (i * 13 + 7) & 0xff
            if binbuf[i] != exp:
                err(errors, f"padding overwritten row={r} pad_offset={j - width * 4} got={binbuf[i]} exp={exp}")
                pad_bad += 1
                break
        if pad_bad >= 4:
            break
    # 地表色重算（跳过品红单位像素）
    bad_cells = 0
    for cy in range(h):
        for cx in range(w):
            exp = tile_color(pal, grid[cy * w + cx])
            mismatch = False
            for py in range(scale):
                for pxx in range(scale):
                    gx, gy = cx * scale + pxx, cy * scale + py
                    o = (gy * width + gx) * 4
                    rgb = (px[o], px[o + 1], px[o + 2])
                    if rgb == MAGENTA:
                        continue
                    if rgb != exp or px[o + 3] != 255:
                        mismatch = True
                        break
                if mismatch:
                    break
            if mismatch:
                err(errors, f"cell=({cx},{cy}) terrain pixel mismatch: expected={exp}")
                bad_cells += 1
                if bad_cells >= 4:
                    break
        if bad_cells >= 4:
            break
    # 单位可见性：与 snapshot_1t 坐标交叉比对
    occ = set()
    for u in s1["units"]:
        occ.add((int(u["x"]), int(u["y"])))
    mcount = sum(1 for o in range(0, len(px), 4)
                 if (px[o], px[o + 1], px[o + 2]) == MAGENTA)
    if mcount != mpp * len(occ):
        err(errors, f"unit magenta pixel count {mcount} != marker_pixels_per_unit*distinct_cells={mpp * len(occ)}")
    if int(fm["nonbg_reported"]) != mpp * UNIT_COUNT:
        err(errors, f"nonbg_reported {fm['nonbg_reported']} != {mpp}*{UNIT_COUNT}")
    vis_bad = 0
    for u in s1["units"]:
        bx, by = int(u["x"]) * scale, int(u["y"]) * scale
        off = (scale - (2 if scale >= 2 else 1)) // 2
        gx, gy = bx + off, by + off
        o = (gy * width + gx) * 4
        if (px[o], px[o + 1], px[o + 2]) != MAGENTA:
            err(errors, f"unit={u['id']} at ({u['x']},{u['y']}) not visible (no magenta marker)")
            vis_bad += 1
            if vis_bad >= 4:
                break
    info["pixels"] = width * height
    info["magenta_pixels"] = mcount
    info["distinct_unit_cells"] = len(occ)

    # ---------------- snapshot 结构 ----------------
    for tag, s in (("1t", s1), ("4t", s4)):
        units = s["units"]
        if len({(u["x"], u["y"]) for u in units}) != len(units):
            err(errors, f"snapshot_{tag} unit overlap")
        if len(units) != UNIT_COUNT:
            err(errors, f"snapshot_{tag} units length {len(units)} != {UNIT_COUNT}")
        ids = [int(u["id"]) for u in units]
        if ids != list(range(UNIT_COUNT)):
            k = next((i for i in range(min(len(ids), UNIT_COUNT)) if ids[i] != i), 0)
            err(errors, f"snapshot_{tag} id order/contiguity broken at unit index={k} id={ids[k] if k < len(ids) else 'NA'}")
        for u in units:
            x, y = int(u["x"]), int(u["y"])
            if not (0 <= x < w and 0 <= y < h):
                err(errors, f"snapshot_{tag} unit={u['id']} coord out of range=({x},{y}) map=({w},{h})")
                break
        if s["map"]["width"] != w or s["map"]["height"] != h:
            err(errors, f"snapshot_{tag} map size mismatch")
        if s["map"]["source"] != tiles.get("source"):
            err(errors, f"snapshot_{tag} source mismatch: {s['map']['source']} != {tiles.get('source')}")
        if int(s["tick"]) != TICKS:
            err(errors, f"snapshot_{tag} tick {s['tick']} != {TICKS}")
    for a, b in zip(s1["units"], s4["units"]):
        if (int(a["id"]), int(a["x"]), int(a["y"])) != (int(b["id"]), int(b["x"]), int(b["y"])):
            err(errors, f"snapshot 1t/4t differ at unit={a['id']} 1t=({a['x']},{a['y']}) 4t=({b['x']},{b['y']})")
            break

    # ---------------- trace 解析 + 逐步 SHA-256 重算 ----------------
    def parse_trace(t, tag):
        N = int(t["unit_count"])
        steps = int(t["steps"])
        hs = list(t["per_step_sha256"])
        try:
            raw = base64.b64decode(t["positions_le_u16_base64"], validate=True)
        except Exception as e:
            err(errors, f"trace_{tag} base64 decode failed: {e}")
            raw = b""
        return N, steps, hs, raw

    N1, steps1, hs1, raw1 = parse_trace(t1, "1t")
    N4, steps4, hs4, raw4 = parse_trace(t4, "4t")
    if N1 != UNIT_COUNT or N4 != UNIT_COUNT:
        err(errors, f"trace unit_count mismatch: 1t={N1} 4t={N4} expected={UNIT_COUNT}")
    if steps1 != TICKS or steps4 != TICKS:
        err(errors, f"trace steps mismatch: 1t={steps1} 4t={steps4} expected={TICKS}")
    if len(raw1) != steps1 * N1 * 4:
        err(errors, f"trace_1t raw length {len(raw1)} != {steps1}*{N1}*4")
    if len(raw4) != steps4 * N4 * 4:
        err(errors, f"trace_4t raw length {len(raw4)} != {steps4}*{N4}*4")
    for tag, raw, hs, N, steps in (("1t", raw1, hs1, N1, steps1),
                                   ("4t", raw4, hs4, N4, steps4)):
        if len(hs) != steps:
            err(errors, f"trace_{tag} per_step_sha256 length {len(hs)} != steps {steps}")
        for s in range(min(steps, len(hs))):
            seg = raw[s * N * 4:(s + 1) * N * 4]
            hh = sha256_hex(seg)
            if hh != hs[s]:
                err(errors, f"trace_{tag} step={s} per_step_sha256 mismatch: recorded={hs[s]} recomputed={hh}")
            if len(seg) == N * 4:
                coords = list(struct.iter_unpack("<hh", seg))
                if len(set(coords)) != N:
                    err(errors, f"trace_{tag} step={s} unit overlap")
                    break
                bad = next(((i, x, y) for i, (x, y) in enumerate(coords)
                            if not (0 <= x < w and 0 <= y < h)
                            or not passable(grid[y*w+x], lvl[y*w+x], ice[y*w+x])), None)
                if bad:
                    err(errors, f"trace_{tag} step={s} unit={bad[0]} invalid cell=({bad[1]},{bad[2]})")
                    break
    # 1/4 线程逐字节一致性（可定位首个 step/unit）
    byte_eq = (raw1 == raw4)
    hash_eq = (hs1 == hs4)
    if not byte_eq:
        d0 = first_step_unit_diff(raw1, raw4, N1)
        if d0:
            step, unit, axis, v1, v4 = d0
            err(errors, f"NOT_ACHIEVED: 1/4 positions differ first at step={step} unit={unit} axis={axis} coord_1t={v1} coord_4t={v4}")
        else:
            err(errors, "1/4 positions_byte_equal=false (length differs)")
    if not hash_eq:
        k = next((i for i in range(min(len(hs1), len(hs4))) if hs1[i] != hs4[i]), -1)
        err(errors, f"per_step_sha256 arrays differ at step={k} 1t={hs1[k] if k >= 0 else 'NA'} 4t={hs4[k] if k >= 0 else 'NA'}")
    actual_identical = byte_eq and hash_eq
    if not actual_identical:
        err(errors, "recomputed 1/4 thread equality = false (NOT_ACHIEVED if this is a real run; or a tampered artifact)")

    # 比较对数：可核算整数 360*1200 = 432000
    compared = steps1 * N1
    if compared != EXPECTED_COMPARED_PAIRS:
        err(errors, f"compared_pairs {compared} != {EXPECTED_COMPARED_PAIRS} (360*1200)")
    info["compared_pairs"] = compared

    # ---------------- snapshot 与 trace 末步坐标交叉比对 ----------------
    if steps1 > 0 and len(raw1) >= steps1 * N1 * 4:
        last = raw1[(steps1 - 1) * N1 * 4:steps1 * N1 * 4]
        coords = struct.unpack("<%dh" % (N1 * 2), last)
        for i, u in enumerate(s1["units"]):
            x, y = coords[2 * i], coords[2 * i + 1]
            if x != int(u["x"]) or y != int(u["y"]):
                err(errors, f"snapshot_1t vs last trace step mismatch unit={u['id']} snap=({u['x']},{u['y']}) trace=({x},{y})")
                break

    # ---------------- 一致性结论交叉验证（防篡改结论） ----------------
    if bool(cons.get("identical")) != actual_identical:
        err(errors, f"consistency.identical={cons.get('identical')} contradicts recomputed equality={actual_identical}")
    if bool(cons.get("positions_byte_equal")) != byte_eq:
        err(errors, f"consistency.positions_byte_equal={cons.get('positions_byte_equal')} contradicts byte_eq={byte_eq}")
    if bool(cons.get("per_step_hashes_equal")) != hash_eq:
        err(errors, f"consistency.per_step_hashes_equal={cons.get('per_step_hashes_equal')} contradicts hash_eq={hash_eq}")
    if int(cons.get("compared_pairs", -1)) != compared:
        err(errors, f"consistency.compared_pairs={cons.get('compared_pairs')} != {compared}")
    if int(cons.get("steps", -1)) != steps1 or int(cons.get("unit_count", -1)) != N1:
        err(errors, "consistency steps/unit_count mismatch with trace")

    # ---------------- threads / order / init 守卫 ----------------
    for c in tg.get("cases", []):
        if int(c["return_code"]) != -3:
            err(errors, f"threads={c['threads']} return_code={c['return_code']} expected -3")
    if not any(int(c["threads"]) == 2 for c in tg.get("cases", [])):
        err(errors, "threads_guard missing threads=2 case")
    for k in ("no_selection", "out_of_bounds", "impassable"):
        if int(og.get(k, 0)) > 0:
            err(errors, f"order_guard {k}={og.get(k)} should be <=0 (不得把非法命令计成功)")
    if int(og.get("valid_accepted", 0)) <= 0:
        err(errors, f"order_guard valid_accepted={og.get('valid_accepted')} should be >0")
    if not (idet.get("equal") is True and idet.get("hash_a") == idet.get("hash_b")
            and len(str(idet.get("hash_a", ""))) == 64):
        err(errors, "init_determinism hash_a != hash_b or equal != true")

    # ---------------- timing ----------------
    for tag, tm in (("1t", tm1), ("4t", tm4)):
        if int(tm.get("steps", -1)) != TICKS or int(tm.get("unit_count", -1)) != UNIT_COUNT \
                or int(tm.get("seed", -1)) != SEED or int(tm.get("advanced", -1)) != TICKS:
            err(errors, f"timing_{tag} steps/unit_count/seed/advanced mismatch")
    info["wall_ms_1t"] = tm1.get("wall_ms")
    info["wall_ms_4t"] = tm4.get("wall_ms")
    info["identical"] = actual_identical

    # ---------------- 输出 ----------------
    if errors:
        for e in errors:
            print(e)
        print("RESULT: FAIL (%d checks failed)" % len(errors))
        return False, info, errors
    print("OK: all independent recomputations passed")
    print("METRICS unit_count=%d steps=%d compared_pairs=%d pixels=%d magenta_pixels=%d "
          "passable_count=%d distinct_unit_cells=%d identical=%s wall_ms_1t=%s wall_ms_4t=%s" %
          (UNIT_COUNT, TICKS, compared, info["pixels"], mcount, pc, len(occ),
           actual_identical, tm1.get("wall_ms"), tm4.get("wall_ms")))
    print("RESULT: PASS")
    return True, info, errors


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=".", help="产物目录")
    args = ap.parse_args()
    ok, info, errors = verify(args.dir)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
