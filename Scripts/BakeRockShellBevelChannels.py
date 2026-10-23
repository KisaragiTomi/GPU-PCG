# -*- coding: utf-8 -*-
"""把岩壳假倒角的三个逐顶点量烘进 `rocky_terrain_shell.glb` 的 COLOR_0。

**系统 python 直接跑，不依赖 UE**。产物是一份新 glb（原件不动），供
`TinyGladeImportRockShell.py` 以 `ROCKSHELL_SRC_GLB` 环境变量指过去重导。
通道字典与消费端材质见 `Docs/TinyGlade/RockShell_EdgeBevel.md`。

打包布局（与 `CSGroundRockShell.h` 的顶点色字典 v2 逐位一致，中性值必须是 1）::

    R = bIsCapTri            # 1 = 盖 / 0 = 裙（沿用字典 v1，语义不变）
    G = 到上沿折痕的平面距离  # saturate(dist_m / 0.5)，1 = 远（中性：无倒角）
    B = 逐石头相位            # fract(cell_id * 黄金比)，仅作噪声域偏移
    A = 外向方向角            # atan2(-dir_to_centroid) / 2π，盖侧法线弯的目标

距离**只在图案平面（glb 的 XZ）里量**：裙面在静止姿态里竖直展开 3.12 m，
运行时会被高度替换压扁到几十 cm，量三维距离会让整张裙读成"远"。

用法::

    python BakeRockShellBevelChannels.py
"""

import json
import math
import os
import struct
import sys
from collections import defaultdict

GEO_DIR = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                        "..", "Docs", "TinyGlade", "geo"))
SRC_GLB = os.path.join(GEO_DIR, "rocky_terrain_shell.glb")
# ⚠️ 文件名必须与原件相同：Interchange 用**源文件名**当资产内层文件夹名，
# 换名会导去 `.../rocky_terrain_shell_bevel/...` 而盖不掉 `DefaultPatternAssetPath` 指的正主。
DST_GLB = os.path.join(GEO_DIR, "bevel", "rocky_terrain_shell.glb")

DIST_MAX_M = 0.5          # G 通道的归一上限；材质端 DistMax 参数必须同值（50 cm）
GOLDEN = 0.6180339887     # 相位散列
POS_KEY_DECIMALS = 4      # 顶点位置匹配精度（1e-4 m = 0.1 mm）

COMP = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2),
        5123: ("H", 2), 5125: ("I", 4), 5126: ("f", 4)}
NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}


def read_glb(path):
    data = open(path, "rb").read()
    magic, _ver, total = struct.unpack_from("<III", data, 0)
    assert magic == 0x46546C67, "not a glb"
    off, chunks = 12, {}
    while off < total:
        clen, ctype = struct.unpack_from("<II", data, off)
        chunks[ctype] = bytearray(data[off + 8:off + 8 + clen])
        off += 8 + clen
    return json.loads(bytes(chunks[0x4E4F534A])), chunks[0x004E4942], data[:12]


def accessor_view(g, idx):
    """返回 (fmt, stride, start, count, ncomp)，直接在 bin 上定位。"""
    a = g["accessors"][idx]
    bv = g["bufferViews"][a["bufferView"]]
    fmt, size = COMP[a["componentType"]]
    n = NCOMP[a["type"]]
    stride = bv.get("byteStride", size * n)
    start = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    return fmt, stride, start, a["count"], n, size, a


def read_accessor(g, bin_, idx):
    fmt, stride, start, count, n, _size, _a = accessor_view(g, idx)
    unpack = struct.Struct("<" + fmt * n).unpack_from
    return [unpack(bin_, start + i * stride) for i in range(count)]


