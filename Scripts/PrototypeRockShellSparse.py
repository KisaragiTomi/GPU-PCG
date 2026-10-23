# -*- coding: utf-8 -*-
"""稀疏岩壳（Docs/TinyGlade/RockShell_Sparse.md）的离线原型与证据脚本。

程序化胞腔 = 世界锚定抖动网格的 Voronoi 胞腔（可选低频坐标扭曲 / 功率权重），逐胞腔在
(2R+1)^2 邻域里做半平面裁剪 —— GPU 版照这里的求值顺序写。本脚本量四件事：

  1. 邻域半径 R 的精确性：无权重对 scipy 真 Voronoi，有权重对 R = 5；
  2. 胞腔面积离散度 / 细条数，对照 TG 原件 `rocky_terrain_shell.glb`；
  3. 固定拓扑模板（M 个边界点、内缩唇、一圈内环 + 中心扇）每胞腔的三角数；
  4. 演示土台（半径 600 / 裙边 800 / 台高 700，与 RockShell.DrapesOnSlopes 同一档）按
     「裙边环带」激活的胞腔数、活胞腔数、三角数与显存，对照现状整张 tile。

并出一张 TG 原件（×0.35）与程序化图案同窗口、同比例尺的对照图。

依赖 numpy / scipy / Pillow —— 与 VerifyRockShellGlb.py 不同，**不是纯标准库**。
用法：python PrototypeRockShellSparse.py [--glb PATH] [--no-render]
"""
import argparse
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw
from scipy.spatial import Voronoi

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from VerifyRockShellGlb import DEFAULT_GLB, Glb, Shell  # noqa: E402

PLUGIN_ROOT = os.path.dirname(HERE)
DEFAULT_COMPARE_RELPATH = os.path.join("Docs", "TinyGlade", "img", "RockShell_Sparse_PatternCompare.png")

TG_NATIVE_SPACING_CM = 13650.0 / math.sqrt(609.0)   # 原件 tile 跨度 / √胞腔数 = 553.1 cm
TG_LIP_CM = 19.5                                    # 原件底圈相对盖边界的外移（RockShell_Pattern.md §3）
BYTES_PER_VERTEX = 12 + 8 + 7 * 8 + 4 + 4           # Position + TangentBasis + 7 组 UV + Color + Index
BYTES_PER_TRI = 3 * BYTES_PER_VERTEX + 4            # + 逐三角材质 id
M32 = 0xFFFFFFFF


# ---------------------------------------------------------------------------
# 与 CSGroundShaperField.ush 逐位相同的哈希 / value noise（CPU 孪生已在 CSGroundShaperField.h）
# ---------------------------------------------------------------------------
def hash_u32(cx, cy, seed):
    h = (cx * 0x9E3779B1) & M32
    h ^= (cy * 0x85EBCA77) & M32
    h ^= (seed * 0xC2B2AE3D) & M32
    h ^= h >> 15
    h = (h * 0x2545F491) & M32
    h ^= h >> 13
    return h


def hash01(cx, cy, seed):
    return (hash_u32(cx & M32, cy & M32, seed & M32) & 0xFFFFFF) / 16777216.0


def value_noise(x, y, seed):
    fx, fy = math.floor(x), math.floor(y)
    tx, ty = x - fx, y - fy
    ux, uy = tx * tx * (3 - 2 * tx), ty * ty * (3 - 2 * ty)
    ix, iy = int(fx), int(fy)
    h00, h10 = hash01(ix, iy, seed), hash01(ix + 1, iy, seed)
    h01, h11 = hash01(ix, iy + 1, seed), hash01(ix + 1, iy + 1, seed)
    a = h00 + (h10 - h00) * ux
    b = h01 + (h11 - h01) * ux
    return (a + (b - a) * uy) * 2 - 1


