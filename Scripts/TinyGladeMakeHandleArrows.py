# -*- coding: utf-8 -*-
"""
EditHouse（`ACSHouseActor::EnterResizeMode`）的推墙抓手、脊长抓手与楼梯拉宽用的 **TG 原版标识箭头**：把 TG 的
`flat_arrow`、它的描边 `flat_arrow_outline`、脊长那只 `brush_arrow_flat_single_foward`（以及备用的竖箭头
`brush_arrow_flat_single_up`）烘成静态网格。

--- 为什么是这几张 ----------------------------------------------------------------
- `flat_arrow` + `flat_arrow_outline`：TG 矩形房子拉尺寸（`rectangle/ui_modes/edit_dims.rs`，signifier id
  `rectangle__edit_dims_arrow_`）画的那一对，本体 + 外扩一圈、更薄的描边；TG 楼梯拉宽（`ui_focus_stairs`）也用它。
- `brush_arrow_flat_single_foward`（TG 原文如此拼写）：TG 拖脊长（`ui_edit_roof_ridge_length.rs`，signifier id
  `edit_roof_ridge_signifier`）在脊两端各画一只，尾在脊端外 0.2 m、放大 2 倍。TG 没给它做描边网格。
  本项目的脊长抓手 `ACSHouseRidgeHandleActor` 用它（2026-10-02，证据 `Docs/TinyGlade/RE_RoofRidgeGable.md`）。
- `brush_arrow_flat_single_up`：TG 聚焦模式的墙高 / 抬升控件（`ui_focus_mode_shape_height`，上下各一只，下面那只翻转）
  与屋顶高度控件（`ui_edit_roof_ridge_and_height_focused`，一只、放大 1.5）。TG 没给它做描边网格。
  ⚠️ 本项目的高度抓手 2026-09-22 试过这一版，用户判不如原先的四条框、已还原 —— 这张网格当前无人引用，留着备用。
证据（字符串表 + 反汇编）在 `Docs/TinyGlade/evidence/edit-signifier-arrows-20260922.txt`。

--- 输入 / 轴向 / 绕序 -------------------------------------------------------------
源 json 原样拷在 `Docs/TinyGlade/evidence/<名字>-source.json`（TG 米、Y 朝上、只有位置），
脚本只读这几份，不碰 `D:/MyProject/Tiny Glade`。

轴向沿用 TinyGladeAsset 的导入口径 **UE (X, Y, Z) = TG (X, Z, Y) × 100**（卷二「轴向换算」）。换完之后几张网格
共用一套轴语义，C++ 侧一律 `MakeFromYZ(Facing, Up)` 摆：
- `flat_arrow(_outline)`：躺平、尾在原点、**头指局部 +Y**、+Z 是厚度方向（Facing = 外法线）；
- `brush_arrow_flat_single_foward`：同样躺平、尾在原点、**头指局部 +Y**（Facing = 脊端朝外的那个方向），长 57.4 cm、宽 46.4 cm；
- `brush_arrow_flat_single_up`：竖板、**板面法线是局部 +Y**、箭根在原点、**头指 +Z**（Facing = 朝相机）。

换 Y/Z 是一次镜像（行列式 −1）。源绕序是「从外看顺时针」（有符号体积 < 0，与 `roof_tile-source.json`
同一约定），镜像之后变成逆时针；而 UE 的 DynamicMesh 取 `cross(e2, e1)` 当面法线（`VectorUtil::Normal`），
要求 `cross(b−a, c−a)` 朝**内**。所以每个三角形交换后两个下标，闭合的本体再用有符号体积断言一次。

--- 产物 ---------------------------------------------------------------------------
`/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/<名字>`（与其余 TG 网格同目录、同名）。
逐三角硬法线（本体是带倒角的薄板，平直着色就是 TG 的观感）；UV0 = 俯视平面投影，只为让切线有定义；
无碰撞（抓手的示意组件本来就 NoCollision，编辑器点选走 hit proxy）；不开 Nanite。
槽 0 挂 `M_CSHandleHighlight`，只影响内容浏览器里的样子 —— 抓手运行时自己挂材质（本体高亮、描边压暗）。
已存在就原地覆写几何（`copy_mesh_to_static_mesh`），不删资产，引用不断。

用法::

    UnrealEditor-Cmd.exe <uproject> -ExecutePythonScript=".../TinyGladeMakeHandleArrows.py" \\
        -unattended -nosplash -stdout -AbsLog=<独立日志>

日志自诊断：最后一行是 `HANDLE ARROWS OK` 或 `HANDLE ARROWS FAILED`。
"""
import json
import math
from pathlib import Path