def main():
    g, bin_, _header = read_glb(SRC_GLB)
    prim = g["meshes"][0]["primitives"][0]
    attrs = prim["attributes"]
    for need in ("POSITION", "TEXCOORD_0", "TEXCOORD_1", "TEXCOORD_2", "COLOR_0"):
        if need not in attrs:
            sys.exit("missing attribute %s" % need)

    pos = read_accessor(g, bin_, attrs["POSITION"])       # 米，Y-up：平面 = XZ
    uv0 = read_accessor(g, bin_, attrs["TEXCOORD_0"])     # dir_to_centroid.xz（指向质心）
    uv1 = read_accessor(g, bin_, attrs["TEXCOORD_1"])     # (cell_id, is_corner)
    uv2 = read_accessor(g, bin_, attrs["TEXCOORD_2"])     # (cell_bby, is_top)
    idx = ([t[0] for t in read_accessor(g, bin_, prim["indices"])]
           if "indices" in prim else list(range(len(pos))))
    tri_count = len(idx) // 3
    print("verts %d  tris %d" % (len(pos), tri_count))

    key = lambda p: (round(p[0], POS_KEY_DECIMALS), round(p[2], POS_KEY_DECIMALS))

    # --- 每胞腔的盖轮廓线段：只被一个盖三角使用的边（平面 XZ）------------------
    edge_use = defaultdict(int)      # (cell, kA, kB) -> 盖三角使用次数
    edge_seg = {}
    cell_of_vert = [round(v[0]) for v in uv1]
    cap_cells = set()
    for t in range(tri_count):
        i0, i1, i2 = idx[t * 3], idx[t * 3 + 1], idx[t * 3 + 2]
        if uv2[i0][1] < 0.5:
            continue                                     # 裙三角不参与轮廓
        cell = cell_of_vert[i0]
        cap_cells.add(cell)
        for a, b in ((i0, i1), (i1, i2), (i2, i0)):
            ka, kb = key(pos[a]), key(pos[b])
            ek = (cell,) + tuple(sorted((ka, kb)))
            edge_use[ek] += 1
            edge_seg[ek] = (pos[a][0], pos[a][2], pos[b][0], pos[b][2])

    rim = defaultdict(list)          # cell -> [(ax, az, bx, bz)]
    for ek, n in edge_use.items():
        if n == 1:
            rim[ek[0]].append(edge_seg[ek])
    n_rim = sum(len(v) for v in rim.values())
    print("cells with caps %d  rim segments %d" % (len(cap_cells), n_rim))

    def dist_to_rim(cell, x, z):
        segs = rim.get(cell)
        if not segs:
            return DIST_MAX_M                            # 无盖胞腔：远（中性）
        best = 1e30
        for ax, az, bx, bz in segs:
            dx, dz = bx - ax, bz - az
            t = ((x - ax) * dx + (z - az) * dz) / max(dx * dx + dz * dz, 1e-12)
            t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
            ex, ez = ax + dx * t - x, az + dz * t - z
            d2 = ex * ex + ez * ez
            if d2 < best:
                best = d2
        return math.sqrt(best)

    # --- 逐顶点打包 ------------------------------------------------------------
    rgba = []
    dists = []
    for v in range(len(pos)):
        cell = cell_of_vert[v]
        cap = 1.0 if uv2[v][1] > 0.5 else 0.0
        d = dist_to_rim(cell, pos[v][0], pos[v][2])
        dists.append(d)
        g01 = min(d / DIST_MAX_M, 1.0)
        phase = (cell * GOLDEN) % 1.0
        ox, oz = -uv0[v][0], -uv0[v][1]                  # 外向 = −指向质心
        theta = (math.atan2(oz, ox) / (2.0 * math.pi)) % 1.0
        rgba.append((cap, g01, phase, theta))

    ds = sorted(dists)
    print("rim dist m: p10 %.3f  med %.3f  p90 %.3f  max %.3f"
          % (ds[len(ds) // 10], ds[len(ds) // 2], ds[9 * len(ds) // 10], ds[-1]))
    in_band = sum(1 for d in dists if d < 0.25)
    print("verts within 25cm of rim: %d (%.1f%%)" % (in_band, 100.0 * in_band / len(dists)))

    # --- 原地覆写 COLOR_0（保持原 accessor 的组件类型与布局，字节数不变）--------
    fmt, stride, start, count, n, size, acc = accessor_view(g, attrs["COLOR_0"])
    assert count == len(rgba), "COLOR_0 count mismatch"
    normalized = acc.get("normalized", False)
    print("COLOR_0: %s x%d  normalized=%s  stride=%d" % (fmt, n, normalized, stride))

    def encode(vals):
        vals = vals[:n]                                  # VEC3 就丢掉 A（届时 θ 走不了，报错）
        if fmt == "f":
            return struct.pack("<" + "f" * n, *vals)
        lim = 255.0 if fmt == "B" else 65535.0
        return struct.pack("<" + fmt * n, *(int(round(v * lim)) for v in vals))

    if n < 4:
        sys.exit("COLOR_0 只有 %d 个分量，装不下 4 个通道 —— 需要改成 VEC4 重导出" % n)
    for i, vals in enumerate(rgba):
        bin_[start + i * stride:start + i * stride + size * n] = encode(list(vals))

    # --- 写出新 glb（JSON 原样，BIN 用打过补丁的那份）---------------------------
    json_bytes = json.dumps(g, separators=(",", ":")).encode("utf-8")
    json_bytes += b" " * ((4 - len(json_bytes) % 4) % 4)
    while len(bin_) % 4:
        bin_.append(0)
    total = 12 + 8 + len(json_bytes) + 8 + len(bin_)
    os.makedirs(os.path.dirname(DST_GLB), exist_ok=True)
    with open(DST_GLB, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(json_bytes), 0x4E4F534A))
        f.write(json_bytes)
        f.write(struct.pack("<II", len(bin_), 0x004E4942))
        f.write(bytes(bin_))
    print("written %s (%.1f MB)" % (DST_GLB, os.path.getsize(DST_GLB) / 1048576.0))


if __name__ == "__main__":
    main()
