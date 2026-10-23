#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""披挂岩壳碎裂图案的**后备**生成器 —— 正式路线用的是 Tiny Glade 原件，不是这个。

⚠️ **先看这一条**：链 B 的碎裂图案**已经有原件**，在
``D:/MyProject/Tiny Glade/extracted/meshes/rocky_terrain_shell.glb``
（TG 自己的那张：49,598 三角 / 148,794 顶点 / 609 胞腔）。
导入与接线见 ``Docs/TinyGlade/RockShell_Pattern.md``，导入脚本是 ``TinyGladeImportRockShell.py``，
核对脚本是 ``VerifyRockShellGlb.py``。**默认走原件。**

本脚本是**原件不可用时的后备**，只在下面两种情况下才有价值：

1. 需要原件给不了的尺度 —— 原件的胞腔固定在 ~5.5 m，tile 固定 136.5 m 且**不可无缝平铺**；
   如果哪天定下要 ~3 m 胞腔（计划 D9 密度表的原始口径），原件做不到，只能自己烘。
2. 需要重新分发而不便携带 TG 的提取物。

产出一张**平的** 2D 碎裂图案：顶圈（cap）+ 横向错开 ``LipOffset`` 的底圈（skirt）两层，
逐三角展开（顶点不共享，``Tri*3 + k`` 布局），写成紧凑二进制供
``FCSMeshResident::AddStream`` 的两条 ``ECSGpuStreamRole::AuxVertex`` 流直接 memcpy 上传。

**与原件的三处已知形态差异**（实测，见 ``Docs/TinyGlade/RockShell_Pattern.md``「原件 vs 后备」）：
本脚本的顶盖直接铺到 Voronoi 边界、裙边向外压在邻居盖上；原件的顶盖是**内缩的孤岛**，
裙边精确填满缝（盖 86.60% + 裙 13.40% = 100.0000%）。本脚本的 ``DirToCentroid`` 按计划契约
写成**朝外**；原件实测是**朝质心**。本脚本相邻胞腔共享角点坐标；原件一个顶点都不共享。

数据契约见 ``Plan_House.md`` 的 D9「侧面碎石：Tiny Glade 式披挂岩壳 / 数据契约」，
字段与本文件 :class:`BakedVertex` 一一对应；文件布局与接线见 ``Docs/TinyGlade/RockShell_Pattern.md``。

**这里不产生任何 Z / 高度。** 三维形态 100% 由运行时披挂产生（计划裁决一）；
裙边高度是「两圈各自采自己 XY 的高度」的副产品（裁决四），烘焙侧一行都不算它。

⚠️ **2026-08-31 起，本脚本的产物与此前烘出来的 .bin 不再逐位相同。**
唯一的改动是 ``LIP_OFFSET_CM`` 由 21.4 订正为 19.5 —— 21.4 是从计划正文抄来的旧口径，
而离线核对器 ``VerifyRockShellGlb.py`` 在**原件**上实测出来的是 19.50 cm
（众数 3,910 / 5,131 个点，p25 = 中位 = p75 = 19.50，= 0.00300 tile）。
它进的是底圈每个点的 XY 坐标 ⇒ 顶点缓冲整体改变 ⇒ 校验和、预览图、下游任何"与上一版逐位比对"
的核对都会报不同。**这是有意的**：后备图案本来就该与原件同口径，不同才是缺陷。
需要旧产物时不要改这个常数，直接从版本库取旧的 .bin。
（运行时不读这个常数 —— 它已经烘进坐标里，所以走原件的正式路线一个字节都不受影响。）

依赖：只用标准库产数据（``matplotlib`` 仅用于可选的预览图）。
固定种子 ⇒ 重跑逐位相同（PRNG 为自带的 splitmix64，几何全走 Python float64）。

用法::

    python BakeRockShellPattern.py                  # 写 .bin + 预览图
    python BakeRockShellPattern.py --no-preview     # 只写 .bin
    python BakeRockShellPattern.py --out <path.bin> --preview-out <path.png>
