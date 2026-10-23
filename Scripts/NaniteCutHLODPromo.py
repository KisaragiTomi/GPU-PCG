# -*- coding: utf-8 -*-
"""Nanite 截面 HLOD 宣传短片：纯代码逐帧绘制（发光线框 + 分簇着色 + 中英双语字幕），配乐后编码成 mp4。

    python NaniteCutHLODPromoMusic.py         # 先出配乐 Saved/NaniteCutHLODVideo/promo_music.wav
    python NaniteCutHLODPromo.py              # 全片 → Saved/NaniteCutHLODVideo/NaniteCutHLOD_Promo.mp4
    python NaniteCutHLODPromo.py --preview    # 抽 20 帧拼成预览图

系统 Python（numpy + PIL + scipy），不进 UE。分镜：

   0–23 s  岩块群从线条里长出来，镜头拉远缩成 4 个像素
  23–33 s  Nanite 把网格切成一簇簇三角形（分簇着色，Nanite 的 Clusters 视图）
  33–48 s  一组簇简化时锁住边界、只动内部 → 精度不同的簇依然严丝合缝
  48–58 s  80 m 的距离波扫过，岩块换成粗一级的簇、轮廓不变；合成一份网格 378,362 → 49,516
  58–70 s  1024 个视点凝视，看不见的消失 → 40,919
  70–80 s  重分 UV、烘 2048 图集
  80–90 s  距离轴上 80 m 切换
  90–94 s  片名

画面里的数字取自测试关卡的实测结果。
"""
import heapq
import math
import os
import shutil
import subprocess
import sys
from collections import defaultdict
from multiprocessing import Pool

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont
from scipy.spatial import Delaunay

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
OUT_DIR = os.path.join(ROOT, "Saved", "NaniteCutHLODVideo")
MP4 = os.path.join(OUT_DIR, "NaniteCutHLOD_Promo.mp4")
MUSIC = os.path.join(OUT_DIR, "promo_music.wav")

# 合成版（--combined）：宣传片正文 → 「实机演示」章节卡 → 编辑器录屏 → 片名。
# 这几个时间点与 NaniteCutHLODPromoMusic.py --combined 里的同名常量必须一致。
REC_SRC = r"D:\MyProject\ocam\oCam\录制_2026_09_28_16_52_09_19.mp4"
REC_DIR = os.path.join(OUT_DIR, "rec_frames")
REC_W, REC_H = 2536, 1368
DEMO_START, DEMO_CARD = 89.2, 3.0          # 宣传片在片名之前收住
REC_START = DEMO_START + DEMO_CARD
REC_DUR = 73.86
COMBINED_TITLE = REC_START + REC_DUR
COMBINED_DUR = COMBINED_TITLE + 4.8
COMBINED_MP4 = os.path.join(OUT_DIR, "NaniteCutHLOD_Promo_Demo.mp4")
COMBINED_MUSIC = os.path.join(OUT_DIR, "promo_demo_music.wav")
# 录屏里的取景（录屏像素坐标）：整屏 / 视口 / 材质编辑器窗口
REC_FULL = (0, 0, REC_W, REC_H)
REC_VIEWPORT = (520, 108, 1737, 888)
REC_MATERIAL = (590, 266, 2230, 1296)
# (录屏时间, 取景)：相邻关键帧之间缓动过渡
REC_SHOTS = [(0.0, REC_FULL), (2.5, REC_FULL), (4.5, REC_VIEWPORT), (65.4, REC_VIEWPORT), (66.8, REC_MATERIAL),
             (71.6, REC_MATERIAL), (72.8, REC_VIEWPORT), (72.9, REC_VIEWPORT), (73.86, REC_FULL)]

W, H, SS, FPS = 1920, 1080, 2, 30
DURATION = 94.0
N_FRAMES = int(DURATION * FPS)
SHIFT = 14.0     # 可见性 / 烘焙 / 距离三段相对第一版整体后移（中间插进了分簇与锁边两段）

LINE = np.array([214, 238, 255], np.float32)
CYAN = np.array([120, 200, 255], np.float32)
GOLD = np.array([255, 206, 122], np.float32)
AMBER = np.array([255, 160, 44], np.float32)       # 锁住的边界：比 GOLD 饱和，泛光之后仍读得出金色
DIM = np.array([70, 110, 160], np.float32)
GROUP_TINT = np.array([66, 108, 176], np.float32)  # 选中成组后的底色：压暗，让金边跳出来
RESPLIT_COLS = [np.array([88, 168, 255], np.float32), np.array([170, 128, 255], np.float32)]
# 分簇配色：冷暖交错、饱和度压低，放在深蓝底上不刺眼
PALETTE = np.array([[92, 170, 255], [255, 170, 110], [130, 225, 190], [200, 140, 255], [255, 215, 120],
                    [110, 205, 235], [240, 130, 170], [160, 200, 120], [150, 160, 255], [255, 150, 140],
                    [100, 230, 230], [220, 190, 255]], np.float32)

F_ZH = r"C:\Windows\Fonts\simfang.ttf"
F_EN = r"C:\Windows\Fonts\georgiai.ttf"
F_NUM = r"C:\Windows\Fonts\cambria.ttc"     # Georgia 没有 → 字形
F_TITLE = r"C:\Windows\Fonts\simsun.ttc"

CAPTIONS = [
    (4.0, 8.2, "每一座山，每一块石头，", "Every mountain, every stone,"),
    (8.6, 13.0, "都由千万个三角形织成。", "is woven from countless triangles."),
    (14.0, 17.6, "可在远处，", "But from afar,"),
    (18.0, 22.4, "它们只剩下几个像素。", "they are only a few pixels."),
    (23.6, 27.6, "Nanite 把每个网格，", "Nanite splits every mesh"),
    (28.0, 32.4, "切成一簇簇三角形。", "into clusters of triangles."),
    (33.4, 37.6, "简化一组簇时，锁住它的边界，", "Simplify a group of clusters — lock its border,"),
    (38.0, 42.2, "只让内部变粗。", "and let only the inside grow coarse."),
    (42.8, 47.6, "所以精度不同的簇，依然严丝合缝。", "So clusters of different detail still meet without a seam."),
    (48.6, 52.6, "我们在八十米处，", "At eighty meters,"),
    (53.0, 57.6, "横切一刀。", "we make a single cut."),
    (58.6, 63.2, "从一千个方向凝视，", "Gaze from a thousand directions,"),
    (63.6, 69.4, "看不见的，就让它消失。", "and let the unseen fade away."),
    (70.4, 74.4, "把光与颜色，", "Light and colour,"),
    (74.8, 79.6, "烘进一张图里。", "baked into a single image."),
    (80.6, 84.6, "远处的世界，", "The distant world"),
    (85.0, 89.2, "不必每一粒沙。", "needs not every grain of sand."),
]


# -----------------------------------------------------------------------------
# 数学小件
# -----------------------------------------------------------------------------
def clamp01(x):
    return max(0.0, min(1.0, x))


def seg(t, a, b):
    return clamp01((t - a) / (b - a)) if b > a else float(t >= a)


def smooth(x):
    x = clamp01(x)
    return x * x * (3.0 - 2.0 * x)


def ease_out(x):
    x = clamp01(x)
    return 1.0 - (1.0 - x) ** 3


def ease_in_out(x):
    x = clamp01(x)
    return 4 * x * x * x if x < 0.5 else 1 - (-2 * x + 2) ** 3 / 2


def window(t, a, b, fade=0.8):
    return smooth(seg(t, a, a + fade)) * (1.0 - smooth(seg(t, b - fade, b)))


def rot(yaw, pitch):
    cy, sy, cp, sp = math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch)
    ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]], np.float32)
    rx = np.array([[1, 0, 0], [0, cp, -sp], [0, sp, cp]], np.float32)
    return rx @ ry


# -----------------------------------------------------------------------------
# 几何：岩块（削切的二十面体细分）、分簇
# -----------------------------------------------------------------------------
def icosphere(level):
    t = (1.0 + 5 ** 0.5) / 2.0
    v = [(-1, t, 0), (1, t, 0), (-1, -t, 0), (1, -t, 0), (0, -1, t), (0, 1, t), (0, -1, -t), (0, 1, -t),
         (t, 0, -1), (t, 0, 1), (-t, 0, -1), (-t, 0, 1)]
    f = [(0, 11, 5), (0, 5, 1), (0, 1, 7), (0, 7, 10), (0, 10, 11), (1, 5, 9), (5, 11, 4), (11, 10, 2), (10, 7, 6),
         (7, 1, 8), (3, 9, 4), (3, 4, 2), (3, 2, 6), (3, 6, 8), (3, 8, 9), (4, 9, 5), (2, 4, 11), (6, 2, 10),
         (8, 6, 7), (9, 8, 1)]
    verts = [np.array(p, np.float64) / np.linalg.norm(p) for p in v]
    faces = list(f)
    for _ in range(level):
        cache, nf = {}, []

        def mid(a, b):
            key = (min(a, b), max(a, b))
            if key not in cache:
                m = verts[a] + verts[b]
                verts.append(m / np.linalg.norm(m))
                cache[key] = len(verts) - 1
            return cache[key]

        for a, b, c in faces:
            ab, bc, ca = mid(a, b), mid(b, c), mid(c, a)
            nf += [(a, ab, ca), (b, bc, ab), (c, ca, bc), (ab, bc, ca)]
        faces = nf
    return np.array(verts, np.float32), np.array(faces, np.int32)


