# -*- coding: utf-8 -*-
"""把 Tiny Glade 原件碎裂图案 `rocky_terrain_shell.glb` 导成 StaticMesh。

**在 UE 编辑器里跑**（`-ExecutePythonScript` 或编辑器 Python 控制台），可重跑：
已存在就先删再导，不会积出 `_1` `_2`。

它是披挂岩壳（链 B）的唯一离线输入，逐顶点胞腔数据打包在多 UV 通道里，
所以**这几个导入选项是承重的，不是调优**：焊接会毁掉 `Tri*3+k` 的逐三角展开，
半精度 UV 会毁掉 `cell_id`，不开 CPU 访问就读不到顶点缓冲去填 aux 流。
逐项理由与实测数字见 `Docs/TinyGlade/RockShell_Pattern.md`。

⚠️ **别导错文件**：`assets/meshes/terrain_rocks.json`（已经在
`/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/terrain_rocks`）是 ±430 m 的背景岩石，零 cell 属性，**不是这一份**。
本脚本要的是 `assets/data/rocky_terrain.json` 经 `extract/rocky_terrain2glb.py` 导出的那份。

用法::

    UnrealEditor-Cmd.exe <uproject> -run=pythonscript -script="<本文件>"
    # 或编辑器 Python 控制台： exec(open(r"<本文件>", encoding="utf-8").read())
"""

import os

import unreal

# =============================================================================
# 参数
# =============================================================================

# 可用环境变量 ROCKSHELL_SRC_GLB 覆盖：假倒角管线用它指向
# Docs/TinyGlade/geo/bevel/rocky_terrain_shell.glb（COLOR_0 已按通道字典 v2 重烘，
# 见 Docs/TinyGlade/RockShell_EdgeBevel.md）。不设则导原件（COLOR_0 = 胞腔辨认色）。
SRC_GLB = os.environ.get(
    "ROCKSHELL_SRC_GLB",
    r"D:\MyProject\Tiny Glade\extracted\meshes\rocky_terrain_shell.glb")

# Interchange 的 glTF 管线在 DEST_PATH 下建 <源文件名>/StaticMeshes/ 两层（实测），
# 所以源文件必须叫 rocky_terrain_shell.glb —— 换名会导去别的文件夹，盖不掉
# CSGroundRockShell.cpp 的 DefaultPatternAssetPath。
DEST_PATH = "/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/rocky_terrain_shell"
ASSET_NAME = "rocky_terrain_shell"
EXPECTED_ASSET = "%s/%s/StaticMeshes/%s" % (DEST_PATH, ASSET_NAME, ASSET_NAME)

# GLB 的 POSITION 已经是 **米**（导出时乘过 ×65 的 rest→world 换算）。
# UE 的 glTF 导入按 1 uu = 1 cm 收，所以这里给 100 把米换成厘米；
# ×65 那一步**不要再乘一次**，它已经烘在 POSITION 里了。
IMPORT_UNIFORM_SCALE = 100.0

# 核对用（来自 Scripts/VerifyRockShellGlb.py 对原 GLB 的实测）
EXPECT_TRIANGLES = 49598
EXPECT_VERTICES = 148794
EXPECT_UV_CHANNELS = 3


def log(msg):
    unreal.log("ROCKSHELL %s" % msg)


def fail(msg):
    unreal.log_error("ROCKSHELL IMPORT FAILED: %s" % msg)
    raise SystemExit(1)


# =============================================================================
# 导入
# =============================================================================

def build_task():
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", SRC_GLB)
    task.set_editor_property("destination_path", DEST_PATH)
    task.set_editor_property("destination_name", ASSET_NAME)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("automated", True)      # 不弹导入对话框
    task.set_editor_property("save", True)
    return task


# UE 5.7 起 EditorStaticMeshLibrary 已弃用，改走 StaticMeshEditorSubsystem。
# 两个都试，谁在用谁 —— 这个脚本要能跨版本重跑。
def _mesh_api():
    try:
        sub = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
        if sub:
            return sub
    except Exception:
        pass
    return unreal.EditorStaticMeshLibrary


MESH_API = _mesh_api()


def apply_build_settings(mesh):
    """把承重的三项写进每级 LOD 的 BuildSettings，再重建一次。

    Interchange 的 glTF 管线没有暴露这几项，所以只能导完再改；改完必须 build() 才生效。
    """
    changed = []
    for lod in range(MESH_API.get_lod_count(mesh)):
        bs = MESH_API.get_lod_build_settings(mesh, lod)

        # 1) 全精度 UV：默认 UV 走 FVector2DHalf。cell_id 最大 608，FP16 还能精确表示
        #    (≤2048 的整数是精确的)，但 dir_to_centroid 只剩 ~5e-4，而且换成自己烘的图案
        #    胞腔数会超过 2048 —— 到那时是静默的错值，不是报错。
        bs.set_editor_property("use_full_precision_u_vs", True)

        # 2) 不生成光照贴图 UV：会占掉一个 UV 通道并打乱通道编号。
        bs.set_editor_property("generate_lightmap_u_vs", False)

        # 3) 不移除退化三角：碎裂图案里没有退化三角，但这一项开着会静默改变三角数，
        #    让下面的核对失去意义。
        bs.set_editor_property("remove_degenerates", False)

        MESH_API.set_lod_build_settings(mesh, lod, bs)
        changed.append(lod)

    # 4) CPU 访问：运行时要读顶点/索引缓冲把数据搬进 aux 流。不开的话打包后
    #    顶点缓冲只在显存里，CPU 侧读到空。
    mesh.set_editor_property("allow_cpu_access", True)

    # 5) Nanite 必须关：Nanite 会重建簇、丢掉我们依赖的逐三角展开与 UV 语义。
    nanite = mesh.get_editor_property("nanite_settings")
    nanite.set_editor_property("enabled", False)
    mesh.set_editor_property("nanite_settings", nanite)

    # set_lod_build_settings 内部就会触发重建（StaticMeshEditorSubsystem::SetLodBuildSettings
    # → BuildStaticMesh）；5.7 的 python StaticMesh 上没有 build()，别再调它。
    log("build settings applied to LOD %s + allow_cpu_access + Nanite off" % changed)