# ---------------------------------------------------------------------------
# 程序化胞腔
# ---------------------------------------------------------------------------
class CellPattern:
    """S = 种子格距 cm；J = 抖动 0..1；A / L = 扭曲幅度 / 波长（单位 = 格）；V = 功率权重半径 / S。"""

    def __init__(self, S=0.35 * TG_NATIVE_SPACING_CM, J=1.0, A=0.5, L=3.0, V=0.0, seed=1):
        self.S, self.J, self.A, self.L, self.V, self.seed = S, J, A, L, V, seed

    def seed_local(self, i, j):
        """种子相对格 (i,j) 左下角的偏移（单位 = 格）。扭曲场在未扭曲的种子处求值。"""
        u, v = hash01(i, j, self.seed * 3 + 1), hash01(i, j, self.seed * 3 + 2)
        x, y = 0.5 + self.J * (u - 0.5), 0.5 + self.J * (v - 0.5)
        if self.A > 0.0:
            gx, gy = (i + x) / self.L, (j + y) / self.L
            x += value_noise(gx, gy, self.seed * 7 + 101) * self.A
            y += value_noise(gx + 17.31, gy - 9.77, self.seed * 7 + 103) * self.A
        return x, y

    def weight(self, i, j):
        r = self.V * self.S * hash01(i, j, self.seed * 3 + 3)
        return r * r

    def polygon(self, i, j, R=3):
        """格 (i,j) 那粒种子的胞腔（世界 cm，逆时针）。局部坐标求值：d = q − p 由整数格差 +
        局部偏移差算出，世界坐标再大也不丢精度。起始正方形 ±3S，(2R+1)^2 − 1 次裁剪。"""
        S = self.S
        px, py = self.seed_local(i, j)
        wp = self.weight(i, j)
        poly = [(-3 * S, -3 * S), (3 * S, -3 * S), (3 * S, 3 * S), (-3 * S, 3 * S)]
        for dj in range(-R, R + 1):
            for di in range(-R, R + 1):
                if di == 0 and dj == 0:
                    continue
                qx, qy = self.seed_local(i + di, j + dj)
                dx, dy = (di + qx - px) * S, (dj + qy - py) * S
                # 功率图的半平面：2 x·d <= |d|^2 + w_p − w_q（x 是相对种子的局部坐标）
                poly = clip(poly, 2 * dx, 2 * dy, dx * dx + dy * dy + wp - self.weight(i + di, j + dj))
                if not poly:
                    return []
        return [((i + px) * S + x, (j + py) * S + y) for x, y in poly]

    def seed_world(self, i, j):
        x, y = self.seed_local(i, j)
        return (i + x) * self.S, (j + y) * self.S


def clip(poly, nx, ny, c):
    """Sutherland–Hodgman，保留 nx·x + ny·y <= c 的一侧。"""
    out = []
    n = len(poly)
    for k in range(n):
        ax, ay = poly[k]
        bx, by = poly[(k + 1) % n]
        da, db = nx * ax + ny * ay - c, nx * bx + ny * by - c
        if da <= 0:
            out.append((ax, ay))
        if (da <= 0) != (db <= 0):
            t = da / (da - db)
            out.append((ax + t * (bx - ax), ay + t * (by - ay)))
    return out


def area(poly):
    return 0.5 * sum(poly[k][0] * poly[(k + 1) % len(poly)][1] - poly[(k + 1) % len(poly)][0] * poly[k][1]
                     for k in range(len(poly)))


def centroid(poly):
    a = area(poly)
    cx = cy = 0.0
    for k in range(len(poly)):
        x0, y0 = poly[k]
        x1, y1 = poly[(k + 1) % len(poly)]
        f = x0 * y1 - x1 * y0
        cx += (x0 + x1) * f
        cy += (y0 + y1) * f
    return cx / (6 * a), cy / (6 * a)


def dedupe(poly, eps=1e-3):
    """裁剪恰好过角点时会留下重合顶点，合掉。"""
    out = []
    for p in poly:
        if not out or math.hypot(p[0] - out[-1][0], p[1] - out[-1][1]) > eps:
            out.append(p)
    if len(out) > 1 and math.hypot(out[0][0] - out[-1][0], out[0][1] - out[-1][1]) <= eps:
        out.pop()
    return out


