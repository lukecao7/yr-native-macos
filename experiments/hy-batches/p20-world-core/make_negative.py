#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""make_negative.py — 生成 6 个「真实篡改」产物目录，供 verify_core.py 负面测试。

每个目录都是正常产物的完整副本，只改动一处数据（真实字节/字段篡改，非关键词替换）：
  case1: trace_1t.json 中 1 个 int16 整数坐标 +1（step=123, unit=456, x），保留原 hash
  case2: tiles_ref.json 中 1 个 cell 的 tile 值改为「翻转通行性且改颜色」的值
  case3: trace_4t.json 的 per_step_sha256[200] 改为错误 hex，结论仍 identical=true
  case4: snapshot_1t.json 中 1 个单位的 x 改成 (x+7)%width（范围内但错误）
  case5: threads_guard.json 中 threads=2 的 return_code 改成 0（假成功）
  case6: frame_rgba_stride.bin 中 1 个 padding 字节翻转
"""
import base64
import json
import os
import shutil
import struct
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
os.chdir(HERE)

ARTIFACTS = [
    "snapshot_1t.json", "snapshot_4t.json",
    "trace_1t.json", "trace_4t.json",
    "consistency.json", "map_digest.json", "cells_baseline.bin",
    "frame.png", "frame_rgba_stride.bin", "frame_meta.json",
    "threads_guard.json", "order_guard.json", "init_determinism.json",
    "timing_1t.json", "timing_4t.json", "tiles_ref.json", "unittem-ra2.pal",
]

NEG_DIRS = ["case1", "case2", "case3", "case4", "case5", "case6"]


def read_json(p):
    with open(p, "r", encoding="utf-8") as f:
        return json.load(f)


def write_json(p, obj):
    with open(p, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False, indent=2)
        f.write("\n")


def pas(t, l, i):
    if l != 0 or i != 0:
        return False
    return ((t * 2654435761) % 100) < 80


def make_case(name, tamper):
    d = os.path.join("neg", name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    for a in ARTIFACTS:
        shutil.copyfile(a, os.path.join(d, a))
    tamper(d)
    print("built neg/%s" % name)


def case1(d):
    p = os.path.join(d, "trace_1t.json")
    t = read_json(p)
    raw = bytearray(base64.b64decode(t["positions_le_u16_base64"]))
    N = int(t["unit_count"])
    step, unit, axis = 123, 456, 0  # x
    j = step * (N * 2) + unit * 2 + axis
    off = j * 2
    v = struct.unpack_from("<h", raw, off)[0]
    struct.pack_into("<h", raw, off, v + 1)
    t["positions_le_u16_base64"] = base64.b64encode(bytes(raw)).decode("ascii")
    write_json(p, t)  # per_step_sha256 故意保留原值 -> hash 自洽性矛盾


def case2(d):
    p = os.path.join(d, "tiles_ref.json")
    tiles = read_json(p)
    target_x, target_y = 100, 40
    hit = None
    for c in tiles["tiles"]:
        if int(c["x"]) == target_x and int(c["y"]) == target_y:
            hit = c
            break
    assert hit is not None, "target cell not found"
    old = int(hit["tile"])
    l, i = int(hit.get("level", 0)), int(hit.get("ice", 0))
    # 找一个既翻转通行性、又改变 tile%256（从而改变地表色）的新值
    new = None
    for v in range(0, 65536):
        if v % 256 != old % 256 and pas(v, l, i) != pas(old, l, i):
            new = v
            break
    assert new is not None
    hit["tile"] = new
    write_json(p, tiles)  # cells_baseline.bin / frame.png / map_digest 故意不联动


def case3(d):
    p = os.path.join(d, "trace_4t.json")
    t = read_json(p)
    t["per_step_sha256"][200] = "0" * 64  # 错误 hash
    write_json(p, t)  # base64 不变、consistency.json 仍 identical=true


def case4(d):
    p = os.path.join(d, "snapshot_1t.json")
    s = read_json(p)
    w = int(s["map"]["width"])
    u = s["units"][300]
    u["x"] = (int(u["x"]) + 7) % w  # 范围内但错误
    write_json(p, s)


def case5(d):
    p = os.path.join(d, "threads_guard.json")
    g = read_json(p)
    for c in g["cases"]:
        if int(c["threads"]) == 2:
            c["return_code"] = 0  # 假成功
    write_json(p, g)


def case6(d):
    p = os.path.join(d, "frame_rgba_stride.bin")
    with open(p, "rb") as f:
        b = bytearray(f.read())
    fm = read_json(os.path.join(d, "frame_meta.json"))
    stride = int(fm["stride"])
    row_bytes = int(fm["width"]) * 4
    row, pad_off = 100, 5
    idx = row * stride + row_bytes + pad_off
    b[idx] ^= 0xff  # 覆写一个 padding 字节
    with open(p, "wb") as f:
        f.write(b)


def case7(d):
    # 真实跳过 step；保持正常 4t 对照，不以 CLI 参数错误充当检出。
    env = dict(os.environ, YR_TILES_PATH=os.path.abspath(
        os.environ.get("YR_TILES_PATH", "../p5-isomap/tiles.json")))
    subprocess.run([os.path.join(HERE, "core_host"), "1t", "1", "--no-step"],
                   cwd=d, env=env, check=True)


def main():
    os.makedirs("neg", exist_ok=True)
    make_case("case1", case1)
    make_case("case2", case2)
    make_case("case3", case3)
    make_case("case4", case4)
    make_case("case5", case5)
    make_case("case6", case6)
    make_case("case7", case7)
    print("make_negative: 7 negative cases built (case7 really skips step)")


if __name__ == "__main__":
    main()