"""

from __future__ import annotations

import argparse
import math
import os
import struct
import sys
import time
import zlib
from typing import Dict, List, Sequence, Tuple

# =============================================================================
# 参数（集中可调；改任何一条都会改变输出的逐位内容）
# =============================================================================

# --- 世界尺度（厘米，UE 单位；相对地面原点，中心对称）-------------------------
# 128 m = ACSGroundActor 的 NumCells 256 × CellSize 50。计划 D9「密度与尺度」表。
WORLD_SPAN_CM = 12800.0

# 胞腔标称尺寸。1,820 个胞腔 = (12800/300)^2，即计划表的「~3 m（~1,820 个）」。
CELL_SIZE_CM = 300.0

# 三角边长目标。1.2 m = 与 Tiny Glade 同**绝对**密度（TG 实测 1.25 m，见 Docs/TinyGlade/Ground_Shaper.md）。
TRI_EDGE_CM = 120.0

# 底圈相对顶圈的**横向**错开量。
# 方向 = DirToCentroid（由胞腔质心指向该点，向外），与原型 attribwrangle3 的
# `@P += normalize(@P - v@center) * offset` 同向。
#
# ⚠️ 2026-08-31 从 21.4 订正为 19.5（见文件头「与旧产物不再逐位相同」那一条）。
# 21.4 = 0.0033 tile 是**从计划正文抄来的**旧口径，从来没有人在原件上量过；
# 离线核对器 `VerifyRockShellGlb.py` 直接量底圈点到同胞腔盖边界的最近距离，
# 结果是**众数 19.50 cm（3,910 / 5,131 个点）、p25 = 中位 = p75 = 19.50**，
# = 0.00300 tile，方向朝外 5,131 / 5,131。差 9%，且分布是一根尖峰而不是散布 ——
# 这不是"两个都对的取值"，是旧口径量错了。
LIP_OFFSET_CM = 19.5

# --- 图案生成 -----------------------------------------------------------------
# Lloyd 松弛迭代数。0 = 纯随机 Voronoi（胞腔大小极不均）；6 = 大小均匀但形状仍不规则；
# >30 会向正六边形晶格收敛，一眼看出是程序化的 —— 这正是要避开的下限。
LLOYD_ITERATIONS = 6

# 胞腔内部点的 hex 晶格离边界的最小距离（× TRI_EDGE_CM）。太小会在边界附近生出薄三角。
INTERIOR_MARGIN_FRAC = 0.55

# 内部点的随机抖动幅度（× TRI_EDGE_CM）。抹掉逐胞腔 hex 晶格的可辨认感。
INTERIOR_JITTER_FRAC = 0.12

# 焊接容差（cm）。同一个 Voronoi 顶点由相邻胞腔各自裁剪算出，浮点差 ~1e-9 cm；
# 焊到同一坐标是「相邻胞腔共享角不裂开」的前提（对应 TG 的 is_corner 钉死）。
WELD_TOL_CM = 1.0e-3

# --- 绕序 ---------------------------------------------------------------------
# 默认按**计划 kernel 骨架**的口径出三角：`normalize(cross(P[2]-P[0], P[1]-P[0]))` 朝外
# （顶盖朝 +Z、裙边朝径向外）。这与 TG 的 `cross(v2-v0, v1-v0)` 逐字同源。
# 若 P1 实测发现壳是内外翻的（从上方看不见），翻这一个 bool 重烘即可。
FLIP_WINDING = False

# --- 随机种子 -----------------------------------------------------------------
SEED = 0x54474C41  # 'TGLA'

# --- 输出 ---------------------------------------------------------------------
FORMAT_MAGIC = b"CSRS"     # CS Rock Shell
FORMAT_VERSION = 1
HEADER_BYTES = 64

# 后备产物不进版本控制：本脚本 0.7 s 就能重跑出逐位相同的结果，没必要压一个 3.4 MB 的 blob。
# 正式路线的预览图是 Docs/TinyGlade/RockShell_Pattern.preview.png（原件），别覆盖它。
DEFAULT_BIN_RELPATH = os.path.join("Intermediate", "RockShell", "CSRockShellPattern.bin")
DEFAULT_PREVIEW_RELPATH = os.path.join("Docs", "TinyGlade", "RockShell_Pattern.fallback.preview.png")

# 打包位（blob B 的 uint32）
CELL_ID_BITS = 24
CELL_ID_MASK = (1 << CELL_ID_BITS) - 1
BIT_TOP_RIM = 1 << 24
BIT_CORNER = 1 << 25
# bit 26..31 保留（写 0）。见 Docs/TinyGlade/RockShell_Pattern.md「留给 kernel 作者的一个坑」。

# flags 位（header 0x0C）
FLAG_TRIANGLE_SOUP = 1 << 0   # 顶点不共享，Tri*3+k
FLAG_WINDING_CROSS_20_10 = 1 << 1  # cross(P2-P0, P1-P0) 朝外


# =============================================================================
# 确定性 PRNG（splitmix64，纯整数；不依赖 random / numpy 的版本行为）
# =============================================================================

_U64 = (1 << 64) - 1


class SplitMix64:
    __slots__ = ("state",)

    def __init__(self, seed: int) -> None:
        self.state = seed & _U64

    def next_u64(self) -> int:
        self.state = (self.state + 0x9E3779B97F4A7C15) & _U64
        z = self.state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & _U64
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & _U64
        return (z ^ (z >> 31)) & _U64

    def unit(self) -> float:
        """[0, 1) 的 float64，取 u64 高 53 位。"""
        return (self.next_u64() >> 11) * (1.0 / (1 << 53))

    def sym(self) -> float:
        """(-1, 1) 的 float64。"""
        return self.unit() * 2.0 - 1.0


def stream_rng(seed: int, stream: int) -> SplitMix64:
    """从主种子派生一条独立子流；子流之间互不影响，改一处不会扰动全局。"""
    return SplitMix64((seed * 0x9E3779B97F4A7C15 + stream * 0xD1B54A32D192ED03) & _U64)


# =============================================================================
# 2D 几何基元
# =============================================================================

Pt = Tuple[float, float]


def poly_area_centroid(poly: Sequence[Pt]) -> Tuple[float, float, float]:
    """返回 (cx, cy, signed_area)。CCW 多边形面积为正。"""
    a2 = 0.0
    cx = 0.0
    cy = 0.0
    n = len(poly)
    for i in range(n):
        x0, y0 = poly[i]
        x1, y1 = poly[(i + 1) % n]
        cross = x0 * y1 - x1 * y0
        a2 += cross
        cx += (x0 + x1) * cross
        cy += (y0 + y1) * cross
    if abs(a2) < 1e-12:
        return poly[0][0], poly[0][1], 0.0
    return cx / (3.0 * a2), cy / (3.0 * a2), 0.5 * a2


def clip_halfplane(poly: List[Pt], nx: float, ny: float, c: float) -> List[Pt]:
    """Sutherland–Hodgman：保留满足 nx*x + ny*y <= c 的一侧。"""
    out: List[Pt] = []
    n = len(poly)
    if n == 0:
        return out
    ax, ay = poly[-1]
    da = nx * ax + ny * ay - c
    for i in range(n):
        bx, by = poly[i]
        db = nx * bx + ny * by - c
        if db <= 0.0:
            if da > 0.0:
                t = da / (da - db)
                out.append((ax + (bx - ax) * t, ay + (by - ay) * t))
            out.append((bx, by))
        elif da <= 0.0:
            t = da / (da - db)
            out.append((ax + (bx - ax) * t, ay + (by - ay) * t))
        ax, ay, da = bx, by, db
    return out


# =============================================================================
# 站点 + Voronoi（半平面裁剪法；胞腔天生凸，裁剪是精确解）
# =============================================================================

class SiteGrid:
    """均匀桶格，用来取「某点附近的站点」。"""

    def __init__(self, sites: Sequence[Pt], half: float, bucket: float) -> None:
        self.half = half
        self.bucket = bucket
        self.dim = max(1, int(math.ceil(2.0 * half / bucket)))
        self.cells: Dict[int, List[int]] = {}
        for idx, (x, y) in enumerate(sites):
            self.cells.setdefault(self._key(x, y), []).append(idx)

    def _coord(self, v: float) -> int:
        i = int((v + self.half) / self.bucket)
        return min(max(i, 0), self.dim - 1)

    def _key(self, x: float, y: float) -> int:
        return self._coord(x) * self.dim + self._coord(y)

    def near(self, x: float, y: float, rings: int) -> List[int]:
        cx = self._coord(x)
        cy = self._coord(y)
        found: List[int] = []
        for gx in range(max(0, cx - rings), min(self.dim - 1, cx + rings) + 1):
            base = gx * self.dim
            for gy in range(max(0, cy - rings), min(self.dim - 1, cy + rings) + 1):
                bucket = self.cells.get(base + gy)
                if bucket:
                    found.extend(bucket)
        return found


def voronoi_cell(sites: Sequence[Pt], index: int, grid: SiteGrid, half: float) -> List[Pt]:
    """站点 index 的 Voronoi 胞腔，已裁剪到 [-half, half]^2。"""
    px, py = sites[index]
    poly: List[Pt] = [(-half, -half), (half, -half), (half, half), (-half, half)]

    rings = 2
    while True:
        cand = grid.near(px, py, rings)
        ordered = []
        for j in cand:
            if j == index:
                continue
            sx, sy = sites[j]
            dx = sx - px
            dy = sy - py
            ordered.append((dx * dx + dy * dy, j))
        ordered.sort()

        poly = [(-half, -half), (half, -half), (half, half), (-half, half)]
        covered = False
        for d2, j in ordered:
            sx, sy = sites[j]
            dx = sx - px
            dy = sy - py
            # 保留 (q - mid)·d <= 0 的一侧（离 px,py 更近的一侧）
            mx = 0.5 * (px + sx)
            my = 0.5 * (py + sy)
            poly = clip_halfplane(poly, dx, dy, mx * dx + my * dy)
            if len(poly) < 3:
                covered = True  # 胞腔被邻居完全吃掉（站点重合），再扩搜索环也没用
                break
            far2 = 0.0
            for qx, qy in poly:
                r2 = (qx - px) ** 2 + (qy - py) ** 2
                if r2 > far2:
                    far2 = r2
            # 后续站点距离 >= sqrt(d2)，其分界线到 p 的距离 >= sqrt(d2)/2。
            if 4.0 * far2 <= d2:
                covered = True
                break
        else:
            covered = False

        if covered or rings >= grid.dim:
            return poly
        rings *= 2


def build_sites(seed: int, count: int, half: float) -> List[Pt]:
    rng = stream_rng(seed, 1)
    span = 2.0 * half
    return [(rng.unit() * span - half, rng.unit() * span - half) for _ in range(count)]


def lloyd_relax(sites: List[Pt], half: float, iterations: int, bucket: float) -> List[Pt]:
    for _ in range(iterations):
        grid = SiteGrid(sites, half, bucket)
        moved: List[Pt] = []
        for i in range(len(sites)):
            poly = voronoi_cell(sites, i, grid, half)
            if len(poly) < 3:
                moved.append(sites[i])
                continue
            cx, cy, area = poly_area_centroid(poly)
            moved.append(sites[i] if area <= 0.0 else (cx, cy))
        sites = moved
    return sites


# =============================================================================
# 顶点焊接（保证相邻胞腔的共享角是**同一个**坐标）
# =============================================================================

class Welder:
    def __init__(self, tol: float) -> None:
        self.tol = tol
        self.inv = 1.0 / tol
        self.buckets: Dict[Tuple[int, int], List[int]] = {}
        self.points: List[Pt] = []

    def weld(self, x: float, y: float) -> int:
        kx = int(math.floor(x * self.inv))
        ky = int(math.floor(y * self.inv))
        tol2 = self.tol * self.tol
        for ox in (-1, 0, 1):
            for oy in (-1, 0, 1):
                for idx in self.buckets.get((kx + ox, ky + oy), ()):
                    qx, qy = self.points[idx]
                    if (qx - x) ** 2 + (qy - y) ** 2 <= tol2:
                        return idx
        idx = len(self.points)
        self.points.append((x, y))
        self.buckets.setdefault((kx, ky), []).append(idx)
        return idx


# =============================================================================
# Delaunay（Bowyer–Watson，逐胞腔小规模；胞腔是凸的 ⇒ 凸包 == 胞腔多边形）
# =============================================================================

def _circumcircle(ax, ay, bx, by, cx, cy):
    d = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by))
    if d == 0.0:
        return None
    a2 = ax * ax + ay * ay
    b2 = bx * bx + by * by
    c2 = cx * cx + cy * cy
    ux = (a2 * (by - cy) + b2 * (cy - ay) + c2 * (ay - by)) / d
    uy = (a2 * (cx - bx) + b2 * (ax - cx) + c2 * (bx - ax)) / d
    return ux, uy, (ux - ax) ** 2 + (uy - ay) ** 2


def delaunay(points: Sequence[Pt]) -> List[Tuple[int, int, int]]:
    """返回点集的 Delaunay 三角形（索引三元组，CCW）。点应已平移到原点附近。"""
    n = len(points)
    if n < 3:
        return []

    minx = min(p[0] for p in points)
    maxx = max(p[0] for p in points)
    miny = min(p[1] for p in points)
    maxy = max(p[1] for p in points)
    dx = max(maxx - minx, 1.0)
    dy = max(maxy - miny, 1.0)
    dmax = 20.0 * max(dx, dy)
    midx = 0.5 * (minx + maxx)
    midy = 0.5 * (miny + maxy)

    pts = list(points) + [
        (midx - dmax, midy - dmax),
        (midx + dmax, midy - dmax),
        (midx, midy + dmax),
    ]

    tris: List[Tuple[int, int, int]] = [(n, n + 1, n + 2)]
    circles = {}
    circles[(n, n + 1, n + 2)] = _circumcircle(*pts[n], *pts[n + 1], *pts[n + 2])

    for i in range(n):
        px, py = pts[i]
        bad: List[Tuple[int, int, int]] = []
        for tri in tris:
            circle = circles.get(tri)
            if circle is None:
                bad.append(tri)
                continue
            ux, uy, r2 = circle
            if (px - ux) ** 2 + (py - uy) ** 2 < r2:
                bad.append(tri)
        if not bad:
            continue

        edge_count: Dict[Tuple[int, int], int] = {}
        for a, b, c in bad:
            for e in ((a, b), (b, c), (c, a)):
                key = (e[0], e[1]) if e[0] < e[1] else (e[1], e[0])
                edge_count[key] = edge_count.get(key, 0) + 1
        boundary = [e for e, k in edge_count.items() if k == 1]

        bad_set = set(bad)
        tris = [t for t in tris if t not in bad_set]
        for t in bad:
            circles.pop(t, None)

        for a, b in boundary:
            ax, ay = pts[a]
            bx, by = pts[b]
            # 统一成 CCW，circumcircle 与后续朝向都靠它
            if (bx - ax) * (py - ay) - (by - ay) * (px - ax) < 0.0:
                a, b = b, a
            tri = (a, b, i)
            tris.append(tri)
            circles[tri] = _circumcircle(*pts[tri[0]], *pts[tri[1]], *pts[tri[2]])

    return [t for t in tris if t[0] < n and t[1] < n and t[2] < n]


# =============================================================================
# 逐胞腔建网格
# =============================================================================

def subdivide_count(length: float, target: float) -> int:
    return max(1, int(round(length / target)))


def hex_lattice_points(poly: Sequence[Pt], centroid: Pt, spacing: float,
                       margin: float, jitter: float, rng: SplitMix64) -> List[Pt]:
    """胞腔内部的 hex 晶格点。晶格原点挂在**本胞腔质心**上并逐胞腔随机相位，
    避免全场出现一张可辨认的公共晶格。"""
    minx = min(p[0] for p in poly)
    maxx = max(p[0] for p in poly)
    miny = min(p[1] for p in poly)
    maxy = max(p[1] for p in poly)

    row_h = spacing * math.sqrt(3.0) * 0.5
    ox = centroid[0] + (rng.unit() - 0.5) * spacing
    oy = centroid[1] + (rng.unit() - 0.5) * row_h

    n = len(poly)
    # 预算每条边的向内法线（CCW 多边形 ⇒ 内侧法线是 (-dy, dx) 归一化）
    edges = []
    for i in range(n):
        x0, y0 = poly[i]
        x1, y1 = poly[(i + 1) % n]
        ex = x1 - x0
        ey = y1 - y0
        el = math.hypot(ex, ey)
        if el <= 0.0:
            continue
        edges.append((x0, y0, -ey / el, ex / el))

    out: List[Pt] = []
    row_lo = int(math.floor((miny - oy) / row_h)) - 1
    row_hi = int(math.ceil((maxy - oy) / row_h)) + 1
    for r in range(row_lo, row_hi + 1):
        py = oy + r * row_h
        shift = 0.5 * spacing if (r & 1) else 0.0
        col_lo = int(math.floor((minx - ox - shift) / spacing)) - 1
        col_hi = int(math.ceil((maxx - ox - shift) / spacing)) + 1
        for c in range(col_lo, col_hi + 1):
            px = ox + shift + c * spacing
            jx = px + rng.sym() * jitter
            jy = py + rng.sym() * jitter
            ok = True
            for x0, y0, nx, ny in edges:
                if (jx - x0) * nx + (jy - y0) * ny < margin:
                    ok = False
                    break
            if ok:
                out.append((jx, jy))
    return out


class BakedVertex:
    """与计划 D9 `CSRockShell::FBakedVertex` 一一对应（**没有 Z**）。"""
    __slots__ = ("rest_x", "rest_y", "dir_x", "dir_y", "cell_id", "is_top_rim", "is_corner")

    def __init__(self, rest_x, rest_y, dir_x, dir_y, cell_id, is_top_rim, is_corner):
        self.rest_x = rest_x
        self.rest_y = rest_y
        self.dir_x = dir_x
        self.dir_y = dir_y
        self.cell_id = cell_id
        self.is_top_rim = is_top_rim
        self.is_corner = is_corner


def dir_to_centroid(x: float, y: float, cx: float, cy: float) -> Tuple[float, float]:
    dx = x - cx
    dy = y - cy
    d = math.hypot(dx, dy)
    if d <= 1e-9:
        return 0.0, 0.0
    return dx / d, dy / d


def build_pattern(seed: int) -> dict:
    t0 = time.time()
    half = 0.5 * WORLD_SPAN_CM
    target_cells = int(round((WORLD_SPAN_CM / CELL_SIZE_CM) ** 2))
    bucket = CELL_SIZE_CM * 2.0

    sites = build_sites(seed, target_cells, half)
    sites = lloyd_relax(sites, half, LLOYD_ITERATIONS, bucket)
    t_sites = time.time()

    grid = SiteGrid(sites, half, bucket)
    raw_cells = [voronoi_cell(sites, i, grid, half) for i in range(len(sites))]

    # --- 焊接：让相邻胞腔的共享角变成**同一个**坐标 ---------------------------
    welder = Welder(WELD_TOL_CM)
    cell_corner_ids: List[List[int]] = []
    for poly in raw_cells:
        ids: List[int] = []
        for x, y in poly:
            wid = welder.weld(x, y)
            if not ids or ids[-1] != wid:
                ids.append(wid)
        if len(ids) > 1 and ids[0] == ids[-1]:
            ids.pop()
        cell_corner_ids.append(ids)

    corner_cell_count: Dict[int, int] = {}
    for ids in cell_corner_ids:
        for wid in set(ids):
            corner_cell_count[wid] = corner_cell_count.get(wid, 0) + 1
    junction_count = sum(1 for v in corner_cell_count.values() if v >= 3)

    # --- 共享边细分：按 (min_id, max_id) 缓存，两侧拿到逐位相同的插点 ---------
    edge_cache: Dict[Tuple[int, int], List[Pt]] = {}

    def edge_points(a: int, b: int) -> List[Pt]:
        key = (a, b) if a < b else (b, a)
        cached = edge_cache.get(key)
        if cached is None:
            p0 = welder.points[key[0]]
            p1 = welder.points[key[1]]
            length = math.hypot(p1[0] - p0[0], p1[1] - p0[1])
            n = subdivide_count(length, TRI_EDGE_CM)
            cached = [(p0[0] + (p1[0] - p0[0]) * (k / n),
                       p0[1] + (p1[1] - p0[1]) * (k / n)) for k in range(1, n)]
            edge_cache[key] = cached
        return cached if a < b else list(reversed(cached))

    # --- 逐胞腔铺网格 ---------------------------------------------------------
    verts: List[BakedVertex] = []
    tris: List[Tuple[int, int, int]] = []
    centroids: List[Pt] = []
    stats = {
        "cap_tris": 0,
        "skirt_tris": 0,
        "fallback_cells": 0,
        "degenerate_dir": 0,
        "boundary_pts": 0,
        "interior_pts": 0,
        "corner_pts": 0,
        "cell_areas": [],
        "boundary_seg_len": [],
        "cap_edge_len": [],
    }

    margin = INTERIOR_MARGIN_FRAC * TRI_EDGE_CM
    jitter = INTERIOR_JITTER_FRAC * TRI_EDGE_CM

    def emit_tri(a: BakedVertex, b: BakedVertex, c: BakedVertex) -> None:
        base = len(verts)
        if FLIP_WINDING:
            verts.extend((a, c, b))
        else:
            verts.extend((a, b, c))
        tris.append((base, base + 1, base + 2))

    for cell_id, corner_ids in enumerate(cell_corner_ids):
        if len(corner_ids) < 3:
            centroids.append(sites[cell_id])
            continue

        poly = [welder.points[i] for i in corner_ids]
        cx, cy, area = poly_area_centroid(poly)
        if area <= 0.0:
            poly.reverse()
            corner_ids = list(reversed(corner_ids))
            cx, cy, area = poly_area_centroid(poly)
        centroids.append((cx, cy))
        stats["cell_areas"].append(area)

        # 边界环：胞腔角点 + 共享边上的细分点。is_corner 只给角点。
        ring: List[Pt] = []
        ring_corner: List[bool] = []
        m = len(corner_ids)
        for k in range(m):
            a = corner_ids[k]
            b = corner_ids[(k + 1) % m]
            ring.append(welder.points[a])
            ring_corner.append(True)
            for p in edge_points(a, b):
                ring.append(p)
                ring_corner.append(False)
        nb = len(ring)
        stats["boundary_pts"] += nb
        stats["corner_pts"] += m
        for k in range(nb):
            x0, y0 = ring[k]
            x1, y1 = ring[(k + 1) % nb]
            stats["boundary_seg_len"].append(math.hypot(x1 - x0, y1 - y0))

        # 内部点
        cell_rng = stream_rng(seed, 0x1000 + cell_id)
        interior = hex_lattice_points(poly, (cx, cy), TRI_EDGE_CM, margin, jitter, cell_rng)
        stats["interior_pts"] += len(interior)

        # Delaunay（平移到质心附近后再算，改善行列式条件数）
        local = [(x - cx, y - cy) for x, y in ring] + [(x - cx, y - cy) for x, y in interior]
        cap = delaunay(local)

        # 校验：三角面积和 == 多边形面积；只用一次的边 == 边界环
        ok = len(cap) > 0
        if ok:
            tri_area = 0.0
            edge_uses: Dict[Tuple[int, int], int] = {}
            for a, b, c in cap:
                ax, ay = local[a]
                bx, by = local[b]
                ccx, ccy = local[c]
                tri_area += abs((bx - ax) * (ccy - ay) - (by - ay) * (ccx - ax)) * 0.5
                for e in ((a, b), (b, c), (c, a)):
                    key = (e[0], e[1]) if e[0] < e[1] else (e[1], e[0])
                    edge_uses[key] = edge_uses.get(key, 0) + 1
            if abs(tri_area - area) > max(1e-6 * area, 1e-6):
                ok = False
            else:
                once = {e for e, k in edge_uses.items() if k == 1}
                want = set()
                for k in range(nb):
                    a, b = k, (k + 1) % nb
                    want.add((a, b) if a < b else (b, a))
                if once != want:
                    ok = False

        if not ok:
            # 兜底：凸多边形的质心扇形剖分（覆盖一定正确，代价是丢内部细分）
            stats["fallback_cells"] += 1
            interior = [(cx, cy)]
            cap = [(k, (k + 1) % nb, nb) for k in range(nb)]

        # world 用**原始**坐标，不做 -centroid/+centroid 的往返 ——
        # 往返会在最后一位上改动共享边界点，相邻胞腔的角就不再逐位相同。
        world = list(ring) + list(interior)

        # --- 顶圈顶点（cap 与 skirt 共用坐标，但逐三角展开各存一份）-----------
        top_verts: List[BakedVertex] = []
        for idx, (x, y) in enumerate(world):
            dx, dy = dir_to_centroid(x, y, cx, cy)
            if dx == 0.0 and dy == 0.0:
                stats["degenerate_dir"] += 1
            is_corner = 1 if (idx < nb and ring_corner[idx]) else 0
            top_verts.append(BakedVertex(x, y, dx, dy, cell_id, 1, is_corner))

        # --- 顶盖三角（CCW → 翻成 kernel 口径的 CW）--------------------------
        for a, b, c in cap:
            emit_tri(top_verts[a], top_verts[c], top_verts[b])
            stats["cap_tris"] += 1
            for u, v in ((a, b), (b, c), (c, a)):
                stats["cap_edge_len"].append(math.dist(world[u], world[v]))

        # --- 底圈：顶圈边界环沿 DirToCentroid **横向**错开 LipOffset ----------
        #     没有 Z、没有下沉；裙边的竖直高度在运行时由两圈各自采高度产生。
        bottom_verts: List[BakedVertex] = []
        for k in range(nb):
            tv = top_verts[k]
            bx = tv.rest_x + tv.dir_x * LIP_OFFSET_CM
            by = tv.rest_y + tv.dir_y * LIP_OFFSET_CM
            bottom_verts.append(BakedVertex(bx, by, tv.dir_x, tv.dir_y, cell_id, 0, tv.is_corner))

        for k in range(nb):
            j = (k + 1) % nb
            emit_tri(top_verts[k], top_verts[j], bottom_verts[j])
            emit_tri(top_verts[k], bottom_verts[j], bottom_verts[k])
            stats["skirt_tris"] += 2

    t_mesh = time.time()
    stats["timing"] = {"sites": t_sites - t0, "mesh": t_mesh - t_sites}
    stats["junction_corners"] = junction_count
    stats["welded_points"] = len(welder.points)
    stats["voronoi_edges"] = len(edge_cache)
    stats["subdivided_edges"] = sum(1 for v in edge_cache.values() if v)
    return {
        "verts": verts,
        "tris": tris,
        "centroids": centroids,
        "cells": cell_corner_ids,
        "welded": welder.points,
        "sites": sites,
        "stats": stats,
    }


# =============================================================================
# 序列化
# =============================================================================

def pack_flag_word(v: BakedVertex) -> int:
    word = v.cell_id & CELL_ID_MASK
    if v.is_top_rim:
        word |= BIT_TOP_RIM
    if v.is_corner:
        word |= BIT_CORNER
    return word


def serialize(pattern: dict) -> bytes:
    verts: List[BakedVertex] = pattern["verts"]
    centroids: List[Pt] = pattern["centroids"]
    vcount = len(verts)
    tcount = vcount // 3
    ccount = len(centroids)

    blob_a = bytearray()
    pack_a = struct.Struct("<4f").pack
    for v in verts:
        blob_a += pack_a(v.rest_x, v.rest_y, v.dir_x, v.dir_y)

    blob_b = bytearray()
    pack_b = struct.Struct("<I").pack
    for v in verts:
        blob_b += pack_b(pack_flag_word(v))

    blob_c = bytearray()
    pack_c = struct.Struct("<2f").pack
    for cx, cy in centroids:
        blob_c += pack_c(cx, cy)

    payload = bytes(blob_a) + bytes(blob_b) + bytes(blob_c)
    crc = zlib.crc32(payload) & 0xFFFFFFFF

    min_x = min(v.rest_x for v in verts)
    max_x = max(v.rest_x for v in verts)
    min_y = min(v.rest_y for v in verts)
    max_y = max(v.rest_y for v in verts)

    flags = FLAG_TRIANGLE_SOUP
    if not FLIP_WINDING:
        flags |= FLAG_WINDING_CROSS_20_10

    header = struct.pack(
        "<4s3I3II4f4f",
        FORMAT_MAGIC, FORMAT_VERSION, HEADER_BYTES, flags,
        tcount, vcount, ccount, crc,
        WORLD_SPAN_CM, CELL_SIZE_CM, TRI_EDGE_CM, LIP_OFFSET_CM,
        min_x, min_y, max_x, max_y,
    )
    assert len(header) == HEADER_BYTES, len(header)
    return header + payload


# =============================================================================
# 预览图
# =============================================================================

def write_preview(pattern: dict, path: str) -> bool:
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

    welded = pattern["welded"]
    cells = pattern["cells"]
    centroids = pattern["centroids"]
    verts: List[BakedVertex] = pattern["verts"]
    half = 0.5 * WORLD_SPAN_CM

    fig, axes = plt.subplots(1, 3, figsize=(21.0, 7.4))
    fig.patch.set_facecolor("#12141a")

    # ---- ① 全场：逐胞腔哈希色 ------------------------------------------------
    ax = axes[0]
    polys = []
    colors = []
    for cid, ids in enumerate(cells):
        if len(ids) < 3:
            continue
        polys.append([welded[i] for i in ids])
        h = stream_rng(SEED, 0x7000 + cid)
        colors.append((0.30 + 0.45 * h.unit(), 0.28 + 0.40 * h.unit(), 0.26 + 0.36 * h.unit()))
    ax.add_collection(PolyCollection(polys, facecolors=colors, edgecolors="#0d0f14", linewidths=0.25))
    ax.set_xlim(-half, half)
    ax.set_ylim(-half, half)
    ax.set_aspect("equal")
    ax.set_title(f"① 全场 {WORLD_SPAN_CM/100:.0f} m × {WORLD_SPAN_CM/100:.0f} m · "
                 f"{len(polys)} 胞腔 · Lloyd × {LLOYD_ITERATIONS}", color="#e8e8e8", fontsize=11)

    # 逐胞腔重建「顶圈边界环 / 底圈环 / 顶盖三角」，供 ②③ 两图用。
    # 只有出现在**裙边三角**里的顶圈点才有底圈搭档；顶盖内部点没有。
    per_cell: Dict[int, dict] = {}
    for t in range(len(verts) // 3):
        a, b, c = verts[t * 3], verts[t * 3 + 1], verts[t * 3 + 2]
        rec = per_cell.setdefault(a.cell_id, {"cap": [], "rim": {}, "corner": set()})
        if a.is_top_rim and b.is_top_rim and c.is_top_rim:
            rec["cap"].append(((a.rest_x, a.rest_y), (b.rest_x, b.rest_y), (c.rest_x, c.rest_y)))
        else:
            for v in (a, b, c):
                if v.is_top_rim:
                    p = (v.rest_x, v.rest_y)
                    rec["rim"][p] = (v.rest_x + v.dir_x * LIP_OFFSET_CM,
                                     v.rest_y + v.dir_y * LIP_OFFSET_CM)
                    if v.is_corner:
                        rec["corner"].add(p)

    def rings(cid: int) -> Tuple[List[Pt], List[Pt]]:
        """按绕质心的极角排序还原顶圈 / 底圈闭合环（胞腔是凸的 ⇒ 极角序 == 边界序）。"""
        rec = per_cell.get(cid)
        if not rec or not rec["rim"]:
            return [], []
        cx, cy = centroids[cid]
        top = sorted(rec["rim"].keys(), key=lambda p: math.atan2(p[1] - cy, p[0] - cx))
        return top, [rec["rim"][p] for p in top]

    def loop(points: Sequence[Pt]) -> List[Tuple[Pt, Pt]]:
        n = len(points)
        return [(points[i], points[(i + 1) % n]) for i in range(n)] if n >= 3 else []

    # ---- ② 12 m 窗口：顶盖三角网 + 顶/底两圈 ---------------------------------
    ax = axes[1]
    win = 600.0
    cap_seg: List[Tuple[Pt, Pt]] = []
    top_loop: List[Tuple[Pt, Pt]] = []
    bot_loop: List[Tuple[Pt, Pt]] = []
    for cid, (ccx, ccy) in enumerate(centroids):
        if max(abs(ccx), abs(ccy)) > win + 400.0:
            continue
        rec = per_cell.get(cid)
        if not rec:
            continue
        for tri in rec["cap"]:
            cap_seg.extend([(tri[0], tri[1]), (tri[1], tri[2]), (tri[2], tri[0])])
        top, bot = rings(cid)
        top_loop.extend(loop(top))
        bot_loop.extend(loop(bot))
    ax.add_collection(LineCollection(cap_seg, colors="#3f5170", linewidths=0.45))
    ax.add_collection(LineCollection(bot_loop, colors="#d98b3a", linewidths=1.1))
    ax.add_collection(LineCollection(top_loop, colors="#e8e8e8", linewidths=1.1))
    ax.set_xlim(-win, win)
    ax.set_ylim(-win, win)
    ax.set_aspect("equal")
    ax.set_title("② 12 m 窗口 · 蓝=顶盖三角 白=顶圈边界 橙=底圈（向外 21.4 cm）",
                 color="#e8e8e8", fontsize=11)

    # ---- ③ 单胞腔：把 21.4 cm 的错开放大到看得见 -----------------------------
    ax = axes[2]
    pick = min(range(len(centroids)),
               key=lambda i: (centroids[i][0] ** 2 + centroids[i][1] ** 2) if per_cell.get(i) else 1e18)
    pcx, pcy = centroids[pick]
    rec = per_cell[pick]
    top, bot = rings(pick)
    ax.add_collection(LineCollection([(t[0], t[1]) for t in
                                      [(a, b) for tri in rec["cap"] for a, b in
                                       ((tri[0], tri[1]), (tri[1], tri[2]), (tri[2], tri[0]))]],
                                     colors="#3f5170", linewidths=0.7))
    # 裙边带：顶圈 → 底圈的四边形，填出来最直观
    band = [[top[i], top[(i + 1) % len(top)], bot[(i + 1) % len(bot)], bot[i]] for i in range(len(top))]
    ax.add_collection(PolyCollection(band, facecolors="#d98b3a", alpha=0.22,
                                     edgecolors="#d98b3a", linewidths=0.5))
    ax.add_collection(LineCollection(loop(bot), colors="#d98b3a", linewidths=1.6))
    ax.add_collection(LineCollection(loop(top), colors="#e8e8e8", linewidths=1.6))
    ax.scatter(*zip(*top), s=16, c="#e8e8e8", zorder=4, label="顶圈 bIsTopRim=1")
    ax.scatter(*zip(*bot), s=16, c="#d98b3a", zorder=4,
               label=f"底圈 bIsTopRim=0（沿 DirToCentroid 外移 {LIP_OFFSET_CM} cm）")
    corner_pts = [p for p in top if p in rec["corner"]]
    if corner_pts:
        ax.scatter(*zip(*corner_pts), s=64, facecolors="none", edgecolors="#5ad1a0",
                   linewidths=1.4, zorder=5, label="bIsCorner = 1（Voronoi 角点）")
    ax.plot([pcx], [pcy], marker="+", ms=9, c="#5ad1a0", zorder=5)
    ax.set_xlim(pcx - 300, pcx + 300)
    ax.set_ylim(pcy - 300, pcy + 340)
    ax.set_aspect("equal")
    ax.set_title(f"③ 单胞腔 #{pick} · 顶/底两圈的横向错开 = LipOffset {LIP_OFFSET_CM} cm（图案是平的，无 Z）",
                 color="#e8e8e8", fontsize=11)
    leg = ax.legend(loc="upper left", fontsize=8, framealpha=0.35, facecolor="#1b1e26",
                    edgecolor="#2a2f3a")
    for text in leg.get_texts():
        text.set_color("#e8e8e8")

    for ax in axes:
        ax.set_facecolor("#12141a")
        ax.tick_params(colors="#7a8290", labelsize=7)
        for spine in ax.spines.values():
            spine.set_color("#2a2f3a")

    fig.tight_layout()
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    fig.savefig(path, dpi=110, facecolor=fig.get_facecolor())
    plt.close(fig)
    return True


# =============================================================================
# 报告
# =============================================================================

def report(pattern: dict, blob: bytes, bin_path: str) -> None:
    s = pattern["stats"]
    verts = pattern["verts"]
    tris = len(verts) // 3
    cells = sum(1 for c in pattern["cells"] if len(c) >= 3)
    areas = s["cell_areas"]
    mean_area = sum(areas) / len(areas)
    var = sum((a - mean_area) ** 2 for a in areas) / len(areas)
    cv = math.sqrt(var) / mean_area
    seg = s["boundary_seg_len"]
    cap_e = s["cap_edge_len"]
    corner_verts = sum(1 for v in verts if v.is_corner)
    top_verts = sum(1 for v in verts if v.is_top_rim)

    print("=" * 78)
    print("披挂岩壳碎裂图案 —— 实测数字")
    print("=" * 78)
    print(f"  胞腔                 {cells}            （计划口径 ~1,820）")
    print(f"  三角                 {tris}            （顶盖 {s['cap_tris']} + 裙边 {s['skirt_tris']}）")
    print(f"  顶点                 {len(verts)}       （= 三角 × 3，逐三角展开）")
    print(f"  顶圈 / 底圈顶点      {top_verts} / {len(verts) - top_verts}")
    print(f"  bIsCorner = 1        {corner_verts} ({100.0*corner_verts/len(verts):.1f}% ，TG 实测 19.3%)")
    print(f"  三片交汇的焊接点     {s['junction_corners']} / {s['welded_points']}")
    print(f"  Voronoi 边           {s['voronoi_edges']}（其中 {s['subdivided_edges']} 条被细分，"
          f"{100.0*s['subdivided_edges']/s['voronoi_edges']:.0f}%）")
    print(f"  胞腔面积             均值 {mean_area/1e4:.2f} m²  等效边长 {math.sqrt(mean_area)/100:.2f} m  CV {cv*100:.1f}%"
          f"  [{min(areas)/1e4:.2f} .. {max(areas)/1e4:.2f} m²]")
    print(f"  边界段长             均值 {sum(seg)/len(seg)/100:.2f} m  中位 {sorted(seg)[len(seg)//2]/100:.2f} m")
    print(f"  顶盖三角边长         均值 {sum(cap_e)/len(cap_e)/100:.2f} m")
    print(f"  逐胞腔               顶盖 {s['cap_tris']/cells:.1f} 三角 · 裙边 {s['skirt_tris']/cells:.1f} 三角"
          f" · 边界点 {s['boundary_pts']/cells:.1f} · 内部点 {s['interior_pts']/cells:.1f}")
    print(f"  兜底扇形剖分的胞腔   {s['fallback_cells']}")
    print(f"  DirToCentroid 退化   {s['degenerate_dir']}")
    print(f"  文件                 {len(blob)} B = {len(blob)/1024/1024:.2f} MiB   {bin_path}")
    print(f"  CRC32(payload)       0x{struct.unpack_from('<I', blob, 0x1C)[0]:08X}")
    aux = len(verts) * 20
    std = len(verts) * (12 + 8)
    print(f"  常驻显存估算         aux {aux/1024/1024:.2f} MiB + 标准流(Pos12+Tan8) {std/1024/1024:.2f} MiB"
          f" = {(aux+std)/1024/1024:.2f} MiB   （计划口径 ~8 MiB）")
    print(f"  耗时                 站点/Lloyd {s['timing']['sites']:.1f}s  建网格 {s['timing']['mesh']:.1f}s")
    print("=" * 78)


def main(argv: Sequence[str]) -> int:
    # Windows 控制台默认 GBK，报告里的中文与 m² 会直接抛 UnicodeEncodeError。
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=os.path.join(root, DEFAULT_BIN_RELPATH))
    ap.add_argument("--preview-out", default=os.path.join(root, DEFAULT_PREVIEW_RELPATH))
    ap.add_argument("--no-preview", action="store_true")
    ap.add_argument("--seed", type=int, default=SEED)
    args = ap.parse_args(list(argv))

    pattern = build_pattern(args.seed)
    blob = serialize(pattern)

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "wb") as fh:
        fh.write(blob)

    report(pattern, blob, args.out)

    if not args.no_preview:
        if write_preview(pattern, args.preview_out):
            print(f"  预览图               {args.preview_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
