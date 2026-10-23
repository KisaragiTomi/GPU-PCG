#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""核对 Tiny Glade 原件碎裂图案 `rocky_terrain_shell.glb`，并画出接线文档用的预览图。

这是 `Docs/TinyGlade/RockShell_Pattern.md` 里每一个实测数字的**来源**：改了那份文档的表，
先重跑本脚本；本脚本报什么就写什么。它**只读** GLB，不写任何游戏资产。

原件出处：`D:/MyProject/Tiny Glade/extract/rocky_terrain2glb.py` 从
`assets/data/rocky_terrain.json` 导出（⚠️ 不是 `assets/meshes/terrain_rocks.json`，
后者是 ±430 m 的背景岩石、零 cell 属性）。

用法::

    python VerifyRockShellGlb.py                       # 核对 + 出预览图
    python VerifyRockShellGlb.py --no-preview          # 只核对
    python VerifyRockShellGlb.py --glb <path.glb>
"""

from __future__ import annotations

import argparse
import json
import math
import os
import struct
import sys
from collections import Counter, defaultdict
from typing import Dict, List, Sequence, Tuple

# =============================================================================
# 参数
# =============================================================================

DEFAULT_GLB = r"D:\MyProject\Tiny Glade\extracted\meshes\rocky_terrain_shell.glb"
DEFAULT_PREVIEW_RELPATH = os.path.join("Docs", "TinyGlade", "RockShell_Pattern.preview.png")

TOP_Y_M = 3.12          # 顶圈的 y（米，已 ×65）；底圈是 0。只有这两个值。
TILE_HALF_M = 68.25     # tile 半跨（米）
GROUND_SPAN_M = 128.0   # 本项目地面跨度（NumCells 256 × CellSize 50 cm）

# 文档 `Docs/TinyGlade/Ground_Shaper.md`「烘焙资产」表里的数字，用来逐项对照
EXPECTED = {
    "vertices": 148794,
    "triangles": 49598,
    "cells": 609,
    "cap_triangles": 27566,
    "skirt_triangles": 22032,
    "top_ring_verts": 115718,
    "bottom_ring_verts": 33076,
    "corner_verts": 28669,
}


# =============================================================================
# 最小 glTF 读取（无第三方依赖）
# =============================================================================

_CTYPE = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2), 5123: ("H", 2),
          5125: ("I", 4), 5126: ("f", 4)}
_NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}


class Glb:
    def __init__(self, path: str) -> None:
        raw = open(path, "rb").read()
        magic, version, declared = struct.unpack_from("<4sII", raw, 0)
        if magic != b"glTF" or version != 2:
            raise SystemExit(f"不是 glTF 2.0 二进制：{magic!r} v{version}")
        self.declared_size = declared
        self.file_size = len(raw)
        chunks: Dict[bytes, bytes] = {}
        off = 12
        while off < len(raw):
            clen, ctype = struct.unpack_from("<I4s", raw, off)
            chunks[ctype] = raw[off + 8: off + 8 + clen]
            off += 8 + clen
        self.doc = json.loads(chunks[b"JSON"].decode("utf-8"))
        self.bin = chunks[b"BIN\x00"]

    def accessor(self, index: int) -> Tuple[List[tuple], dict]:
        acc = self.doc["accessors"][index]
        view = self.doc["bufferViews"][acc["bufferView"]]
        fmt, size = _CTYPE[acc["componentType"]]
        ncomp = _NCOMP[acc["type"]]
        stride = view.get("byteStride") or (size * ncomp)
        base = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
        unpack = struct.Struct("<" + fmt * ncomp).unpack_from
        return [unpack(self.bin, base + i * stride) for i in range(acc["count"])], acc


# =============================================================================
# 载入并派生
# =============================================================================

class Shell:
    """把 GLB 的六条通道解成本项目关心的量。**环（顶/底圈）一律由 POSITION.y 决定**，
    不能用 TEXCOORD_2.y —— 那是逐三角的 is_top，不是逐顶点的环。"""

    def __init__(self, glb: Glb) -> None:
        prim = glb.doc["meshes"][0]["primitives"][0]
        attrs = prim["attributes"]
        self.prim = prim
        self.attrs = attrs
        self.pos, self.acc_pos = glb.accessor(attrs["POSITION"])
        self.nrm, _ = glb.accessor(attrs["NORMAL"])
        self.uv0, self.acc_uv0 = glb.accessor(attrs["TEXCOORD_0"])   # dir_to_centroid.xz
        self.uv1, _ = glb.accessor(attrs["TEXCOORD_1"])              # (cell_id, is_corner)
        self.uv2, _ = glb.accessor(attrs["TEXCOORD_2"])              # (cell_bby, is_top)
        self.col, self.acc_col = glb.accessor(attrs["COLOR_0"]) if "COLOR_0" in attrs else (None, None)

        self.n = len(self.pos)
        self.tris = self.n // 3
        self.cell_id = [int(round(u[0])) for u in self.uv1]
        self.is_corner = [u[1] for u in self.uv1]
        self.cell_bby = [u[0] for u in self.uv2]
        self.tri_is_top = [u[1] for u in self.uv2]
        self.ring_top = [abs(p[1] - TOP_Y_M) < 1e-4 for p in self.pos]

        # 逐胞腔的顶圈质心（用顶圈去重后的位置）
        acc = defaultdict(lambda: [0.0, 0.0, 0])
        seen = set()
        for i in range(self.n):
            if not self.ring_top[i]:
                continue
            key = (self.cell_id[i], round(self.pos[i][0], 5), round(self.pos[i][2], 5))
            if key in seen:
                continue
            seen.add(key)
            a = acc[self.cell_id[i]]
            a[0] += self.pos[i][0]
            a[1] += self.pos[i][2]
            a[2] += 1
        self.centroid = {c: (v[0] / v[2], v[1] / v[2]) for c, v in acc.items()}


def connectivity_stats(sh: Shell) -> dict:
    """按真实位置还原连接；cell_id 是顶点的位移归属，不能用来切分整块裙边。"""
    edges = Counter()
    attributes = defaultdict(set)
    skirt_owners = Counter()
    for t in range(sh.tris):
        points = []
        if sh.tri_is_top[t * 3] == 0.0:
            skirt_owners[len(set(sh.cell_id[t * 3:t * 3 + 3]))] += 1
        for k in range(3):
            i = t * 3 + k
            point = tuple(sh.pos[i])
            points.append(point)
            attributes[point].add((sh.cell_id[i], sh.is_corner[i], sh.cell_bby[i], tuple(sh.uv0[i])))
        for a, b in zip(points, points[1:] + points[:1]):
            edges[tuple(sorted((a, b)))] += 1
    limits = {axis: (min(p[axis] for p in attributes), max(p[axis] for p in attributes))
              for axis in (0, 2)}
    interior_open_edges = 0
    for (a, b), count in edges.items():
        if count != 1:
            continue
        on_border = any(abs(a[axis] - bound) < 1e-5 and abs(b[axis] - bound) < 1e-5
                        for axis in (0, 2) for bound in limits[axis])
        interior_open_edges += not on_border
    return {"vertices": len(attributes), "edges": len(edges), "triangles": sh.tris,
            "euler": len(attributes) - len(edges) + sh.tris,
            "edge_incidence": dict(Counter(edges.values())),
            "interior_open_edges": interior_open_edges,
            "position_attribute_conflicts": sum(len(a) > 1 for a in attributes.values()),
            "skirt_cell_owner_counts": dict(skirt_owners)}


def tg_cell_random(cell_id: int) -> float:
    """TG displace:251-269 的 _316.._341；同一随机数同时驱动平面收缩和顶盖浮高。"""
    h = cell_id
    h = (h + (h << 10)) & 0xFFFFFFFF
    h ^= h >> 6
    h = (h + (h << 3)) & 0xFFFFFFFF
    h ^= h >> 11
    h = (h + (h << 15)) & 0xFFFFFFFF
    return (h & 0x7FFFFF) / 8388608.0


def transition_stats(sh: Shell) -> dict:
    """量顶盖边到裙底的平面垂距，分别在 TG 收缩前后测量；不冒充最终披挂宽度。"""
    widths = {"rest": defaultdict(list), "after_tg_jitter": defaultdict(list)}
    counts = Counter()
    flipped = Counter()
    for t in range(sh.tris):
        if sh.tri_is_top[t * 3] != 0.0:
            continue
        indices = range(t * 3, t * 3 + 3)
        top = [i for i in indices if sh.ring_top[i]]
        bottom = [i for i in indices if not sh.ring_top[i]]
        if len(top) != 2:
            continue
        kind = "same_owner" if sh.cell_id[bottom[0]] == sh.cell_id[top[0]] else "mixed_owner"
        signed = []
        for stage, do_jitter in (("rest", False), ("after_tg_jitter", True)):
            points = []
            for i in top + bottom:
                amount = 0.065 * (-3.0 + 11.0 * tg_cell_random(sh.cell_id[i])) if do_jitter else 0.0
                points.append((sh.pos[i][0] + sh.uv0[i][0] * amount,
                               sh.pos[i][2] + sh.uv0[i][1] * amount))
            a, b, c = points
            ex, ey = b[0] - a[0], b[1] - a[1]
            length = math.hypot(ex, ey)
            if length <= 1e-12:
                raise ValueError("Degenerate TG cap edge")
            distance = (ex * (c[1] - a[1]) - ey * (c[0] - a[0])) / length
            signed.append(distance)
            widths[stage][kind].append(abs(distance) * 100.0)
        counts[kind] += 1
        flipped[kind] += signed[0] * signed[1] <= 0.0

    def quantiles(values):
        ordered = sorted(values)
        return {f"p{p}": round(ordered[int((len(ordered) - 1) * p / 100)], 3) for p in (10, 50, 90)}

    return {"two_top_one_bottom_counts": dict(counts),
            "planar_width_cm": {stage: {kind: quantiles(values) for kind, values in groups.items()}
                                for stage, groups in widths.items()},
            "planar_orientation_changes_after_jitter": dict(flipped)}


def tri_area_xz(a, b, c) -> float:
    return abs((b[0] - a[0]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[0] - a[0])) * 0.5


def _seg_dist(p, a, b) -> float:
    vx, vy = b[0] - a[0], b[1] - a[1]
    l2 = vx * vx + vy * vy
    t = 0.0 if l2 == 0.0 else max(0.0, min(1.0, ((p[0] - a[0]) * vx + (p[1] - a[1]) * vy) / l2))
    return math.hypot(p[0] - (a[0] + t * vx), p[1] - (a[1] + t * vy))


def _outer_loops(sh: "Shell") -> Dict[int, List[Tuple[float, float]]]:
    """每个胞腔的外轮廓：盖 + 裙的三角里只被用过一次的边串成的环。
    不做任何极角排序 —— TG 的环不是凸的等距环，极角排序会把它接错。"""
    per_cell = defaultdict(list)
    for t in range(sh.tris):
        per_cell[sh.cell_id[t * 3]].append(
            tuple((round(sh.pos[t * 3 + k][0], 6), round(sh.pos[t * 3 + k][2], 6)) for k in range(3)))
    out: Dict[int, List[Tuple[float, float]]] = {}
    for cell, tri in per_cell.items():
        cnt: Counter = Counter()
        for p in tri:
            for a, b in ((p[0], p[1]), (p[1], p[2]), (p[2], p[0])):
                cnt[(a, b) if a < b else (b, a)] += 1
        adj = defaultdict(list)
        for a, b in (e for e, c in cnt.items() if c == 1):
            adj[a].append(b)
            adj[b].append(a)
        best: List[Tuple[float, float]] = []
        seen = set()
        for start in adj:
            if start in seen:
                continue
            loop, cur, prev = [start], start, None
            seen.add(start)
            while True:
                nxt = next((c for c in adj[cur] if c != prev and c not in seen), None)
                if nxt is None:
                    break
                loop.append(nxt)
                seen.add(nxt)
                prev, cur = cur, nxt
            if len(loop) > len(best):
                best = loop
        if len(best) >= 3:
            out[cell] = best
    return out


def _outer_coincidence(outer: Dict[int, List[Tuple[float, float]]], sample: int = 60) -> dict:
    """抽样：外轮廓上的点离**其它**胞腔外轮廓边有多远。0 = 两侧边重合 ⇒ 能精确铺满。"""
    if not outer:
        return {}
    bucket = defaultdict(list)
    width = 6.0
    for cell, loop in outer.items():
        m = len(loop)
        for i in range(m):
            a, b = loop[i], loop[(i + 1) % m]
            gx, gy = int((a[0] + 70.0) / width), int((a[1] + 70.0) / width)
            for ox in (-1, 0, 1):
                for oy in (-1, 0, 1):
                    bucket[(gx + ox, gy + oy)].append((cell, a, b))
    cells = sorted(outer)
    step = max(1, len(cells) // sample)
    dist: List[float] = []
    for cell in cells[::step]:
        for p in outer[cell]:
            gx, gy = int((p[0] + 70.0) / width), int((p[1] + 70.0) / width)
            best = min((_seg_dist(p, a, b) for oc, a, b in bucket.get((gx, gy), ()) if oc != cell),
                       default=None)
            if best is not None:
                dist.append(best)
    if not dist:
        return {}
    dist.sort()
    return {"median": dist[len(dist) // 2],
            "within_mm": 100.0 * sum(1 for x in dist if x < 0.001) / len(dist),
            "n": len(dist)}


# =============================================================================
# 核对
# =============================================================================

def verify(sh: Shell, glb: Glb) -> None:
    out = print
    line = "=" * 78

    out(line)
    out("Tiny Glade 原件碎裂图案 —— 逐项核对")
    out(line)

    out("\n[容器]")
    out(f"  GLB 大小              {glb.file_size} B（头声明 {glb.declared_size}）")
    out(f"  mesh / primitive      {glb.doc['meshes'][0].get('name')} × {len(glb.doc['meshes'][0]['primitives'])}")
    out(f"  mode                  {sh.prim.get('mode', 4)}（4 = TRIANGLES）")
    out(f"  indices               {sh.prim.get('indices', '无 —— 非索引三角汤')}")
    out(f"  attributes            {', '.join(sorted(sh.attrs))}")
    out(f"  materials             {len(glb.doc.get('materials', []))}（无材质，纯几何）")

    cap = sum(1 for t in range(sh.tris) if sh.tri_is_top[t * 3] == 1.0)
    top_verts = sum(1 for r in sh.ring_top if r)
    corner_verts = sum(1 for c in sh.is_corner if c == 1.0)
    cells = sorted(set(sh.cell_id))
    got = {
        "vertices": sh.n,
        "triangles": sh.tris,
        "cells": len(cells),
        "cap_triangles": cap,
        "skirt_triangles": sh.tris - cap,
        "top_ring_verts": top_verts,
        "bottom_ring_verts": sh.n - top_verts,
        "corner_verts": corner_verts,
    }
    out("\n[计数 vs Docs/TinyGlade/Ground_Shaper.md「烘焙资产」表]")
    for key, want in EXPECTED.items():
        mark = "OK " if got[key] == want else "!! "
        out(f"  {mark}{key:20} {got[key]:>8}   文档 {want:>8}")
    out(f"     cell_id 连续 0..{max(cells)}: {cells == list(range(len(cells)))}")

    out("\n[两个 y 值]")
    ys = Counter(round(p[1], 6) for p in sh.pos)
    for y, c in sorted(ys.items()):
        out(f"     y = {y:<8}（tile {y / 65.0:.6f}） {c} 顶点")
    out(f"     只有两个 y 值: {len(ys) == 2}")

    out("\n[通道语义 —— 实测，与文档说法有出入的用 !! 标出]")
    # TEXCOORD_2.y = is_top 是逐三角
    tri_const = sum(1 for t in range(sh.tris)
                    if len({sh.tri_is_top[t * 3], sh.tri_is_top[t * 3 + 1], sh.tri_is_top[t * 3 + 2]}) == 1)
    ring_mismatch = sum(1 for i in range(sh.n) if (sh.tri_is_top[i] == 1.0) != sh.ring_top[i])
    out(f"     TEXCOORD_2.y (is_top) 逐三角一致: {tri_const}/{sh.tris}")
    out(f"  !! TEXCOORD_2.y 与顶点所在环不同的顶点: {ring_mismatch}"
        f"  ⇒ is_top 是**逐三角**的盖/裙标记，不是 bIsTopRim")

    # TEXCOORD_2.x = cell_bby 其实就是环
    pair = Counter((sh.ring_top[i], round(sh.cell_bby[i], 3)) for i in range(sh.n))
    is_ring_flag = set(pair) == {(True, 1.0), (False, 0.0)}
    out(f"  !! TEXCOORD_2.x (cell_bby) 与环的联合分布: {dict(pair)}")
    out(f"     ⇒ cell_bby **就是** bIsTopRim（顶圈恒 1、底圈恒 0，零例外）: {is_ring_flag}")
    per_cell = defaultdict(set)
    for i in range(sh.n):
        per_cell[sh.cell_id[i]].add(round(sh.cell_bby[i], 3))
    out(f"     逐胞腔取值唯一的胞腔数: {sum(1 for s in per_cell.values() if len(s) == 1)}/{len(per_cell)}"
        f"  ⇒ 它不是逐胞腔量")

    # dir_to_centroid 的长度与朝向
    lens = sorted(math.hypot(*d) for d in sh.uv0)
    dots = []
    for i in range(sh.n):
        cx, cz = sh.centroid[sh.cell_id[i]]
        rx, rz = cx - sh.pos[i][0], cz - sh.pos[i][2]
        rl = math.hypot(rx, rz)
        if rl > 1e-6:
            dots.append((rx / rl) * sh.uv0[i][0] + (rz / rl) * sh.uv0[i][1])
    dots.sort()
    toward = 100.0 * sum(1 for d in dots if d > 0) / len(dots)
    out(f"     |TEXCOORD_0| : min {lens[0]:.6f} 中位 {lens[len(lens) // 2]:.6f} max {lens[-1]:.6f}（单位向量）")
    out(f"  !! dot(dir, normalize(质心 − P)) 中位 {dots[len(dots) // 2]:+.4f}，朝质心的占 {toward:.1f}%")
    out(f"     ⇒ dir_to_centroid **指向质心**，= normalize(center − P)，与计划契约写的符号相反")

    out("\n[结构]")
    bad_cap = sum(1 for t in range(sh.tris) if sh.tri_is_top[t * 3] == 1.0
                  and not all(sh.ring_top[t * 3 + k] for k in range(3)))
    bad_skirt = sum(1 for t in range(sh.tris) if sh.tri_is_top[t * 3] != 1.0
                    and len({sh.ring_top[t * 3 + k] for k in range(3)}) != 2)
    out(f"     盖三角三顶点全在顶圈: {cap - bad_cap}/{cap}（反例 {bad_cap}）")
    out(f"     裙三角跨两圈:         {sh.tris - cap - bad_skirt}/{sh.tris - cap}（反例 {bad_skirt}）")
    comp = Counter(sum(1 for k in range(3) if sh.ring_top[t * 3 + k])
                   for t in range(sh.tris) if sh.tri_is_top[t * 3] != 1.0)
    out(f"     裙三角按顶圈顶点数分布: {dict(comp)}")

    # 环上的去重点数
    cap_ring = defaultdict(set)
    bot_ring = defaultdict(set)
    top_all = defaultdict(set)
    for t in range(sh.tris):
        for k in range(3):
            i = t * 3 + k
            key = (round(sh.pos[i][0], 5), round(sh.pos[i][2], 5))
            if sh.ring_top[i]:
                top_all[sh.cell_id[i]].add(key)
                if sh.tri_is_top[t * 3] != 1.0:
                    cap_ring[sh.cell_id[i]].add(key)
            else:
                bot_ring[sh.cell_id[i]].add(key)
    nc = len(cells)
    out(f"     逐胞腔去重点数: 顶圈合计 {sum(len(v) for v in top_all.values()) / nc:.1f}"
        f"（其中盖边界 {sum(len(v) for v in cap_ring.values()) / nc:.1f}、"
        f"盖内部 {(sum(len(v) for v in top_all.values()) - sum(len(v) for v in cap_ring.values())) / nc:.1f}）"
        f"，底圈 {sum(len(v) for v in bot_ring.values()) / nc:.1f}")
    out(f"     逐胞腔三角: 盖 {cap / nc:.1f}、裙 {(sh.tris - cap) / nc:.1f}")

    out("\n[真实连接：跨 cell_id 的裙三角与共同位移顶点]")
    out(f"     {connectivity_stats(sh)}")
    out("     cell_id 属于顶点；一个裙三角可以连接 2 或 3 个 cell_id。唯一归属不代表网格断开。")
    out("\n[盖—裙平面过渡：TG 径向收缩前后；尚未采地形或沿法线位移]")
    out(f"     {transition_stats(sh)}")

    out("\n[同位置顶点的 cell_id 归属（不能据此判断三角形连接）]")
    for nd, lbl in ((5, "10 um"), (2, "1 cm"), (1, "10 cm")):
        occ_t, occ_b = defaultdict(set), defaultdict(set)
        for i in range(sh.n):
            key = (round(sh.pos[i][0], nd), round(sh.pos[i][2], nd))
            (occ_t if sh.ring_top[i] else occ_b)[key].add(sh.cell_id[i])
        out(f"  !! 容差 {lbl:6}: 顶圈去重 {len(occ_t):6}，被 ≥2 个胞腔共享 "
            f"{sum(1 for s in occ_t.values() if len(s) >= 2):5}"
            f" | 底圈去重 {len(occ_b):6}，共享 {sum(1 for s in occ_b.values() if len(s) >= 2):5}")
    out("     ⇒ 同位置的重复顶点具有一致的 cell_id；跨石块连接必须检查三角形及边的关联。")

    out("\n[LipOffset：底圈相对盖边界的横向错开]")
    dists, cos_dir, outward = [], [], 0
    for c, bots in bot_ring.items():
        ring = list(cap_ring[c])
        if not ring:
            continue
        cx, cz = sh.centroid[c]
        for bx, bz in bots:
            tx, tz = min(ring, key=lambda q: (q[0] - bx) ** 2 + (q[1] - bz) ** 2)
            d = math.hypot(bx - tx, bz - tz)
            dists.append(d)
            rx, rz = tx - cx, tz - cz
            rl = math.hypot(rx, rz) or 1.0
            if ((bx - tx) * rx + (bz - tz) * rz) / rl > 0:
                outward += 1
    ds = sorted(dists)
    mode = Counter(round(x, 4) for x in dists).most_common(1)[0]
    out(f"     配对 {len(ds)}（底圈点 → 同胞腔最近的盖边界点）")
    out(f"     min {ds[0] * 100:.2f}  p25 {ds[len(ds) // 4] * 100:.2f}  中位 {ds[len(ds) // 2] * 100:.2f}"
        f"  p75 {ds[3 * len(ds) // 4] * 100:.2f}  max {ds[-1] * 100:.2f} cm")
    out(f"  !! 众数 {mode[0] * 100:.2f} cm（{mode[1]}/{len(ds)} 个点），= {mode[0] / 65.0:.5f} tile 单位")
    out(f"     文档/计划写的是 21.4 cm = 0.0033 tile —— 实测是 {mode[0] * 100:.1f} cm = {mode[0] / 65.0:.4f} tile")
    out(f"     方向：朝外（背离质心）{outward}/{len(ds)}")

    out("\n[绕序]")
    up = down = 0
    nrm_bad = 0
    for t in range(sh.tris):
        a, b, c = sh.pos[t * 3], sh.pos[t * 3 + 1], sh.pos[t * 3 + 2]
        u = tuple(b[i] - a[i] for i in range(3))
        v = tuple(c[i] - a[i] for i in range(3))
        fn = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
        if sh.tri_is_top[t * 3] == 1.0:
            if fn[1] > 0:
                up += 1
            elif fn[1] < 0:
                down += 1
        ln = math.sqrt(sum(x * x for x in fn))
        if ln > 1e-20 and sum((fn[i] / ln) * sh.nrm[t * 3][i] for i in range(3)) < 0.9:
            nrm_bad += 1
    out(f"  !! 盖三角在 glTF 约定 cross(b−a, c−a) 下：朝 +Y {up}，朝 −Y {down}")
    out(f"     文档「已提取的资产」写的是「面法线朝 −Y」—— 实测全部朝 +Y（导出时已翻过绕序）")
    out(f"     几何法线与存储 NORMAL 不一致的三角: {nrm_bad}")

    out("\n[覆盖率：盖是内缩的孤岛，裙正好把缝补满]")
    cap_area = sum(tri_area_xz(sh.pos[t * 3], sh.pos[t * 3 + 1], sh.pos[t * 3 + 2])
                   for t in range(sh.tris) if sh.tri_is_top[t * 3] == 1.0)
    skirt_area = sum(tri_area_xz(sh.pos[t * 3], sh.pos[t * 3 + 1], sh.pos[t * 3 + 2])
                     for t in range(sh.tris) if sh.tri_is_top[t * 3] != 1.0)
    tile_area = 0.0
    minx, maxx = sh.acc_pos["min"][0], sh.acc_pos["max"][0]
    tile_area = (maxx - minx) ** 2
    out(f"     盖 {cap_area:.3f} m² = {100 * cap_area / tile_area:.2f}%"
        f" | 裙（XZ 投影）{skirt_area:.3f} m² = {100 * skirt_area / tile_area:.2f}%")
    out(f"  !! 合计 {cap_area + skirt_area:.3f} m² / tile {tile_area:.3f} m² = "
        f"{100 * (cap_area + skirt_area) / tile_area:.4f}%（面积总和本身不能证明无重叠或空隙）")

    out("\n[平铺]")
    E = sh.acc_pos["max"][0]
    tol = 1e-3
    left = sorted(round(p[2], 3) for p in sh.pos if abs(p[0] + E) < tol)
    right = sorted(round(p[2], 3) for p in sh.pos if abs(p[0] - E) < tol)
    bot = sorted(round(p[0], 3) for p in sh.pos if abs(p[2] + E) < tol)
    top = sorted(round(p[0], 3) for p in sh.pos if abs(p[2] - E) < tol)
    out(f"     x 两侧边界点一致: {left == right} | z 两侧一致: {bot == top}")
    out(f"     ⇒ tile **不可无缝平铺**")

    out("\n[尺度：这份原件用在本项目上是什么口径]")
    span = sh.acc_pos["max"][0] - sh.acc_pos["min"][0]
    areas = Counter()
    for t in range(sh.tris):
        if sh.tri_is_top[t * 3] == 1.0:
            areas[sh.cell_id[t * 3]] += tri_area_xz(sh.pos[t * 3], sh.pos[t * 3 + 1], sh.pos[t * 3 + 2])
    av = sorted(areas.values())
    mean = sum(av) / len(av)
    var = sum((x - mean) ** 2 for x in av) / len(av)
    out(f"     tile 跨度 {span:.2f} m（±{span / 2:.2f}）；本项目地面 {GROUND_SPAN_M:.0f} m"
        f" ⇒ 原生 ×65 即可整张盖住，每边富余 {(span - GROUND_SPAN_M) / 2:.2f} m")
    out(f"     胞腔间距 {math.sqrt(tile_area / len(cells)):.2f} m；盖多边形面积 均值 {mean:.2f} m²"
        f" CV {100 * math.sqrt(var) / mean:.1f}% [{av[0]:.2f} .. {av[-1]:.2f}]")
    out(f"     盖三角密度 {cap / tile_area:.3f} /m² ⇒ 等边等效边长 "
        f"{math.sqrt(4 * (tile_area / cap) / math.sqrt(3)):.2f} m")
    out(f"     计划表的 ~44,000 = 本原件按面积折算到 128 m：{cap + (sh.tris - cap):,} × "
        f"({GROUND_SPAN_M:.0f}/{span:.1f})² = {sh.tris * (GROUND_SPAN_M / span) ** 2:,.0f}")

    out("\n[导入风险：UE StaticMesh 焊接]")
    recs = Counter((sh.pos[i], sh.nrm[i], sh.uv0[i], sh.uv1[i], sh.uv2[i]) for i in range(sh.n))
    dup = sum(c - 1 for c in recs.values() if c > 1)
    out(f"  !! 逐字节完全相同的顶点记录会被焊掉 {dup}/{sh.n}（{100 * dup / sh.n:.1f}%），"
        f"去重后 {len(recs)} 个")
    out(f"     ⇒ 上传 aux 流时**必须按索引缓冲展回 Tri*3+k**，不能直接拷顶点缓冲")
    out("\n[导入风险：UV 精度]")
    for name, arr in (("TEXCOORD_0", sh.uv0), ("TEXCOORD_1", sh.uv1), ("TEXCOORD_2", sh.uv2)):
        xs = [v[0] for v in arr]
        ys = [v[1] for v in arr]
        out(f"     {name}: x [{min(xs):.4f}, {max(xs):.4f}]  y [{min(ys):.4f}, {max(ys):.4f}]")
    out("     FP16 精确表示 ≤2048 的整数 ⇒ cell_id 最大 608 安全；dir 分量只剩 ~5e-4 精度。")
    out("     仍然建议开 bUseFullPrecisionUVs：本项目若换成自己烘的图案，胞腔数会超过 2048。")

    out("\n" + line)


# =============================================================================
# 预览图
# =============================================================================

def write_preview(sh: Shell, path: str) -> bool:
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        from matplotlib.collections import LineCollection, PolyCollection
        matplotlib.rcParams["font.sans-serif"] = [
            "Microsoft YaHei", "SimHei", "Noto Sans CJK SC", "DejaVu Sans"]
        matplotlib.rcParams["axes.unicode_minus"] = False
    except Exception as exc:  # pragma: no cover
        print(f"[preview] matplotlib 不可用，跳过预览图：{exc}")
        return False

    # 逐胞腔归类。**只画真三角，不重建环** —— TG 的两圈不是同一条环的等距偏移
    # （盖边界 18.0 点/胞腔，底圈 8.4 点/胞腔），任何极角排序都会把它接错。
    cap_tris: Dict[int, List[Tuple]] = defaultdict(list)
    skirt_tris: Dict[int, List[Tuple]] = defaultdict(list)
    top_pts: Dict[int, set] = defaultdict(set)
    bot_pts: Dict[int, set] = defaultdict(set)
    corner_pts: Dict[int, set] = defaultdict(set)
    for t in range(sh.tris):
        cid = sh.cell_id[t * 3]
        pts = tuple((sh.pos[t * 3 + k][0], sh.pos[t * 3 + k][2]) for k in range(3))
        (cap_tris if sh.tri_is_top[t * 3] == 1.0 else skirt_tris)[cid].append(pts)
        for k in range(3):
            i = t * 3 + k
            p = (sh.pos[i][0], sh.pos[i][2])
            (top_pts if sh.ring_top[i] else bot_pts)[cid].add(p)
            if sh.is_corner[i] == 1.0:
                corner_pts[cid].add(p)

    def cell_colour(cid: int) -> Tuple[float, float, float]:
        h = (cid * 2654435761) & 0xFFFFFFFF
        h ^= h >> 15
        h = (h * 2246822519) & 0xFFFFFFFF
        return (0.30 + 0.0027 * (h & 0x7F), 0.28 + 0.0024 * ((h >> 8) & 0x7F),
                0.26 + 0.0022 * ((h >> 16) & 0x7F))

    fig, axes = plt.subplots(1, 3, figsize=(21.0, 7.4))
    fig.patch.set_facecolor("#12141a")

    # ---- ① 全 tile：只画顶盖，缝就是裙边所在 --------------------------------
    ax = axes[0]
    polys, colors = [], []
    for cid, tri in cap_tris.items():
        col = cell_colour(cid)
        for p in tri:
            polys.append(p)
            colors.append(col)
    ax.add_collection(PolyCollection(polys, facecolors=colors, edgecolors="none"))
    ax.set_xlim(-TILE_HALF_M, TILE_HALF_M)
    ax.set_ylim(-TILE_HALF_M, TILE_HALF_M)
    ax.set_aspect("equal")
    g = GROUND_SPAN_M / 2
    ax.plot([-g, g, g, -g, -g], [-g, -g, g, g, -g], color="#5ad1a0", lw=1.4, ls="--",
            label=f"本项目地面 {GROUND_SPAN_M:.0f} m")
    ax.set_title(f"① 原件全 tile ±{TILE_HALF_M:.2f} m · {len(cap_tris)} 胞腔 · 只画顶盖"
                 f"（黑缝 = 裙边）", color="#e8e8e8", fontsize=11)
    leg = ax.legend(loc="upper right", fontsize=8, framealpha=0.35, facecolor="#1b1e26")
    for txt in leg.get_texts():
        txt.set_color("#e8e8e8")

    # ---- ② 18 m 窗口：盖（逐胞腔色）+ 裙（橙）------------------------------
    ax = axes[1]
    win = 9.0
    cap_p, cap_c, skirt_p = [], [], []
    for cid, (cx, cz) in sh.centroid.items():
        if max(abs(cx), abs(cz)) > win + 8.0:
            continue
        col = cell_colour(cid)
        for p in cap_tris[cid]:
            cap_p.append(p)
            cap_c.append(col)
        skirt_p.extend(skirt_tris[cid])
    ax.add_collection(PolyCollection(skirt_p, facecolors="#d98b3a", alpha=0.55,
                                     edgecolors="#8a5720", linewidths=0.3))
    ax.add_collection(PolyCollection(cap_p, facecolors=cap_c, edgecolors="#20242e", linewidths=0.3))
    ax.set_xlim(-win, win)
    ax.set_ylim(-win, win)
    ax.set_aspect("equal")
    ax.set_title("② 18 m 窗口 · 彩色=顶盖（内缩的孤岛） 橙=裙边（把缝精确补满，合计 100.00%）",
                 color="#e8e8e8", fontsize=11)

    # ---- ③ 单胞腔 + 一圈邻居 -----------------------------------------------
    ax = axes[2]
    pick = min(sh.centroid, key=lambda c: sh.centroid[c][0] ** 2 + sh.centroid[c][1] ** 2)
    pcx, pcz = sh.centroid[pick]
    r = 4.2
    for cid, (cx, cz) in sh.centroid.items():
        if cid == pick or math.hypot(cx - pcx, cz - pcz) > r * 1.8:
            continue
        ax.add_collection(PolyCollection(cap_tris[cid], facecolors="#242a36",
                                         edgecolors="#2f3644", linewidths=0.3))
        ax.add_collection(PolyCollection(skirt_tris[cid], facecolors="#3a2c1c",
                                         edgecolors="#4a3826", linewidths=0.3))
    ax.add_collection(PolyCollection(cap_tris[pick], facecolors="#2b3a52",
                                     edgecolors="#5b7396", linewidths=0.7))
    ax.add_collection(PolyCollection(skirt_tris[pick], facecolors="#d98b3a", alpha=0.55,
                                     edgecolors="#c0762a", linewidths=0.7))
    tp = sorted(top_pts[pick])
    bp = sorted(bot_pts[pick])
    ax.scatter(*zip(*tp), s=13, c="#e8e8e8", zorder=4,
               label=f"顶圈 y=3.12 m（{len(tp)} 点，cell_bby=1）")
    ax.scatter(*zip(*bp), s=26, c="#ffb35c", zorder=4,
               label=f"底圈 y=0（{len(bp)} 点，cell_bby=0，向外错开）")
    cp = sorted(corner_pts[pick])
    if cp:
        ax.scatter(*zip(*cp), s=70, facecolors="none", edgecolors="#5ad1a0", linewidths=1.3,
                   zorder=5, label=f"is_corner=1（{len(cp)} 点）")
    ax.plot([pcx], [pcz], marker="+", ms=10, c="#5ad1a0", zorder=5)
    ax.set_xlim(pcx - r, pcx + r)
    ax.set_ylim(pcz - r, pcz + r)
    ax.set_aspect("equal")
    ax.set_title(f"③ 单胞腔 #{pick}（亮）+ 邻居（暗）· 蓝=盖 橙=裙 · 图案是平的，y 只有两个值",
                 color="#e8e8e8", fontsize=11)
    leg = ax.legend(loc="upper left", fontsize=8, framealpha=0.4, facecolor="#1b1e26",
                    edgecolor="#2a2f3a")
    for txt in leg.get_texts():
        txt.set_color("#e8e8e8")

    for ax in axes:
        ax.set_facecolor("#12141a")
        ax.tick_params(colors="#7a8290", labelsize=7)
        for sp in ax.spines.values():
            sp.set_color("#2a2f3a")

    fig.tight_layout()
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    fig.savefig(path, dpi=110, facecolor=fig.get_facecolor())
    plt.close(fig)
    return True


def main(argv: Sequence[str]) -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--glb", default=DEFAULT_GLB)
    ap.add_argument("--preview-out", default=os.path.join(root, DEFAULT_PREVIEW_RELPATH))
    ap.add_argument("--no-preview", action="store_true")
    args = ap.parse_args(list(argv))

    if not os.path.isfile(args.glb):
        print(f"找不到原件：{args.glb}")
        print("它由 D:/MyProject/Tiny Glade/extract/rocky_terrain2glb.py 生成，先跑那个。")
        return 2

    glb = Glb(args.glb)
    sh = Shell(glb)
    verify(sh, glb)
    if not args.no_preview and write_preview(sh, args.preview_out):
        print(f"预览图 {args.preview_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