def inradius(poly):
    """质心到各边的最小距离（细条判据；不是严格内切圆，但对凸多边形是它的下界）。"""
    cx, cy = centroid(poly)
    best = 1e30
    for k in range(len(poly)):
        ax, ay = poly[k]
        bx, by = poly[(k + 1) % len(poly)]
        ex, ey = bx - ax, by - ay
        best = min(best, abs((cx - ax) * ey - (cy - ay) * ex) / max(math.hypot(ex, ey), 1e-9))
    return best


# ---------------------------------------------------------------------------
# 1. 精确性
# ---------------------------------------------------------------------------
def check_exactness(pat, radii=(1, 2, 3), n=36, margin=8):
    """返回 {R: (与真值不一致的胞腔数, 总数)}。V = 0 的真值是 scipy Voronoi，V > 0 的真值取 R = 5。"""
    keys = [(i, j) for j in range(margin, n - margin) for i in range(margin, n - margin)]
    truth = {}
    if pat.V == 0.0:
        pts = np.array([pat.seed_world(i, j) for j in range(n) for i in range(n)])
        vor = Voronoi(pts)
        for i, j in keys:
            reg = vor.regions[vor.point_region[j * n + i]]
            truth[(i, j)] = abs(area([tuple(vor.vertices[q]) for q in reg]))
    else:
        for i, j in keys:
            p = pat.polygon(i, j, 5)
            truth[(i, j)] = abs(area(p)) if p else 0.0
    out = {}
    for R in radii:
        bad = 0
        for (i, j), a in truth.items():
            p = pat.polygon(i, j, R)
            bad += abs(a - (abs(area(p)) if p else 0.0)) > 1e-4 * pat.S * pat.S
        out[R] = (bad, len(truth))
    return out


# ---------------------------------------------------------------------------
# 2. 统计
# ---------------------------------------------------------------------------
def check_exactness_wide(pat, radii=(2, 3), ref=5, n=50):
    """大样本：n×n 个胞腔（哈希是无限的，没有边界效应）逐个对 R = ref。返回 {R: 不一致数}。"""
    out = {R: 0 for R in radii}
    for j in range(n):
        for i in range(n):
            q = pat.polygon(i, j, ref)
            a = abs(area(q)) if q else 0.0
            for R in radii:
                p = pat.polygon(i, j, R)
                out[R] += abs(a - (abs(area(p)) if p else 0.0)) > 1e-4 * pat.S * pat.S
    return out, n * n


def pattern_stats(pat, n=50, lip=6.8):
    areas, corners, thin, min_in = [], [], 0, 1e30
    for j in range(n):
        for i in range(n):
            p = pat.polygon(i, j, 3)
            p = dedupe(p) if p else []
            if len(p) < 3:
                areas.append(0.0)
                thin += 1
                min_in = 0.0
                continue
            areas.append(abs(area(p)))
            corners.append(len(p))
            r = inradius(p)
            min_in = min(min_in, r)
            thin += r < 3.0 * lip
    a = np.array(areas)
    live = a[a > 0]
    return {"cv": live.std() / live.mean(), "min": live.min() / live.mean(), "max": live.max() / live.mean(),
            "corners_max": max(corners), "thin": thin, "cells": n * n, "min_inradius": min_in}


def tg_stats(sh):
    """TG 原件逐胞腔（盖 + 裙）XZ 投影面积；贴 tile 边的胞腔被 tile 截过，排除。"""
    half = 68.25 - 0.05
    cell_area, touches = {}, set()
    for t in range(sh.tris):
        a, b, c = sh.pos[3 * t], sh.pos[3 * t + 1], sh.pos[3 * t + 2]
        cid = sh.cell_id[3 * t]
        cell_area[cid] = cell_area.get(cid, 0.0) + abs((b[0] - a[0]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[0] - a[0])) * 0.5
        if any(abs(q[0]) > half or abs(q[2]) > half for q in (a, b, c)):
            touches.add(cid)
    a = np.array([v for k, v in cell_area.items() if k not in touches])
    return {"cells": len(a), "cv": a.std() / a.mean(), "min": a.min() / a.mean(), "max": a.max() / a.mean(),
            "tris_per_cell": sh.tris / len(cell_area)}