def rock_field(level, seed):
    """同一个 seed 作用在不同细分级别上 —— 细的是源，粗的就是它的截面。一组随机平面"削切"出崩面。"""
    v, f = icosphere(level)
    v = v.astype(np.float64)
    rng = np.random.default_rng(seed)
    for _ in range(11):
        n = rng.normal(size=3)
        n /= np.linalg.norm(n)
        o = rng.uniform(0.55, 0.88)
        d = v @ n - o
        v -= np.maximum(d, 0.0)[:, None] * n[None, :]
    dirs = rng.normal(size=(6, 3))
    dirs /= np.linalg.norm(dirs, axis=1, keepdims=True)
    v *= (1.0 + 0.05 * np.sin(v @ dirs.T * 7.0).sum(1))[:, None]
    scale = rng.uniform([0.9, 0.6, 0.85], [1.35, 0.95, 1.25])
    v = (v * scale).astype(np.float32)
    # 统一成外法线的绕序（背面剔除要用）
    n = np.cross(v[f[:, 1]] - v[f[:, 0]], v[f[:, 2]] - v[f[:, 0]])
    bad = (n * v[f].mean(1)).sum(1) < 0
    f = f.copy()
    f[bad] = f[bad][:, [0, 2, 1]]
    return v, f


def edges_of(faces):
    e = np.concatenate([faces[:, [0, 1]], faces[:, [1, 2]], faces[:, [2, 0]]])
    e.sort(axis=1)
    return np.unique(e, axis=0)


def edge_faces(faces):
    """每条边 → 两侧的面（边界边另一侧是 -1）。"""
    m = defaultdict(list)
    for fi, (a, b, c) in enumerate(faces):
        for e in ((a, b), (b, c), (c, a)):
            m[(min(e), max(e))].append(fi)
    keys = sorted(m)
    ef = np.array([(m[k] + [-1])[:2] for k in keys], np.int64)
    return np.array(keys, np.int64), ef


def cluster_faces(V, F, target, iters=4):
    """按面邻接做多源 Dijkstra 生长 + 几轮重心回迁：得到连通、紧凑的簇（Nanite 每簇 128 三角，这里按网格规模缩）。"""
    n = len(F)
    k = max(1, int(round(n / target)))
    C = V[F].mean(1)
    seeds = [0]
    d = np.linalg.norm(C - C[0], axis=1)
    for _ in range(k - 1):
        s = int(d.argmax())
        seeds.append(s)
        d = np.minimum(d, np.linalg.norm(C - C[s], axis=1))
    _, ef = edge_faces(F)
    adj = [[] for _ in range(n)]
    for f0, f1 in ef:
        if f1 >= 0:
            adj[f0].append(f1)
            adj[f1].append(f0)
    label = np.zeros(n, np.int64)
    for _ in range(iters):
        label = -np.ones(n, np.int64)
        heap = [(0.0, s, ci) for ci, s in enumerate(seeds)]
        heapq.heapify(heap)
        while heap:
            dd, f, ci = heapq.heappop(heap)
            if label[f] != -1:
                continue
            label[f] = ci
            for g in adj[f]:
                if label[g] == -1:
                    heapq.heappush(heap, (dd + float(np.linalg.norm(C[f] - C[g])), g, ci))
        new = []
        for ci in range(k):
            m = np.where(label == ci)[0]
            if len(m) == 0:
                new.append(seeds[ci])
                continue
            cc = C[m].mean(0)
            new.append(int(m[np.argmin(np.linalg.norm(C[m] - cc, axis=1))]))
        seeds = new
    return label


def color_clusters(label, ef, seed):
    """相邻的簇不同色：簇邻接图上贪心着色（按度数从大到小，起始色随 seed 错开）。"""
    k = int(label.max()) + 1
    nbr = [set() for _ in range(k)]
    for f0, f1 in ef:
        if f1 >= 0 and label[f0] != label[f1]:
            nbr[label[f0]].add(label[f1])
            nbr[label[f1]].add(label[f0])
    color = -np.ones(k, np.int64)
    for c in sorted(range(k), key=lambda c: -len(nbr[c])):
        used = {color[x] for x in nbr[c] if color[x] >= 0}
        for j in range(len(PALETTE)):
            cand = (j + seed * 5 + c) % len(PALETTE)
            if cand not in used:
                color[c] = cand
                break
    return color


class ClusteredMesh:
    def __init__(self, v, f, target, seed):
        self.v, self.f = v, f
        self.label = cluster_faces(v, f, target)
        self.edges, self.ef = edge_faces(f)
        self.color = PALETTE[color_clusters(self.label, self.ef, seed)[self.label]]


RNG = np.random.default_rng(7)
ROCKS = []
for i, (off, s) in enumerate([((-3.4, -0.25, 0.9), 0.62), ((-1.3, 0.05, -1.1), 0.9), ((1.0, -0.2, 0.7), 0.7),
                              ((3.3, -0.05, -0.6), 0.8), ((-2.2, -0.55, 3.0), 0.45), ((2.2, -0.5, 3.1), 0.5),
                              ((0.1, 0.25, -3.4), 0.66)]):
    fv, ff = rock_field(3, 100 + i)
    mv, mf = rock_field(2, 100 + i)
    cv_, cf = rock_field(1, 100 + i)
    fe, ce = edges_of(ff), edges_of(cf)
    ROCKS.append({"off": np.array(off, np.float32), "s": s, "fv": fv, "ff": ff, "fe": fe, "cv": cv_, "cf": cf,
                  "ce": ce, "fth": RNG.random(len(fe)).astype(np.float32),
                  "fine": ClusteredMesh(fv, ff, 64, i), "mid": ClusteredMesh(mv, mf, 32, i + 3)})
HERO = ClusteredMesh(*rock_field(3, 200), 56, 1)      # 分簇特写那一块
HERO_E = edges_of(HERO.f)

HOUSE_V = np.array([[x, y, z] for x in (-1, 1) for y in (-0.7, 0.7) for z in (-0.8, 0.8)], np.float32)
HOUSE_E = [(a, b) for a in range(8) for b in range(a + 1, 8) if bin(a ^ b).count("1") == 1]
ROOF_V = np.array([[-1.15, 0.7, -0.9], [1.15, 0.7, -0.9], [1.15, 0.7, 0.9], [-1.15, 0.7, 0.9], [-1.15, 1.35, 0], [1.15, 1.35, 0]], np.float32)
ROOF_E = [(0, 4), (3, 4), (1, 5), (2, 5), (4, 5), (0, 1), (2, 3)]
INNER = [rock_field(2, 300 + i) for i in range(3)]
INNER_E = [edges_of(f) for _, f in INNER]
INNER_OFF = [np.array(o, np.float32) for o in ((-0.5, -0.35, 0.1), (0.35, -0.4, -0.2), (0.1, -0.4, 0.45))]


def fib_sphere(n):
    i = np.arange(n) + 0.5
    phi = np.arccos(1 - 2 * i / n)
    th = math.pi * (1 + 5 ** 0.5) * i
    return np.stack([np.cos(th) * np.sin(phi), np.cos(phi), np.sin(th) * np.sin(phi)], 1).astype(np.float32)


VIEWS = fib_sphere(160)


# -----------------------------------------------------------------------------
# 锁边简化演示：平面上一片三角网、分簇、选 4 簇成组、锁住组边界只坍缩内部顶点
# -----------------------------------------------------------------------------
def make_patch():
    rng = np.random.default_rng(21)
    nx, ny = 30, 17
    pts = []
    for j in range(ny):
        for i in range(nx):
            x, y = i / (nx - 1), j / (ny - 1)
            if 0 < i < nx - 1:
                x += (0.5 / (nx - 1) if j % 2 else 0.0) + rng.uniform(-0.28, 0.28) / (nx - 1)
            if 0 < j < ny - 1:
                y += rng.uniform(-0.28, 0.28) / (ny - 1)
            pts.append((min(max(x, 0.0), 1.0), y))
    p = np.array(pts)
    tri = Delaunay(p).simplices.astype(np.int64)
    # 统一逆时针
    a, b, c = p[tri[:, 0]], p[tri[:, 1]], p[tri[:, 2]]
    cross = (b[:, 0] - a[:, 0]) * (c[:, 1] - a[:, 1]) - (b[:, 1] - a[:, 1]) * (c[:, 0] - a[:, 0])
    tri[cross < 0] = tri[cross < 0][:, [0, 2, 1]]
    return p, tri


def tri_area(p, t):
    a, b, c = p[t[0]], p[t[1]], p[t[2]]
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])