def verify(mesh):
    """导完立刻核对。对不上就报出来 —— 下一位写 kernel 的人会拿这些数字当真。"""
    ok = True
    # 5.7：三角/顶点数是 StaticMesh 本体的方法；subsystem 上没有老库的 get_number_* 名字。
    tris = mesh.get_num_triangles(0)
    verts = mesh.get_num_vertices(0)
    uvs = MESH_API.get_num_uv_channels(mesh, 0)
    bounds = mesh.get_bounding_box()
    size = bounds.max - bounds.min

    log("triangles %d (expect %d)" % (tris, EXPECT_TRIANGLES))
    log("vertices  %d (GLB soup had %d; UE 焊接后会更少，用索引缓冲展回来即可)"
        % (verts, EXPECT_VERTICES))
    log("uv channels %d (expect >= %d)" % (uvs, EXPECT_UV_CHANNELS))
    log("bounds cm  x %.1f  y %.1f  z %.1f" % (size.x, size.y, size.z))
    log("  -> 期望：平面两轴各 13650 cm（±68.25 m ×100），厚度轴 312 cm（顶圈 3.12 m）")

    if tris != EXPECT_TRIANGLES:
        unreal.log_error("ROCKSHELL !! 三角数 %d != %d —— 多半是 remove_degenerates 或 LOD 生成没关"
                         % (tris, EXPECT_TRIANGLES))
        ok = False
    if uvs < EXPECT_UV_CHANNELS:
        unreal.log_error("ROCKSHELL !! UV 通道只有 %d，胞腔数据丢了。检查 generate_lightmap_u_vs "
                         "与导入管线是否合并了 UV" % uvs)
        ok = False
    flat = sorted([size.x, size.y, size.z])
    if flat[0] > 400.0:
        unreal.log_error("ROCKSHELL !! 最薄的一轴 %.1f cm，图案应该是平的（312 cm）—— "
                         "检查 IMPORT_UNIFORM_SCALE 是不是又乘了一次 65" % flat[0])
        ok = False
    lods = MESH_API.get_lod_count(mesh)
    if lods != 1:
        unreal.log_warning("ROCKSHELL !! LOD 数 %d != 1；岩壳只用 LOD0，多余 LOD 只是浪费" % lods)

    # UV 值域：cell_id 存在 TEXCOORD_1.x，值域必须是 0..608。有些导入路径会把 UV 归一化到
    # [0,1] —— 一旦归一化，cell_id 就废了，而且是静默的。这一项没法从 StaticMesh 的 Python
    # 接口直接读逐顶点 UV，首次导入后请在 StaticMesh 编辑器的 UV 视图里目视确认 UV1 的范围
    # 远大于 1（或用 C++/蓝图取 LOD0 的 UV 缓冲打一行日志）。
    log("!! 还需人工确认：UV1.x 的值域应是 0..608（不是 0..1）。见 Docs/TinyGlade/RockShell_Pattern.md")
    return ok


def main():
    if not os.path.isfile(SRC_GLB):
        fail("找不到原件 %s\n"
             "  它由 D:/MyProject/Tiny Glade/extract/rocky_terrain2glb.py 生成，先跑那个。" % SRC_GLB)

    if unreal.EditorAssetLibrary.does_asset_exist(EXPECTED_ASSET):
        log("existing asset found, deleting for a clean re-import: %s" % EXPECTED_ASSET)
        unreal.EditorAssetLibrary.delete_asset(EXPECTED_ASSET)

    task = build_task()
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    imported = list(task.get_editor_property("imported_object_paths") or [])
    log("imported object paths: %s" % imported)

    mesh = unreal.EditorAssetLibrary.load_asset(EXPECTED_ASSET)
    if not mesh:
        for path in imported:
            candidate = unreal.EditorAssetLibrary.load_asset(path.split(".")[0])
            if isinstance(candidate, unreal.StaticMesh):
                mesh = candidate
                break
    if not isinstance(mesh, unreal.StaticMesh):
        fail("导入后找不到 StaticMesh。imported_object_paths = %s" % imported)

    log("asset: %s" % mesh.get_path_name())
    apply_build_settings(mesh)
    ok = verify(mesh)
    unreal.EditorAssetLibrary.save_loaded_asset(mesh)
    log("saved. verification %s" % ("OK" if ok else "FAILED — 见上面的 !! 行"))


main()