# ---------------------------------------------------------------------------
# 3. 固定拓扑模板
# ---------------------------------------------------------------------------
def sample_boundary(poly, M):
    """角点全部保留，其余 M − N 个按边长最大余数法分到各边。返回 [(x, y, 是角点, 所在边)]。"""
    N = len(poly)
    L = [math.hypot(poly[(k + 1) % N][0] - poly[k][0], poly[(k + 1) % N][1] - poly[k][1]) for k in range(N)]
    K = M - N
    raw = [K * l / sum(L) for l in L]
    cnt = [int(math.floor(r)) for r in raw]
    for k in sorted(range(N), key=lambda e: raw[e] - cnt[e], reverse=True)[:K - sum(cnt)]:
        cnt[k] += 1
    out = []
    for k in range(N):
        ax, ay = poly[k]
        bx, by = poly[(k + 1) % N]
        out.append((ax, ay, True, k))
        for s in range(1, cnt[k] + 1):
            t = s / (cnt[k] + 1)
            out.append((ax + t * (bx - ax), ay + t * (by - ay), False, k))
    return out


def inset_corners(poly, lip):
    """逐边内移 lip 后相邻两条边线求交 = 内缩后的角点（与原角点一一对应）。"""
    N = len(poly)
    lines = []
    for k in range(N):
        ax, ay = poly[k]
        bx, by = poly[(k + 1) % N]
        ex, ey = bx - ax, by - ay
        L = math.hypot(ex, ey)
        lines.append((ax - ey / L * lip, ay + ex / L * lip, ex, ey))
    out = []
    for k in range(N):
        px, py, ux, uy = lines[k - 1]
        qx, qy, vx, vy = lines[k]
        den = ux * vy - uy * vx
        t = ((qx - px) * vy - (qy - py) * vx) / den if abs(den) > 1e-9 else 0.0
        out.append((px + ux * t, py + uy * t) if abs(den) > 1e-9 else (qx, qy))
    return out


def template(poly, M=16, lip=6.8, ring=0.5):
    """一个胞腔的三角汤 [(三个顶点, 是盖三角)]，恒 5M 个：中心扇 M + 内环带 2M（盖）+ 裙 2M。
    底圈 B = Voronoi 边上的采样点（相邻胞腔共边不共点，同 TG 原件）；顶圈 T = 内缩 lip 的盖边界。"""
    lip = min(lip, 0.4 * inradius(poly))               # 小胞腔：唇宽夹到内切半径的 0.4 倍
    B = sample_boundary(poly, M)
    Tc = inset_corners(poly, lip)
    T = []
    for x, y, is_corner, e in B:
        if is_corner:
            T.append(Tc[e])
        else:
            ax, ay = poly[e]
            bx, by = poly[(e + 1) % len(poly)]
            L = math.hypot(bx - ax, by - ay)
            T.append((x - (by - ay) / L * lip, y + (bx - ax) / L * lip))
    C = centroid(poly)
    Rg = [(C[0] + ring * (t[0] - C[0]), C[1] + ring * (t[1] - C[1])) for t in T]
    tris = []
    for k in range(M):
        n = (k + 1) % M
        bk, bn = (B[k][0], B[k][1]), (B[n][0], B[n][1])
        tris += [((C, Rg[k], Rg[n]), True), ((Rg[k], T[k], T[n]), True), ((Rg[k], T[n], Rg[n]), True),
                 ((T[k], bk, bn), False), ((T[k], bn, T[n]), False)]
    return tris