def simplify_group(p, tris, group_mask, rounds=2, seed=5):
    """锁住组边界，按独立集坍缩内部顶点（每个删掉的顶点并进一个相邻的保留顶点，不许翻面）。
    返回每一轮的 (坍缩前的三角表, 坍缩对)。"""
    rng = np.random.default_rng(seed)
    tris = tris.copy()
    in_group = group_mask.copy()
    history = []
    for _ in range(rounds):
        g_idx = np.where(in_group)[0]
        # 组边界：组内三角的边，另一侧不在组内（或在网格边上）
        emap = defaultdict(list)
        for ti in range(len(tris)):
            a, b, c = tris[ti]
            for e in ((a, b), (b, c), (c, a)):
                emap[(min(e), max(e))].append(ti)
        border = set()
        for (u, v), ts in emap.items():
            flags = [in_group[x] for x in ts]
            if any(flags) and (len(ts) == 1 or not all(flags)):
                border.update((u, v))
        nbr = defaultdict(set)
        vt = defaultdict(list)
        for ti in g_idx:
            a, b, c = tris[ti]
            for x, y in ((a, b), (b, c), (c, a)):
                nbr[x].add(y)
                nbr[y].add(x)
            for x in (a, b, c):
                vt[x].append(ti)
        interior = [v for v in nbr if v not in border]
        rng.shuffle(interior)
        removed, blocked, pairs = set(), set(), []
        for v in interior:
            if v in blocked:
                continue
            best = None
            for u in sorted(nbr[v], key=lambda u: np.linalg.norm(p[u] - p[v])):
                if u in removed:
                    continue
                ok = True
                for ti in vt[v]:
                    t_ = tris[ti]
                    if u in t_:
                        continue
                    nt = np.where(t_ == v, u, t_)
                    if tri_area(p, nt) <= 1e-6 or np.sign(tri_area(p, nt)) != np.sign(tri_area(p, t_)):
                        ok = False
                        break
                if ok:
                    best = u
                    break
            if best is None:
                continue
            pairs.append((v, best))
            removed.add(v)
            blocked.update(nbr[v])
            blocked.add(best)
        history.append((tris.copy(), in_group.copy(), pairs))
        # 应用
        remap = {v: u for v, u in pairs}
        nt, ng = [], []
        for ti, t_ in enumerate(tris):
            t2 = np.array([remap.get(x, x) for x in t_])
            if len(set(t2.tolist())) < 3:
                continue
            nt.append(t2)
            ng.append(in_group[ti])
        tris, in_group = np.array(nt), np.array(ng)
    history.append((tris.copy(), in_group.copy(), []))
    return history


PATCH_P, PATCH_T = make_patch()
PATCH_LABEL = cluster_faces(np.column_stack([PATCH_P, np.zeros(len(PATCH_P))]).astype(np.float32), PATCH_T, 38)


def pick_group(label, tris, p, k=4):
    cents = {c: p[tris[label == c]].mean((0, 1)) for c in np.unique(label)}
    center = min(cents, key=lambda c: np.linalg.norm(cents[c] - np.array([0.5, 0.5])))
    group = sorted(cents, key=lambda c: np.linalg.norm(cents[c] - cents[center]))[:k]
    return set(group)


GROUP = pick_group(PATCH_LABEL, PATCH_T, PATCH_P)
GROUP_MASK = np.array([l in GROUP for l in PATCH_LABEL])
SIMPL = simplify_group(PATCH_P, PATCH_T, GROUP_MASK, rounds=2)
# 简化后组内重新分成 2 簇
_final_t, _final_g, _ = SIMPL[-1]
_gt = _final_t[_final_g]
_sub = cluster_faces(np.column_stack([PATCH_P, np.zeros(len(PATCH_P))]).astype(np.float32), _gt, max(4, len(_gt) // 2), iters=3)
RESPLIT = np.zeros(len(_final_t), np.int64) - 1
RESPLIT[np.where(_final_g)[0]] = _sub
PATCH_RECT = (250.0, 150.0, 1420.0, 640.0)      # x0, y0, w, h
ORIG_LABEL = {tuple(sorted(t_.tolist())): PATCH_LABEL[ti] for ti, t_ in enumerate(PATCH_T)}


def group_border_verts(tris, g):
    emap = defaultdict(list)
    for ti, t_ in enumerate(tris):
        for e in ((t_[0], t_[1]), (t_[1], t_[2]), (t_[2], t_[0])):
            emap[(min(e), max(e))].append(ti)
    out = set()
    for (u, v), ts in emap.items():
        flags = [g[x] for x in ts]
        if any(flags) and (len(ts) == 1 or not all(flags)):
            out.update((int(u), int(v)))
    return out


LOCK_VERTS = sorted(group_border_verts(*SIMPL[-1][:2]))
_pef = edge_faces(PATCH_T)[1]
PATCH_COLOR = color_clusters(PATCH_LABEL, _pef, 2)


# -----------------------------------------------------------------------------
# 背景：深蓝渐变 + 星云 + 远处的三角网 + 景深光斑 + 胶片颗粒（都预先算好，逐帧只做平移与混合）
# -----------------------------------------------------------------------------
def fbm(h, w, seed, octaves=5, base=5):
    rng = np.random.default_rng(seed)
    acc = np.zeros((h, w), np.float32)
    amp, tot = 1.0, 0.0
    for o in range(octaves):
        gh = base * 2 ** o
        gw = int(gh * w / h) + 2
        g = (rng.random((gh + 2, gw)) * 255).astype(np.uint8)
        acc += np.asarray(Image.fromarray(g).resize((w, h), Image.BICUBIC), np.float32) / 255.0 * amp
        tot += amp
        amp *= 0.55
    return acc / tot


def make_background():
    y = np.linspace(0, 1, H, dtype=np.float32)[:, None]
    x = np.linspace(-1, 1, W, dtype=np.float32)[None, :]
    top, bottom = np.array([4, 7, 18], np.float32), np.array([10, 22, 44], np.float32)
    g = top + (bottom - top) * y[..., None]
    r = np.sqrt((x * 0.9) ** 2 + ((y - 0.5) * 1.6) ** 2)
    glow = np.clip(1.0 - r, 0, 1)[..., None] ** 2 * np.array([16, 30, 54], np.float32)
    vign = np.clip(1.15 - 0.55 * r, 0.35, 1.0)[..., None]
    return (g + glow) * vign, vign


BG, VIGN = make_background()
PAD_X, PAD_Y = 520, 300
NEB_D = np.clip((fbm(H + PAD_Y, W + PAD_X, 31) - 0.42) * 2.4, 0, 1) ** 1.7
NEB_M = fbm(H + PAD_Y, W + PAD_X, 32, octaves=3, base=3)
NEB_A = np.array([30, 62, 120], np.float32)
NEB_B = np.array([78, 44, 126], np.float32)


def make_lattice():
    """远处的一张三角网：交错网格的三向连线，像一张铺满天空的、极淡的网格。"""
    hh, ww = H + PAD_Y, W + PAD_X
    img = Image.new("L", (ww * 2, hh * 2))
    dots = Image.new("L", (ww * 2, hh * 2))
    d, dd = ImageDraw.Draw(img), ImageDraw.Draw(dots)
    rng = np.random.default_rng(41)
    sp = 118.0
    rows, cols = int(hh / (sp * 0.866)) + 3, int(ww / sp) + 3
    grid = {}
    for j in range(rows):
        for i in range(cols):
            x = (i + (0.5 if j % 2 else 0.0)) * sp + rng.uniform(-18, 18)
            yv = j * sp * 0.866 + rng.uniform(-14, 14)
            grid[(i, j)] = (x * 2, yv * 2)
    for (i, j), p in grid.items():
        nbrs = [(i + 1, j)]
        nbrs += [(i + (j % 2), j + 1), (i - 1 + (j % 2), j + 1)]
        for q in nbrs:
            if q in grid:
                d.line([p, grid[q]], fill=90, width=2)
        r = 3 + rng.uniform(0, 3)
        dd.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r], fill=int(120 + rng.uniform(0, 135)))
    lat = np.asarray(img.resize((ww, hh), Image.LANCZOS).filter(ImageFilter.GaussianBlur(0.6)), np.float32) / 255.0
    dts = np.asarray(dots.resize((ww, hh), Image.LANCZOS).filter(ImageFilter.GaussianBlur(1.0)), np.float32) / 255.0
    return lat, dts


LATTICE, LATTICE_DOTS = make_lattice()