import unreal

PKG = "/PCGPlugins/HouseTest/TinyGladeAsset/Meshes"
HIGHLIGHT = "/PCGPlugins/HouseTest/M_CSHandleHighlight"
EVIDENCE = Path(__file__).resolve().parents[1] / "Docs/TinyGlade/evidence"
M_TO_CM = 100.0

# (资产名, 源 json, 是否闭合)。闭合的才断言有符号体积 —— 描边有 8 条开边，体积没有意义。
ARROWS = [
    ("flat_arrow", "flat_arrow-source.json", True),
    ("flat_arrow_outline", "flat_arrow_outline-source.json", False),
    ("brush_arrow_flat_single_up", "brush_arrow_flat_single_up-source.json", True),
    ("brush_arrow_flat_single_foward", "brush_arrow_flat_single_foward-source.json", True),
]

_failed = []


def log(msg):
    unreal.log("[HandleArrows] %s" % msg)


def fail(msg):
    _failed.append(msg)
    unreal.log_error("[HandleArrows] %s" % msg)


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def load_source(file_name):
    data = json.loads((EVIDENCE / file_name).read_text(encoding="utf-8"))
    # UE (X, Y, Z) = TG (X, Z, Y) × 100
    positions = [(p[0] * M_TO_CM, p[2] * M_TO_CM, p[1] * M_TO_CM) for p in data["Vertex_Position"]["buffer"]]
    flat = data["indices"]["buffer"]
    # 换轴是镜像 ⇒ 交换后两个下标，保住 UE 的正面约定（见文件头）。
    triangles = [(flat[i], flat[i + 2], flat[i + 1]) for i in range(0, len(flat), 3)]
    return positions, triangles


def signed_volume(positions, triangles):
    return sum(dot(positions[a], cross(positions[b], positions[c])) for a, b, c in triangles) / 6.0


def build_dynamic_mesh(positions, triangles):
    vertices, normals, uv, tris = [], [], [], []
    for a, b, c in triangles:
        pa, pb, pc = positions[a], positions[b], positions[c]
        n = cross(sub(pb, pa), sub(pc, pa))
        length = math.sqrt(dot(n, n))
        if length <= 1.0e-9:
            fail("退化三角 (%d, %d, %d)" % (a, b, c))
            continue
        # UE 的面法线是 cross(e2, e1) = −cross(e1, e2)。
        n = (-n[0] / length, -n[1] / length, -n[2] / length)
        base = len(vertices)
        for p in (pa, pb, pc):
            vertices.append(unreal.Vector(*p))
            normals.append(unreal.Vector(*n))
            uv.append(unreal.Vector2D(p[0] / M_TO_CM, p[1] / M_TO_CM))
        tris.append(unreal.IntVector(base, base + 1, base + 2))
    buffers = unreal.GeometryScriptSimpleMeshBuffers(vertices=vertices, normals=normals, uv0=uv, triangles=tris)
    dynamic = unreal.DynamicMesh()
    dynamic.append_buffers_to_mesh(buffers)
    return dynamic, len(tris)