# ---------------------------------------------------------------------------
# 4. 演示土台的激活账
# ---------------------------------------------------------------------------
def demo_activation(pat, M, r0=600.0, F=800.0, H=700.0, slope_lo=0.75, margin=60.0):
    """激活 = 胞腔与裙边环带 [r0, r0+F] 相交：SDF 是 1-Lipschitz 的，所以拿种子处的有符号距离 ±
    外接半径判（外接半径由 CPU 侧同一份裁剪算出；margin 盖住坡度差分跨距 50 cm 与扩张 / 倾斜位移）。
    活 = 模板任一顶点处的解析坡度 > SlopeLo。不计裙边噪声，所以活胞腔数是**下界**。"""
    S = pat.S
    reach = r0 + F + 4 * S
    lo, hi = int(math.floor(-reach / S)) - 1, int(math.ceil(reach / S)) + 1
    active = live = 0
    for j in range(lo, hi):
        for i in range(lo, hi):
            poly = pat.polygon(i, j, 3)
            poly = dedupe(poly) if poly else []
            if len(poly) < 3:
                continue
            sx, sy = pat.seed_world(i, j)
            rc = max(math.hypot(x - sx, y - sy) for x, y in poly)
            sd = math.hypot(sx, sy) - r0
            if sd - rc - margin > F or sd + rc + margin < 0.0:
                continue
            active += 1
            for tri, _ in template(poly, M):
                if any(0.0 < (math.hypot(x, y) - r0) / F < 1.0 and
                       6.0 * ((math.hypot(x, y) - r0) / F) * (1.0 - (math.hypot(x, y) - r0) / F) * H / F > slope_lo
                       for x, y in tri):
                    live += 1
                    break
    return active, live


def current_tile_bytes(tris=49598, cells=609):
    verts = tris * 3
    aux = verts * (16 + 4 + 8) + verts * 4 + tris * 3 * 4 + cells * 8   # RestDir/CellFlags/IncidentRange + IncidentTris + Neighbours + Centroids
    return tris * BYTES_PER_TRI + aux


# ---------------------------------------------------------------------------
# 5. 对照图
# ---------------------------------------------------------------------------
def color_of(key):
    h = hash_u32(key & M32, 0x51ED, 7)
    return 90 + (h & 0x7F), 90 + ((h >> 8) & 0x7F), 90 + ((h >> 16) & 0x7F)


def draw_tris(img, tris, x0, y0, W, px):
    d = ImageDraw.Draw(img)
    s = px / W
    for pts, col, is_cap in tris:
        k = 1.0 if is_cap else 0.45
        d.polygon([((x - x0) * s, px - (y - y0) * s) for x, y in pts], fill=tuple(int(v * k) for v in col))


def render_compare(sh, pat, M, lip, path, W=2400.0, px=900):
    scale = 100.0 * 0.35                               # 原件 POSITION 是米，×100 → cm，×0.35 = 现状口径
    tg = []
    for t in range(sh.tris):
        a, b, c = sh.pos[3 * t], sh.pos[3 * t + 1], sh.pos[3 * t + 2]
        pts = [(q[0] * scale, q[2] * scale) for q in (a, b, c)]
        if all(abs(x) < W and abs(y) < W for x, y in pts):
            tg.append((pts, color_of(sh.cell_id[3 * t]), sh.tri_is_top[3 * t] > 0.5))
    left = Image.new("RGB", (px, px), (245, 245, 240))
    draw_tris(left, tg, -W / 2, -W / 2, W, px)

    pr = []
    n = int(W / pat.S) + 4
    for j in range(-n, n):
        for i in range(-n, n):
            poly = pat.polygon(i, j, 3)
            poly = dedupe(poly) if poly else []
            if len(poly) < 3 or max(max(abs(x), abs(y)) for x, y in poly) > W:
                continue
            col = color_of((i * 7919) ^ (j * 104729))
            pr += [(tri, col, is_cap) for tri, is_cap in template(poly, M, lip)]
    right = Image.new("RGB", (px, px), (245, 245, 240))
    draw_tris(right, pr, -W / 2, -W / 2, W, px)

    out = Image.new("RGB", (2 * px + 24, px + 34), (255, 255, 255))
    out.paste(left, (0, 34))
    out.paste(right, (px + 24, 34))
    d = ImageDraw.Draw(out)
    d.text((8, 10), "LEFT: TG rocky_terrain_shell x0.35 (current, one tile = 47.8 m)   24 m x 24 m", fill=(0, 0, 0))
    d.text((px + 32, 10), f"RIGHT: procedural  S={pat.S:.0f}cm  jitter {pat.J}  warp A={pat.A} L={pat.L}  "
                          f"power V={pat.V}  R=3  M={M}   same window", fill=(0, 0, 0))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    out.save(path)