def make_bokeh():
    sprites = []
    for rad in (7, 12, 19, 28):
        s = rad * 2 + 8
        yy, xx = np.mgrid[0:s, 0:s].astype(np.float32) - s / 2
        r = np.sqrt(xx * xx + yy * yy) / rad
        disc = np.clip(1.0 - r, 0, 1) ** 0.6 * 0.55 + np.exp(-((r - 0.92) ** 2) / 0.004) * 0.45
        disc = np.asarray(Image.fromarray((np.clip(disc, 0, 1) * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(1.2)), np.float32) / 255.0
        sprites.append(disc)
    rng = np.random.default_rng(51)
    parts = []
    for _ in range(46):
        k = int(rng.integers(0, 4))
        parts.append((rng.uniform(0, W), rng.uniform(0, H), k, rng.uniform(0.3, 1.0), rng.uniform(0, 6.28),
                      GOLD if rng.random() < 0.18 else (CYAN if rng.random() < 0.5 else LINE)))
    return sprites, parts


BOKEH_SPRITES, BOKEH = make_bokeh()
GRAIN = np.random.default_rng(61).normal(0.0, 1.0, (H + 64, W + 64)).astype(np.float32)
STARS = np.column_stack([RNG.random(220) * W, RNG.random(220) * H, RNG.random(220), RNG.random(220) * 6.28]).astype(np.float32)


def mood(t):
    """背景色调随段落走：蓝 → 切的时候偏金 → 烘焙偏紫 → 收尾回到深蓝。"""
    gold = window(t, 48.0, 58.0, 2.0) * 0.35
    violet = window(t, 70.0, 80.0, 2.0) * 0.4
    return gold, violet


def background(t, frame):
    out = BG.copy()
    # 全片 94 s 内偏移不超过留边，不用取模（取模会在绕回时跳一下）
    xo = min(int(t * 5.0), PAD_X - 1)
    yo = min(int(t * 2.2), PAD_Y - 1)
    d = NEB_D[yo:yo + H, xo:xo + W]
    m = NEB_M[yo:yo + H, xo:xo + W]
    gold, violet = mood(t)
    ca = NEB_A * (1 - gold) + np.array([90, 70, 40], np.float32) * gold
    cb = NEB_B * (1 - violet) + np.array([110, 60, 150], np.float32) * violet
    neb = d[..., None] * (ca * (1 - m[..., None]) + cb * m[..., None])
    out += neb * 0.55 * VIGN
    # 三角网：跟星云走（星云浓的地方网才显出来），视差比星云慢
    lx, ly = min(int(t * 3.0), PAD_X - 1), min(int(t * 1.0), PAD_Y - 1)
    lat = LATTICE[ly:ly + H, lx:lx + W]
    tw = 0.5 + 0.5 * math.sin(t * 0.7)
    dots = LATTICE_DOTS[ly:ly + H, lx:lx + W]
    mask = 0.12 + 0.88 * d
    out += (lat * 26.0 + dots * 34.0 * (0.6 + 0.4 * tw))[..., None] * mask[..., None] * np.array([0.55, 0.8, 1.0], np.float32)
    # 景深光斑
    for x0, y0, k, depth, ph, col in BOKEH:
        spr = BOKEH_SPRITES[k]
        s = spr.shape[0]
        x = int((x0 + t * 5.0 * depth) % (W + s)) - s
        y = int((y0 - t * 3.0 * depth) % (H + s)) - s
        a = (0.10 + 0.16 * depth) * (0.65 + 0.35 * math.sin(t * 0.8 + ph))
        xa, ya, xb, yb = max(x, 0), max(y, 0), min(x + s, W), min(y + s, H)
        if xb <= xa or yb <= ya:
            continue
        out[ya:yb, xa:xb] += spr[ya - y:yb - y, xa - x:xb - x, None] * col * a
    # 颗粒
    gx, gy = (frame * 37) % 64, (frame * 53) % 64
    out += GRAIN[gy:gy + H, gx:gx + W, None] * 2.4
    return out


# -----------------------------------------------------------------------------
# 画布：填充层（不泛光）+ 线条层（泛光）+ 文字层
# -----------------------------------------------------------------------------
class Canvas:
    def __init__(self):
        self.img = Image.new("RGB", (W * SS, H * SS))
        self.base = Image.new("RGB", (W * SS, H * SS))
        self.d = ImageDraw.Draw(self.img)
        self.bd = ImageDraw.Draw(self.base)
        self.text = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        self.td = ImageDraw.Draw(self.text)
        self.used_base = False
        self.photo = None       # (RGB 数组, x, y, 不透明度)：录屏画面，压在背景之上、线条之下

    def seg(self, p, q, color, alpha=1.0, width=1.0):
        if alpha <= 0.01:
            return
        c = tuple(int(v) for v in np.clip(color * alpha, 0, 255))
        self.d.line([(p[0] * SS, p[1] * SS), (q[0] * SS, q[1] * SS)], fill=c, width=max(1, int(round(width * SS))))

    def dot(self, p, r, color, alpha=1.0):
        if alpha <= 0.01:
            return
        c = tuple(int(v) for v in np.clip(color * alpha, 0, 255))
        x, y, rr = p[0] * SS, p[1] * SS, r * SS
        self.d.ellipse([x - rr, y - rr, x + rr, y + rr], fill=c)

    def poly(self, pts, color, alpha=1.0, outline=None, oa=1.0, base=True):
        c = tuple(int(v) for v in np.clip(color * alpha, 0, 255))
        sp = [(x * SS, y * SS) for x, y in pts]
        if base:
            self.bd.polygon(sp, fill=c)
            self.used_base = True
            if outline is not None and oa > 0.01:
                o = tuple(int(v) for v in np.clip(outline * oa, 0, 255))
                self.d.line(sp + [sp[0]], fill=o, width=SS)
        else:
            o = tuple(int(v) for v in np.clip(outline * oa, 0, 255)) if outline is not None else None
            self.d.polygon(sp, fill=c, outline=o)

    def finish(self, t, frame):
        out = background(t, frame)
        if self.photo is not None:
            img, px, py, pa = self.photo
            ph, pw = img.shape[:2]
            # 投影：矩形外一圈柔和压暗
            yy = np.arange(H, dtype=np.float32)[:, None]
            xx = np.arange(W, dtype=np.float32)[None, :]
            dx = np.maximum(np.maximum(px - xx, xx - (px + pw)), 0)
            dy = np.maximum(np.maximum(py - yy, yy - (py + ph)), 0)
            shade = np.clip(1.0 - np.sqrt(dx * dx + dy * dy) / 60.0, 0, 1) ** 2
            out *= (1.0 - 0.6 * pa * shade)[..., None]
            region = out[py:py + ph, px:px + pw]
            out[py:py + ph, px:px + pw] = region * (1 - pa) + img.astype(np.float32) * pa
        if self.used_base:
            out += np.asarray(self.base.resize((W, H), Image.LANCZOS), np.float32)
        lines = np.asarray(self.img.resize((W, H), Image.LANCZOS), np.float32)
        small = self.img.resize((W // 2, H // 2), Image.BILINEAR)
        b1 = np.asarray(small.filter(ImageFilter.GaussianBlur(5)).resize((W, H), Image.BILINEAR), np.float32)
        b2 = np.asarray(small.filter(ImageFilter.GaussianBlur(18)).resize((W, H), Image.BILINEAR), np.float32)
        out = out + lines + b1 * 0.85 + b2 * 0.6
        tx = np.asarray(self.text, np.float32)
        a = tx[..., 3:4] / 255.0
        out = out * (1 - a) + tx[..., :3] * a
        return np.clip(out, 0, 255).astype(np.uint8)


def project(v, yaw, pitch, dist, fov_px, cx=W / 2, cy=H / 2, shift=(0, 0, 0)):
    p = (v + np.array(shift, np.float32)) @ rot(yaw, pitch).T
    z = np.maximum(p[:, 2] + dist, 0.1)
    return np.stack([cx + fov_px * p[:, 0] / z, cy - fov_px * p[:, 1] / z], 1), z


def draw_mesh(cv, pts, depth, edges, color, alpha, width=1.0, thresholds=None, reveal=1.0):
    dmin, dmax = float(depth.min()), float(depth.max())
    span = max(dmax - dmin, 1e-3)
    for k, (a, b) in enumerate(edges):
        ea = alpha
        if thresholds is not None:
            ea *= clamp01((reveal - thresholds[k]) * 6.0)
            if ea <= 0.01:
                continue
        dd = ((depth[a] + depth[b]) * 0.5 - dmin) / span
        cv.seg(pts[a], pts[b], color, ea * (1.0 - 0.6 * dd), width)


LIGHT = np.array([-0.45, 0.62, -0.64], np.float32)
LIGHT /= np.linalg.norm(LIGHT)


def draw_clustered(cv, cm, scale, yaw, pitch, dist, fov, alpha, cx=W / 2, cy=H / 2, shift=(0, 0, 0),
                   fill=0.5, inner=0.28, border=1.0, border_col=None, tint=0.0, reveal=1.0):
    """分簇着色的网格：背面剔除 + 画家算法填色（兰伯特明暗），簇内边暗、簇边界亮。"""
    if alpha <= 0.01:
        return
    P = (cm.v * scale + np.array(shift, np.float32)) @ rot(yaw, pitch).T
    z = np.maximum(P[:, 2] + dist, 0.1)
    pts = np.stack([cx + fov * P[:, 0] / z, cy - fov * P[:, 1] / z], 1)
    F = cm.f
    a, b, c = P[F[:, 0]], P[F[:, 1]], P[F[:, 2]]
    n = np.cross(b - a, c - a)
    nn = n / (np.linalg.norm(n, axis=1, keepdims=True) + 1e-9)
    cen = (a + b + c) / 3.0
    vis = (n * (np.array([0, 0, -dist], np.float32) - cen)).sum(1) > 0
    shade = 0.28 + 0.72 * np.clip(nn @ LIGHT, 0, 1)
    order = np.argsort(-(cen[:, 2]))
    base_col = cm.color * (1 - tint) + GOLD * tint
    # 按簇"点亮"：reveal 从 0 到 1 时簇一个个亮起来
    lit = np.clip((reveal * (cm.label.max() + 1.6) - cm.label) , 0, 1)
    for fi in order:
        if not vis[fi]:
            continue
        k = lit[fi] * fill
        if k <= 0.01:
            continue
        cv.poly([tuple(pts[i]) for i in F[fi]], base_col[fi] * shade[fi], alpha * k)
    f0, f1 = cm.ef[:, 0], cm.ef[:, 1]
    v0 = vis[f0]
    v1 = np.where(f1 >= 0, vis[np.maximum(f1, 0)], False)
    show = v0 | v1
    sil = v0 ^ v1
    diff = np.where(f1 >= 0, cm.label[f0] != cm.label[np.maximum(f1, 0)], True)
    bcol = LINE if border_col is None else border_col
    for k in np.where(show)[0]:
        e0, e1 = cm.edges[k]
        if diff[k] or sil[k]:
            cv.seg(pts[e0], pts[e1], bcol, alpha * border, 1.35)
        else:
            fc = cm.color[f0[k]] if v0[k] else cm.color[f1[k]]
            cv.seg(pts[e0], pts[e1], fc * 0.6 + LINE * 0.4, alpha * inner, 0.8)


def draw_floor(cv, yaw, pitch, dist, fov, alpha, cy=H / 2, y=-1.15, extent=9, step=1.0):
    if alpha <= 0.01:
        return
    xs = np.arange(-extent, extent + 0.01, step)
    for x in xs:
        seg3 = np.array([[x, y, -extent], [x, y, extent]], np.float32)
        _draw_floor_line(cv, seg3, yaw, pitch, dist, fov, alpha, cy)
    for zz in xs:
        seg3 = np.array([[-extent, y, zz], [extent, y, zz]], np.float32)
        _draw_floor_line(cv, seg3, yaw, pitch, dist, fov, alpha, cy)


def _draw_floor_line(cv, seg3, yaw, pitch, dist, fov, alpha, cy):
    k = 12
    pts3 = seg3[0] + (seg3[1] - seg3[0]) * np.linspace(0, 1, k)[:, None]
    P = pts3 @ rot(yaw, pitch).T
    z = P[:, 2] + dist
    for i in range(k - 1):
        if z[i] < 0.5 or z[i + 1] < 0.5:
            continue
        p0 = (W / 2 + fov * P[i, 0] / z[i], cy - fov * P[i, 1] / z[i])
        p1 = (W / 2 + fov * P[i + 1, 0] / z[i + 1], cy - fov * P[i + 1, 1] / z[i + 1])
        fade = clamp01(1.4 - (z[i] + z[i + 1]) * 0.5 / (dist * 1.6))
        r3 = np.linalg.norm(pts3[i, [0, 2]]) / 9.0
        cv.seg(p0, p1, DIM, alpha * fade * (1 - r3) * 0.55, 0.8)


# -----------------------------------------------------------------------------
# 文字
# -----------------------------------------------------------------------------
_FONTS = {}


def font(path, size):
    key = (path, size)
    if key not in _FONTS:
        _FONTS[key] = ImageFont.truetype(path, size)
    return _FONTS[key]


def _stroke(alpha):
    """文字外一圈暗描边：压在复杂画面上也读得清。"""
    return {"stroke_width": 3, "stroke_fill": (3, 6, 14, int(150 * alpha))}


def text_center(cv, s, y, path, size, color, alpha, spacing=0):
    if alpha <= 0.01:
        return
    f = font(path, size)
    w = cv.td.textlength(s, font=f) + spacing * max(0, len(s) - 1)
    x = (W - w) / 2
    c = tuple(int(v) for v in color) + (int(255 * alpha),)
    if spacing:
        for ch in s:
            cv.td.text((x, y), ch, font=f, fill=c, **_stroke(alpha))
            x += cv.td.textlength(ch, font=f) + spacing
    else:
        cv.td.text((x, y), s, font=f, fill=c, **_stroke(alpha))


def draw_captions(cv, t):
    for a, b, zh, en in CAPTIONS:
        al = window(t, a, b, 0.7)
        if al > 0.01:
            text_center(cv, zh, H - 168, F_ZH, 46, (236, 242, 250), al, spacing=5)
            text_center(cv, en, H - 104, F_EN, 25, (170, 190, 215), al * 0.9)


def number(cv, value, x, y, size, alpha, color=(255, 214, 140)):
    if alpha <= 0.01:
        return
    s = format(int(round(value)), ",")
    f = font(F_NUM, size)
    w = cv.td.textlength(s, font=f)
    cv.td.text((x - w / 2, y), s, font=f, fill=tuple(color) + (int(255 * alpha),), **_stroke(alpha))


def label(cv, s, x, y, size, alpha, color=(160, 190, 225), path=F_ZH):
    if alpha <= 0.01:
        return
    f = font(path, size)
    w = cv.td.textlength(s, font=f)
    cv.td.text((x - w / 2, y), s, font=f, fill=tuple(color) + (int(255 * alpha),), **_stroke(alpha))


# -----------------------------------------------------------------------------
# 分镜
# -----------------------------------------------------------------------------
def stars(cv, t, alpha):
    for x, y, b, ph in STARS:
        a = alpha * (0.25 + 0.5 * b) * (0.6 + 0.4 * math.sin(t * 1.3 + ph))
        cv.dot((x, (y + t * 4.0 * b) % H), 0.9 + 0.8 * b, LINE, a * 0.5)


def scene_world(cv, t):
    """0–23 s：岩块群从线条里长出来，缓慢环绕；然后镜头拉远缩成几个像素。"""
    a = window(t, 1.0, 23.4, 1.2)
    if a <= 0.01:
        return
    reveal = ease_out(seg(t, 1.5, 9.0)) * 1.05
    pull = ease_in_out(seg(t, 14.5, 21.0))
    dist = 9.0 + 70.0 * pull
    yaw = 0.35 + t * 0.06
    fov = 1150.0
    draw_floor(cv, yaw, -0.28, dist, fov, a * smooth(seg(t, 2.0, 6.0)) * (1 - pull))
    for r in ROCKS:
        pts, z = project(r["fv"] * r["s"], yaw, -0.28, dist, fov, shift=r["off"])
        w = max(0.5, 1.1 - pull * 0.7)
        draw_mesh(cv, pts, z, r["fe"], LINE, a * (1.0 - 0.55 * pull), w, r["fth"], reveal)
    grid_a = a * smooth(seg(t, 18.0, 20.5))
    if grid_a > 0.01:
        cell = 44
        gx0, gy0 = W / 2 - cell * 6, H / 2 - cell * 4
        for i in range(13):
            cv.seg((gx0 + i * cell, gy0), (gx0 + i * cell, gy0 + cell * 8), DIM, grid_a * 0.5, 0.7)
        for j in range(9):
            cv.seg((gx0, gy0 + j * cell), (gx0 + cell * 12, gy0 + j * cell), DIM, grid_a * 0.5, 0.7)
        for (ci, cj) in [(5, 3), (6, 3), (6, 4), (5, 4)]:
            x, y = gx0 + ci * cell, gy0 + cj * cell
            cv.poly([(x + 3, y + 3), (x + cell - 3, y + 3), (x + cell - 3, y + cell - 3), (x + 3, y + cell - 3)],
                    CYAN, grid_a * 0.55, base=False)
        label(cv, "378,362 个三角  →  4 个像素", W / 2, gy0 - 70, 34, grid_a, (200, 218, 240))
    stars(cv, t, a * 0.6)


def scene_clusters(cv, t):
    """23–33 s：一块岩石特写，表面一簇簇亮起来（Nanite 的 Clusters 视图）。"""
    a = window(t, 22.8, 33.6, 1.0)
    if a <= 0.01:
        return
    yaw = 0.2 + (t - 22.8) * 0.16
    wire = 1.0 - smooth(seg(t, 24.0, 26.5))
    pts, z = project(HERO.v * 1.55, yaw, -0.3, 6.2, 1000.0, cy=H / 2 - 60)
    draw_mesh(cv, pts, z, HERO_E, LINE, a * wire * 0.8, 0.9)
    draw_clustered(cv, HERO, 1.55, yaw, -0.3, 6.2, 1000.0, a, cy=H / 2 - 60,
                   reveal=ease_in_out(seg(t, 24.0, 28.5)), fill=0.55)
    n_cl = int(HERO.label.max() + 1)
    la = a * smooth(seg(t, 28.2, 29.4))
    label(cv, "%d 个簇 · 每簇约 %d 个三角" % (n_cl, len(HERO.f) // n_cl), W / 2, 70, 30, la, (200, 218, 240))
    label(cv, "（引擎里每簇 128 个）", W / 2, 116, 22, la * 0.8, (150, 175, 210))


def patch_xy(idx, positions=None, zoom=1.0, focus=None):
    p = PATCH_P if positions is None else positions
    x0, y0, w, h = PATCH_RECT
    xy = np.stack([x0 + p[:, 0] * w, y0 + p[:, 1] * h], 1)
    if focus is not None and zoom != 1.0:
        xy = focus + (xy - focus) * zoom + (np.array([W / 2, H / 2 - 60]) - focus) * min(1.0, zoom - 1.0)
    return xy if idx is None else xy[idx]


def scene_lock(cv, t):
    """33–48 s：平面上的一片簇；选 4 簇成组，边界描金锁住；内部顶点两轮坍缩；组内重新分 2 簇；推近看接缝。"""
    a = window(t, 32.8, 48.4, 1.0)
    if a <= 0.01:
        return
    group_on = smooth(seg(t, 33.8, 35.0))
    lock_on = smooth(seg(t, 35.2, 36.4))
    r1 = seg(t, 38.0, 40.0)
    r2 = seg(t, 40.1, 41.9)
    resplit = smooth(seg(t, 42.0, 43.0))
    zoom = 1.0 + 1.0 * ease_in_out(seg(t, 43.4, 46.6))
    # 选接缝焦点：组边界上离组中心最远的一个锁定顶点附近
    tris0, g0, pairs0 = SIMPL[0]
    tris1, g1, pairs1 = SIMPL[1]
    tris2, g2, _ = SIMPL[2]
    pos = PATCH_P.copy()
    stage_tris, stage_g = tris0, g0
    if r1 > 0:
        for k, (v, u) in enumerate(pairs0):
            d = k / max(len(pairs0), 1) * 0.5
            pos[v] = PATCH_P[v] + (PATCH_P[u] - PATCH_P[v]) * ease_in_out(seg(r1, d, d + 0.5))
    if r1 >= 1.0:
        stage_tris, stage_g = tris1, g1
        pos = PATCH_P.copy()
        for k, (v, u) in enumerate(pairs1):
            d = k / max(len(pairs1), 1) * 0.5
            pos[v] = PATCH_P[v] + (PATCH_P[u] - PATCH_P[v]) * ease_in_out(seg(r2, d, d + 0.5))
    if r2 >= 1.0:
        stage_tris, stage_g = tris2, g2
        pos = PATCH_P.copy()
    x0, y0, w, h = PATCH_RECT
    focus = np.array([x0 + w * 0.5, y0 + h * 0.5])
    if len(LOCK_VERTS):
        fv = min(LOCK_VERTS, key=lambda v: np.linalg.norm(PATCH_P[v] - np.array([0.5, 0.52])))
        focus = np.array([x0 + PATCH_P[fv][0] * w, y0 + PATCH_P[fv][1] * h])
    XY = patch_xy(None, pos, zoom, focus)
    # 簇颜色：原始分簇（组内的簇在选中后合成一色）；重新分簇后组内换成新的 2 色
    for ti, t_ in enumerate(stage_tris):
        in_g = stage_g[ti]
        lab = ORIG_LABEL.get(tuple(sorted(t_.tolist())))
        if lab is None:     # 坍缩后新出现的三角：沿用组内任一簇的颜色
            lab = next(iter(GROUP))
        col = PALETTE[PATCH_COLOR[lab]]
        if in_g:
            col = col * (1 - group_on * 0.75) + GROUP_TINT * group_on * 0.75
            if r2 >= 1.0 and RESPLIT[ti] >= 0:
                newc = RESPLIT_COLS[int(RESPLIT[ti]) % 2]
                col = col * (1 - resplit * 0.8) + newc * resplit * 0.8
            fa = 0.38 + 0.06 * group_on
        else:
            fa = 0.30 * (1 - 0.45 * group_on)
        # 图块边缘柔和渐隐 + 轻微径向衰减：浮在空间里，不是一块硬边矩形
        cxy = PATCH_P[t_].mean(0)
        edge = smooth(min(min(cxy[0], 1 - cxy[0]) / 0.12, min(cxy[1], 1 - cxy[1]) / 0.2))
        rad = 1.0 - 0.35 * smooth(math.hypot(cxy[0] - 0.5, (cxy[1] - 0.5) * 1.2) / 0.7)
        fk = edge * rad
        pts = [tuple(XY[i]) for i in t_]
        cv.poly(pts, col, a * fa * fk)
        for (p_, q_) in ((pts[0], pts[1]), (pts[1], pts[2]), (pts[2], pts[0])):
            cv.seg(p_, q_, col * 0.5 + LINE * 0.5, a * (0.35 if in_g else 0.22) * fk, 0.8)
    # 组边界（锁定）：金线 + 金色钉点
    if lock_on > 0.01:
        emap = defaultdict(list)
        for ti, t_ in enumerate(stage_tris):
            for e in ((t_[0], t_[1]), (t_[1], t_[2]), (t_[2], t_[0])):
                emap[(min(e), max(e))].append(ti)
        pulse = 0.8 + 0.2 * math.sin(t * 5.0) if t > 42.8 else 1.0
        bverts = set()
        for (u, v), ts in emap.items():
            flags = [stage_g[x] for x in ts]
            if any(flags) and (len(ts) == 1 or not all(flags)):
                cv.seg(XY[u], XY[v], AMBER, a * lock_on * pulse * 0.9, 2.6)
                bverts.update((u, v))
        for v in bverts:
            cv.dot(XY[v], 4.4 if zoom > 1.2 else 3.4, AMBER, a * lock_on)
            cv.dot(XY[v], 1.8 if zoom > 1.2 else 1.3, np.array([255, 240, 210], np.float32), a * lock_on)
    # 坍缩中的顶点：正在移动的那些打上亮点
    if 0 < r1 < 1 or 0 < r2 < 1:
        pairs = pairs0 if r1 < 1 else pairs1
        for v, u in pairs:
            cv.dot(XY[v], 2.2, CYAN, a * 0.9)
    # 顶部说明，按阶段轮换，互不重叠
    label(cv, "选 4 个相邻的簇成组，边界锁住", W / 2, 72, 30, a * window(t, 34.0, 38.0, 0.5), (255, 214, 140))
    label(cv, "只坍缩内部顶点：%d → %d 个三角" % (int(g0.sum()), int(g2.sum())), W / 2, 72, 30,
          a * window(t, 38.0, 42.2, 0.5), (200, 218, 240))
    label(cv, "再切回 2 个簇", W / 2, 72, 30, a * window(t, 42.2, 44.2, 0.4), (200, 218, 240))
    label(cv, "细的一侧与粗的一侧，共用同一串顶点 —— 没有裂缝", W / 2, 72, 30,
          a * window(t, 44.2, 48.4, 0.6), (255, 214, 140))


def scene_cut(cv, t):
    """48–58 s：金色距离波扫过岩块群，每块换成粗一级的簇（轮廓不变）；最后合成一份网格。"""
    a = window(t, 47.8, 58.6, 1.0)
    if a <= 0.01:
        return
    yaw = 0.35 + (t - 47.8) * 0.05
    pitch = -0.32
    dist, fov = 10.5, 1100.0
    draw_floor(cv, yaw, pitch, dist, fov, a * 0.8)
    wave = ease_in_out(seg(t, 49.2, 53.0))        # 0 → 1：距离波从镜头这一侧扫到最远
    merged = smooth(seg(t, 53.0, 54.4))
    # 距离波：地面上一圈金色椭圆（以镜头在地面的投影为圆心）
    if 0 < wave < 1:
        cam_ground = np.array([0.0, -1.15, -dist * 0.55], np.float32)
        radius = 1.0 + wave * 9.0
        ring = np.array([[cam_ground[0] + radius * math.cos(k * 2 * math.pi / 96), -1.15,
                          cam_ground[2] + radius * math.sin(k * 2 * math.pi / 96)] for k in range(97)], np.float32)
        P = ring @ rot(yaw, pitch).T
        z = P[:, 2] + dist
        for k in range(96):
            if z[k] < 0.5 or z[k + 1] < 0.5:
                continue
            cv.seg((W / 2 + fov * P[k, 0] / z[k], H / 2 - fov * P[k, 1] / z[k]),
                   (W / 2 + fov * P[k + 1, 0] / z[k + 1], H / 2 - fov * P[k + 1, 1] / z[k + 1]), GOLD, a * 0.9, 1.6)
        label(cv, "80 m", W / 2, 70, 34, a * smooth(seg(t, 49.2, 50.0)) * (1 - merged), (255, 214, 140), F_NUM)
    cam_ground_z = -dist * 0.55
    for r in ROCKS:
        # 这块岩石离"镜头地面投影"的距离决定距离波什么时候经过它
        rd = math.hypot(r["off"][0], r["off"][2] - cam_ground_z)
        passed = wave * 10.0 + 1.0 > rd
        flash = clamp01(1.0 - abs(wave * 10.0 + 1.0 - rd) * 1.6)
        cm = r["mid"] if passed else r["fine"]
        border_col = GOLD if (merged > 0.3 or flash > 0.2) else None
        draw_clustered(cv, cm, r["s"], yaw, pitch, dist, fov, a, shift=r["off"], fill=0.5,
                       inner=0.22, border=0.9 + 0.8 * flash, border_col=border_col, tint=merged * 0.55)
    num_a = a * smooth(seg(t, 53.0, 53.8))
    number(cv, 378362 + (49516 - 378362) * ease_out(seg(t, 53.4, 56.4)), W / 2, 64, 64, num_a)
    label(cv, "按屏幕误差 1 像素取簇 · 合成一份网格", W / 2, 140, 24, num_a, (200, 190, 160))


def house_edges_draw(cv, yaw, pitch, dist, fov, alpha, shift=(0, 0, 0)):
    pts, z = project(HOUSE_V, yaw, pitch, dist, fov, shift=shift)
    for a_, b_ in HOUSE_E:
        cv.seg(pts[a_], pts[b_], LINE, alpha, 1.4)
    rp, _ = project(ROOF_V, yaw, pitch, dist, fov, shift=shift)
    for a_, b_ in ROOF_E:
        cv.seg(rp[a_], rp[b_], LINE, alpha, 1.4)
    door = np.array([[-0.25, -0.7, 0.8], [0.25, -0.7, 0.8], [0.25, 0.1, 0.8], [-0.25, 0.1, 0.8]], np.float32)
    dp, _ = project(door, yaw, pitch, dist, fov, shift=shift)
    for k in range(4):
        cv.seg(dp[k], dp[(k + 1) % 4], CYAN, alpha * 0.8, 1.2)


def scene_visibility(cv, t):
    """（第一版时间轴，调用时减 SHIFT）视点球围住房子，光线落在外表面；屋里的物体化作尘埃。"""
    a = window(t, 44.0, 56.4, 1.0)
    if a <= 0.01:
        return
    yaw, pitch, dist, fov = 0.6 + (t - 44) * 0.12, -0.32, 7.0, 900.0
    house_edges_draw(cv, yaw, pitch, dist, fov, a)
    fade = 1.0 - smooth(seg(t, 50.0, 53.0))
    for (iv, iff), ie, off in zip(INNER, INNER_E, INNER_OFF):
        pts, z = project(iv * 0.22, yaw, pitch, dist, fov, shift=off)
        draw_mesh(cv, pts, z, ie, CYAN, a * fade * 0.9, 1.0)
        if 0 < seg(t, 50.0, 54.5) < 1:
            dt = seg(t, 50.0, 54.5)
            rng = np.random.default_rng(int(off[0] * 100) + 50)
            idx = rng.choice(len(pts), 24)
            for k in idx:
                ang = rng.uniform(0, 6.28)
                cv.dot((pts[k][0] + math.cos(ang) * 70 * dt, pts[k][1] + math.sin(ang) * 70 * dt - 40 * dt),
                       1.3, CYAN, a * (1 - dt) * 0.9)
    vs = smooth(seg(t, 44.4, 47.0))
    vp, vz = project(VIEWS * 2.6, yaw * 0.5, pitch, dist, fov)
    front = vz < dist
    for k in range(len(vp)):
        cv.dot(vp[k], 1.6 if front[k] else 1.0, LINE, a * vs * (0.9 if front[k] else 0.35))
    ray_t = seg(t, 46.5, 50.5)
    if ray_t > 0:
        rng = np.random.default_rng(3)
        shell = project(HOUSE_V, yaw, pitch, dist, fov)[0]
        for k in range(0, len(vp), 3):
            if not front[k]:
                continue
            ph = (ray_t * 3.0 + rng.random()) % 1.0
            target = shell[rng.integers(0, len(shell))]
            p0 = vp[k]
            p1 = p0 + (target - p0) * ph
            cv.seg(p0, p1, GOLD, a * 0.35 * (1 - ph), 0.8)
    num_a = a * smooth(seg(t, 50.5, 51.5))
    number(cv, 49516 + (40919 - 49516) * ease_out(seg(t, 51.0, 54.0)), W / 2, 64, 64, num_a)
    label(cv, "1024 个视点 · GPU 软件光栅", W / 2, 140, 24, num_a, (200, 190, 160))


ATLAS = {"x": W / 2 - 250, "y": 170, "s": 500}


def uv_cells(n):
    cols = int(math.ceil(math.sqrt(n * 1.2)))
    return cols, int(math.ceil(n / cols))


def scene_bake(cv, t):
    """（第一版时间轴）简化网格的三角依次飞进图集，三张贴图亮起，再贴回网格上。"""
    a = window(t, 55.8, 66.6, 1.0)
    if a <= 0.01:
        return
    r = ROCKS[2]
    yaw, pitch = 0.5 + (t - 56) * 0.15, -0.3
    pts, z = project(r["cv"] * 1.6, yaw, pitch, 6.0, 900.0, cx=W / 2, cy=H / 2 - 20)
    faces = r["cf"]
    n = len(faces)
    cols, rows = uv_cells(n)
    cell = ATLAS["s"] / cols
    rng = np.random.default_rng(9)
    hues = rng.uniform(0, 1, n)
    spins = rng.uniform(0, 2 * math.pi, n)
    order = rng.permutation(n)
    rest = r["cv"][faces] * 1.6
    for fi, (i0, i1, i2) in enumerate(faces):
        delay = order[fi] / n * 0.55
        k_out = ease_in_out(seg(seg(t, 57.0, 60.6), delay, delay + 0.45))
        k_back = ease_in_out(seg(seg(t, 63.0, 65.8), delay, delay + 0.45))
        k = k_out * (1 - k_back)
        cx_, cy_ = ATLAS["x"] + (fi % cols + 0.5) * cell, ATLAS["y"] + (fi // cols + 0.5) * cell
        flat = rest[fi][:, :2] - rest[fi][:, :2].mean(0)
        c, s = math.cos(spins[fi]), math.sin(spins[fi])
        flat = flat @ np.array([[c, -s], [s, c]])
        flat *= (cell * 0.46) / max(np.abs(flat).max(), 1e-6)
        tri2d = flat * np.array([1, -1]) + np.array([cx_, cy_])
        tri = pts[[i0, i1, i2]] * (1 - k) + tri2d * k
        warm = np.array([200 + 40 * hues[fi], 150 + 50 * hues[fi], 110 + 30 * (1 - hues[fi])], np.float32)
        fill_a = a * (0.35 * smooth(seg(t, 59.5, 61.0)) * k + 0.4 * smooth(seg(t, 64.5, 66.0)))
        cv.poly([tuple(p) for p in tri], warm, fill_a, LINE, a * (0.55 + 0.35 * k))
    frame_a = a * smooth(seg(t, 57.2, 58.2)) * (1 - smooth(seg(t, 63.2, 64.2)))
    if frame_a > 0.01:
        x0, y0, s = ATLAS["x"], ATLAS["y"], ATLAS["s"]
        for p, q in [((x0, y0), (x0 + s, y0)), ((x0 + s, y0), (x0 + s, y0 + s)), ((x0 + s, y0 + s), (x0, y0 + s)), ((x0, y0 + s), (x0, y0))]:
            cv.seg(p, q, CYAN, frame_a * 0.8, 1.2)
        label(cv, "2048 × 2048 图集", W / 2, y0 + s + 16, 22, frame_a, (170, 200, 235))
    pan_a = a * smooth(seg(t, 60.4, 61.6)) * (1 - smooth(seg(t, 63.0, 64.0)))
    if pan_a > 0.01:
        names = [("BaseColor", np.array([230, 180, 130], np.float32)), ("Normal", np.array([140, 150, 250], np.float32)),
                 ("Roughness", np.array([170, 170, 175], np.float32))]
        for j, (nm, col) in enumerate(names):
            px, py = W / 2 + 420, 200 + j * 160
            cv.poly([(px, py), (px + 120, py), (px + 120, py + 120), (px, py + 120)], col, pan_a * 0.5, LINE, pan_a)
            label(cv, nm, px + 60, py + 128, 20, pan_a, (200, 210, 230), F_NUM)
    label(cv, "开 Nanite 的静态网格", W / 2, 110, 30, a * smooth(seg(t, 64.5, 65.5)), (255, 214, 140))


def scene_distance(cv, t):
    """（第一版时间轴）距离轴上 80 m 切换，源换成截面；最后是片名。"""
    a = window(t, 66.2, 76.0, 1.0)
    if a > 0.01:
        y0, x0, x1 = H / 2 + 150, 260, W - 260
        cv.seg((x0, y0), (x1, y0), DIM, a, 1.2)
        for m in range(0, 121, 20):
            x = x0 + (x1 - x0) * m / 120
            col = GOLD if m == 80 else DIM
            cv.seg((x, y0 - (14 if m == 80 else 8)), (x, y0 + (14 if m == 80 else 8)), col, a, 1.4 if m == 80 else 1.0)
            label(cv, "%d m" % m, x, y0 + 22, 20, a, (255, 214, 140) if m == 80 else (140, 170, 210), F_NUM)
        travel = ease_in_out(seg(t, 67.5, 74.0))
        meters = 12 + 100 * travel
        cam_x = x0 + (x1 - x0) * meters / 120
        cv.poly([(cam_x - 12, y0 - 44), (cam_x + 12, y0 - 44), (cam_x, y0 - 24)], LINE, a * 0.9, base=False)
        swap = smooth((meters - 76) / 8.0)
        r = ROCKS[1]
        scale = 1.15 - 0.55 * travel
        yaw = 0.4 + t * 0.12
        rock_cy = H / 2 - 140
        fpts, fz = project(r["fv"] * scale, yaw, -0.3, 6.0, 900.0, cy=rock_cy)
        cpts, cz = project(r["cv"] * scale, yaw, -0.3, 6.0, 900.0, cy=rock_cy)
        draw_mesh(cv, fpts, fz, r["fe"], LINE, a * (1 - swap), 0.9)
        draw_mesh(cv, cpts, cz, r["ce"], GOLD, a * swap, 1.4)
        label(cv, "源 · 全精度", W / 2, rock_cy - 250, 30, a * (1 - swap), (214, 238, 255))
        label(cv, "截面 HLOD", W / 2, rock_cy - 250, 30, a * swap, (255, 214, 140))
        label(cv, "%d m" % int(meters), cam_x, y0 - 82, 22, a * 0.9, (214, 238, 255), F_NUM)


def draw_title(cv, t, t0, length=4.6):
    """片名：t0 起淡入，末尾 1 s 淡出。宣传片里在 89.4 s，合成版挪到录屏之后。"""
    ta = smooth(seg(t, t0, t0 + 1.2)) * (1 - smooth(seg(t, t0 + length - 1.0, t0 + length)))
    if ta > 0.01:
        stars(cv, t, ta * 0.8)
        text_center(cv, "Nanite 截面 HLOD", H / 2 - 80, F_TITLE, 76, (240, 244, 252), ta, spacing=6)
        text_center(cv, "378,362  →  49,516  →  40,919   ·   10.8%", H / 2 + 30, F_NUM, 30, (255, 214, 140), ta)
        text_center(cv, "PCGPlugins  ·  Unreal Engine 5.7", H / 2 + 86, F_EN, 22, (160, 185, 215), ta * 0.9)


def rec_rect(r):
    """录屏时间 r → 取景矩形（录屏像素），关键帧之间缓动插值。"""
    if r <= REC_SHOTS[0][0]:
        return REC_SHOTS[0][1]
    for (ta_, ra), (tb_, rb) in zip(REC_SHOTS, REC_SHOTS[1:]):
        if ta_ <= r <= tb_:
            k = ease_in_out(seg(r, ta_, tb_))
            return tuple(a_ + (b_ - a_) * k for a_, b_ in zip(ra, rb))
    return REC_SHOTS[-1][1]


REC_BOX = (1720, 930)            # 录屏在画面里占的最大框
REC_CENTER = (W / 2, 520)
_N_REC = []


def n_rec_frames():
    if not _N_REC:
        _N_REC.append(len([f for f in os.listdir(REC_DIR) if f.endswith(".jpg")]))
    return _N_REC[0]


def scene_demo(cv, t):
    """合成版：章节卡 + 录屏（叠在同一套背景上，带发光边框与投影）。"""
    ca = smooth(seg(t, DEMO_START + 0.2, DEMO_START + 1.0)) * (1 - smooth(seg(t, REC_START - 0.4, REC_START + 0.4)))
    if ca > 0.01:
        text_center(cv, "实 机 演 示", H / 2 - 70, F_TITLE, 64, (240, 244, 252), ca, spacing=8)
        text_center(cv, "Unreal Engine 5.7  ·  L_NaniteCutHLODTest", H / 2 + 26, F_EN, 26, (170, 195, 225), ca)
    if not (REC_START <= t < COMBINED_TITLE + 0.9):
        return
    r = t - REC_START
    pa = smooth(seg(t, REC_START, REC_START + 0.7)) * (1 - smooth(seg(t, COMBINED_TITLE - 0.2, COMBINED_TITLE + 0.8)))
    if pa <= 0.01:
        return
    idx = min(max(int(r * 30), 0), n_rec_frames() - 1)
    x0, y0, x1, y1 = rec_rect(min(r, REC_DUR))
    with Image.open(os.path.join(REC_DIR, "f_%05d.jpg" % (idx + 1))) as src:
        crop = src.crop((int(x0), int(y0), int(x1), int(y1)))
    s = min(REC_BOX[0] / (x1 - x0), REC_BOX[1] / (y1 - y0))
    dw, dh = int((x1 - x0) * s), int((y1 - y0) * s)
    crop = crop.resize((dw, dh), Image.LANCZOS)
    px, py = int(REC_CENTER[0] - dw / 2), int(REC_CENTER[1] - dh / 2)
    cv.photo = (np.asarray(crop.convert("RGB")), px, py, pa)
    # 发光细边框
    for p, q in (((px, py), (px + dw, py)), ((px + dw, py), (px + dw, py + dh)),
                 ((px + dw, py + dh), (px, py + dh)), ((px, py + dh), (px, py))):
        cv.seg(p, q, CYAN, pa * 0.55, 1.2)
    label(cv, "实机演示 · UE 5.7 编辑器", px + 150, py - 40, 24, pa * 0.9, (190, 210, 235))


def render_combined(frame):
    t = frame / FPS
    cv = Canvas()
    if t < DEMO_START + 1.0:
        _promo_scenes(cv, t, title=False)
    scene_demo(cv, t)
    draw_title(cv, t, COMBINED_TITLE + 0.3, COMBINED_DUR - COMBINED_TITLE - 0.3)
    img = cv.finish(t, frame)
    fade = smooth(seg(t, 0.0, 2.5)) * (1 - smooth(seg(t, COMBINED_DUR - 0.8, COMBINED_DUR)))
    if fade < 1:
        img = (img.astype(np.float32) * fade).astype(np.uint8)
    return img.tobytes()


def _promo_scenes(cv, t, title=True):
    if t < 23.5:
        scene_world(cv, t)
    if 22.5 < t < 34.0:
        scene_clusters(cv, t)
    if 32.5 < t < 48.6:
        scene_lock(cv, t)
    if 47.5 < t < 58.8:
        scene_cut(cv, t)
    ts = t - SHIFT
    if 43.5 < ts < 57.0:
        scene_visibility(cv, ts)
    if 55.5 < ts < 67.0:
        scene_bake(cv, ts)
    if ts > 66.0:
        scene_distance(cv, ts)
    if title:
        draw_title(cv, t, 89.4)
    draw_captions(cv, t)


def render(frame):
    t = frame / FPS
    cv = Canvas()
    _promo_scenes(cv, t, title=True)
    img = cv.finish(t, frame)
    fade = smooth(seg(t, 0.0, 2.5)) * (1 - smooth(seg(t, DURATION - 0.8, DURATION)))
    if fade < 1:
        img = (img.astype(np.float32) * fade).astype(np.uint8)
    return img.tobytes()


def preview():
    picks = [3.0, 11.0, 21.5, 25.5, 30.0, 34.8, 36.8, 39.2, 41.0, 43.2, 45.8, 50.8, 52.4, 56.0, 61.5, 66.5, 72.5,
             76.0, 83.0, 91.5]
    thumbs = []
    for s in picks:
        img = Image.frombytes("RGB", (W, H), render(int(s * FPS))).resize((480, 270), Image.LANCZOS)
        ImageDraw.Draw(img).text((8, 6), "%.1f s" % s, fill=(255, 220, 150), font=font(F_NUM, 16))
        thumbs.append(img)
    sheet = Image.new("RGB", (480 * 4, 270 * 5))
    for i, im in enumerate(thumbs):
        sheet.paste(im, ((i % 4) * 480, (i // 4) * 270))
    path = os.path.join(OUT_DIR, "promo_preview.png")
    sheet.save(path)
    print("->", path)


def extract_recording(ffmpeg):
    """录屏按 30 fps 恒定帧率拆成 JPEG（oCam 录的是可变帧率）。已拆过就跳过。"""
    if os.path.isdir(REC_DIR) and len([f for f in os.listdir(REC_DIR) if f.endswith(".jpg")]) >= int(REC_DUR * 30) - 2:
        return
    os.makedirs(REC_DIR, exist_ok=True)
    subprocess.run([ffmpeg, "-y", "-hide_banner", "-loglevel", "error", "-i", REC_SRC, "-vf", "fps=30", "-q:v", "2",
                    os.path.join(REC_DIR, "f_%05d.jpg")], check=True)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    combined = "--combined" in sys.argv
    fn = render_combined if combined else render
    if "--preview" in sys.argv:
        preview()
        return
    if "--frame" in sys.argv:
        s = float(sys.argv[sys.argv.index("--frame") + 1])
        path = os.path.join(OUT_DIR, "%s_frame_%05.1f.png" % ("demo" if combined else "promo", s))
        Image.frombytes("RGB", (W, H), fn(int(s * FPS))).save(path)
        print("->", path)
        return
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        sys.exit("找不到 ffmpeg")
    if combined:
        extract_recording(ffmpeg)
    out_mp4, music = (COMBINED_MP4, COMBINED_MUSIC) if combined else (MP4, MUSIC)
    n_frames = int((COMBINED_DUR if combined else DURATION) * FPS)
    # stderr 不能接管道又不读：加了音轨后 ffmpeg 输出变多，写满管道缓冲就会卡住 ffmpeg，
    # 它不再读 stdin，这边写帧也跟着卡死（2026-09-29 就这样停在第 0 帧）。只留错误、写进文件。
    cmd = [ffmpeg, "-y", "-hide_banner", "-loglevel", "error", "-nostats",
           "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "%dx%d" % (W, H), "-r", str(FPS), "-i", "-"]
    if os.path.exists(music):
        cmd += ["-i", music, "-map", "0:v", "-map", "1:a", "-c:a", "aac", "-b:a", "224k", "-shortest"]
    else:
        print("没有配乐（先跑 NaniteCutHLODPromoMusic.py%s），只出画面" % (" --combined" if combined else ""))
    cmd += ["-c:v", "libx264", "-preset", "slow", "-crf", "18", "-pix_fmt", "yuv420p", "-movflags", "+faststart", out_mp4]
    err_path = os.path.join(OUT_DIR, "ffmpeg_promo.log")
    with open(err_path, "wb") as err_file:
        proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=err_file)
        with Pool(max(1, (os.cpu_count() or 4) - 2)) as pool:
            for i, buf in enumerate(pool.imap(fn, range(n_frames), chunksize=4)):
                proc.stdin.write(buf)
                if i % 150 == 0:
                    print("  %d/%d" % (i, n_frames), flush=True)
        proc.stdin.close()
        code = proc.wait()
    if code != 0:
        with open(err_path, encoding="utf-8", errors="replace") as f:
            sys.exit("ffmpeg 失败：\n" + f.read()[-2000:])
    print("-> %s (%.1f MB, %.0f s)" % (out_mp4, os.path.getsize(out_mp4) / 1048576.0, n_frames / FPS))


if __name__ == "__main__":
    main()