def write_asset(name, dynamic):
    path = "%s/%s" % (PKG, name)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        mesh = unreal.load_asset(path)
        options = unreal.GeometryScriptCopyMeshToAssetOptions(enable_recompute_normals=False,
                                                              enable_recompute_tangents=True)
        _, outcome = unreal.GeometryScript_AssetUtils.copy_mesh_to_static_mesh(
            dynamic, mesh, options, unreal.GeometryScriptMeshWriteLOD())
    else:
        options = unreal.GeometryScriptCreateNewStaticMeshAssetOptions(
            enable_recompute_normals=False, enable_recompute_tangents=True,
            enable_collision=False, enable_nanite=False)
        mesh, outcome = unreal.GeometryScript_NewAssetUtils.create_new_static_mesh_asset_from_mesh(
            dynamic, path, options)
    if not mesh or outcome != unreal.GeometryScriptOutcomePins.SUCCESS:
        fail("写不出 %s（%s）" % (path, outcome))
        return None

    if unreal.EditorAssetLibrary.does_asset_exist(HIGHLIGHT):
        mesh.set_material(0, unreal.load_asset(HIGHLIGHT))
    else:
        log("没找到 %s，槽 0 留默认材质（先跑 TinyGladeMakeHandleMaterial.py）" % HIGHLIGHT)

    if not unreal.EditorAssetLibrary.save_loaded_asset(mesh, False):
        fail("存不了盘 %s" % path)
    return mesh


def mismatch(mesh, positions, want_tris):
    """资产与换轴后的源数据差在哪：包围盒（0.05 cm 内）与三角数。一致返回空串。"""
    box = mesh.get_bounding_box()
    want_lo = [min(p[k] for p in positions) for k in range(3)]
    want_hi = [max(p[k] for p in positions) for k in range(3)]
    got_lo = (box.min.x, box.min.y, box.min.z)
    got_hi = (box.max.x, box.max.y, box.max.z)
    if any(abs(g - w) > 0.05 for g, w in zip(got_lo + got_hi, want_lo + want_hi)):
        return "包围盒 %s..%s，期望 %s..%s" % (got_lo, got_hi, want_lo, want_hi)
    tris = mesh.get_num_triangles(0)
    if tris != want_tris:
        return "三角数 %d，期望 %d" % (tris, want_tris)
    return ""


def verify(name, mesh, positions, want_tris):
    """回读资产：包围盒必须逐位等于换轴后的源数据，三角数必须等于源三角数。"""
    problem = mismatch(mesh, positions, want_tris)
    if problem:
        fail("%s %s" % (name, problem))
    box = mesh.get_bounding_box()
    log("%s: %d 三角，包围盒 (%.1f, %.1f, %.1f)..(%.1f, %.1f, %.1f) cm"
        % (name, mesh.get_num_triangles(0), box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z))


for name, source, closed in ARROWS:
    positions, triangles = load_source(source)
    if closed:
        volume = signed_volume(positions, triangles)
        # < 0 ⇔ cross(b−a, c−a) 朝内 ⇔ UE 眼里朝外的是正面。
        if volume >= 0.0:
            fail("%s 换轴后的有符号体积 %.1f cm³ 应为负：绕序没翻对" % (name, volume))
    dynamic, tri_count = build_dynamic_mesh(positions, triangles)
    # 已经烘过、且与源一致的不重写：重存一遍几何一个字节不变，资产文件却会变（内容仓里就多一条无谓的改动）。
    # 往 ARROWS 里加一张新网格时，只有它会被写出来。
    path = "%s/%s" % (PKG, name)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        existing = unreal.load_asset(path)
        if existing and not mismatch(existing, positions, tri_count):
            log("%s: 已是最新，跳过" % name)
            continue
    mesh = write_asset(name, dynamic)
    if mesh:
        verify(name, mesh, positions, tri_count)

log("HANDLE ARROWS FAILED" if _failed else "HANDLE ARROWS OK")