def main(argv):
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--glb", default=DEFAULT_GLB)
    ap.add_argument("--no-render", action="store_true")
    args = ap.parse_args(argv)

    S = 0.35 * TG_NATIVE_SPACING_CM
    lip = TG_LIP_CM * 0.35
    print(f"种子格距 S = {S:.1f} cm（= 0.35 × 原件 {TG_NATIVE_SPACING_CM:.1f} cm），唇宽 {lip:.2f} cm")

    print("\n== 1. 邻域半径 R 的精确性（不一致胞腔 / 总数）")
    for J, A, L, V in ((1.0, 0.0, 3.0, 0.0), (1.0, 0.5, 3.0, 0.0), (1.0, 0.8, 5.0, 0.0),
                       (1.0, 0.0, 3.0, 0.7), (1.0, 0.5, 3.0, 0.6), (1.0, 0.8, 5.0, 0.7)):
        print(f"  J={J} A={A} L={L} V={V}: {check_exactness(CellPattern(S, J, A, L, V))}")
    print("  大样本（50×50 胞腔，对 R = 5）：")
    for J, A, L, V in ((1.0, 0.5, 3.0, 0.0), (1.0, 0.0, 3.0, 0.7), (1.0, 0.5, 3.0, 0.6), (1.0, 0.8, 5.0, 0.7)):
        bad, tot = check_exactness_wide(CellPattern(S, J, A, L, V))
        print(f"    J={J} A={A} L={L} V={V}: R=2 不一致 {bad[2]}/{tot}，R=3 不一致 {bad[3]}/{tot}")

    print("\n== 2. 面积离散度（面积 / 均值；细条 = 空胞腔或内切半径 < 3 × 唇宽）")
    sh = Shell(Glb(args.glb))
    t = tg_stats(sh)
    print(f"  TG 原件: {t['cells']} 个不贴 tile 边的胞腔  CV {t['cv']:.3f}  [{t['min']:.2f}, {t['max']:.2f}]  "
          f"三角 / 胞腔 {t['tris_per_cell']:.1f}")
    for J, A, L, V in ((1.0, 0.0, 3.0, 0.0), (1.0, 0.5, 3.0, 0.0), (1.0, 0.8, 5.0, 0.0),
                       (1.0, 0.0, 3.0, 0.7), (1.0, 0.5, 3.0, 0.4), (1.0, 0.5, 3.0, 0.6)):
        s = pattern_stats(CellPattern(S, J, A, L, V), lip=lip)
        print(f"  J={J} A={A} L={L} V={V}: CV {s['cv']:.3f}  [{s['min']:.2f}, {s['max']:.2f}]  "
              f"最多角点 {s['corners_max']}  细条 {s['thin']}/{s['cells']}  最小内切半径 {s['min_inradius']:.1f} cm")

    print("\n== 3/4. 模板与演示土台激活（J=1 A=0.5 L=3 V=0）")
    pat = CellPattern(S, 1.0, 0.5, 3.0, 0.0)
    cur = current_tile_bytes()
    print(f"  现状整张 tile: 49598 三角 / 609 胞腔，常驻 {cur / 2**20:.2f} MiB（含 aux 拓扑流），盖 47.8 m 见方")
    for M in (12, 16):
        act, live = demo_activation(pat, M)
        tris = act * 5 * M
        slot = 2 * M * 8 + 32
        mem = tris * BYTES_PER_TRI + act * slot
        print(f"  M={M}: 三角 / 胞腔 {5 * M}；激活 {act} 胞腔（活 ≥ {live}）；三角 {tris}；"
              f"显存 {mem / 2**20:.2f} MiB（{BYTES_PER_TRI} B / 三角 + {slot} B / 胞腔几何）")

    if not args.no_render:
        path = os.path.join(PLUGIN_ROOT, DEFAULT_COMPARE_RELPATH)
        render_compare(sh, pat, 16, lip, path)
        print(f"\n对照图 → {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
