# -*- coding: utf-8 -*-
"""岩壳假倒角一键落地（**UE 编辑器里跑**）。做三件事：

1. 用 `ROCKSHELL_SRC_GLB` 指向烘过倒角通道的 glb 重导图案 StaticMesh
   （先跑 `BakeRockShellBevelChannels.py` 产出它）；环境变量 `ROCKBEVEL_SKIP_REIMPORT=1` 可跳过；
2. 从 `M_TinyGladeStone` 复制出 `M_TinyGladeRockShell`，加装假倒角子图（直摆图案看图用）；
3. 把倒角子图嫁接进 TG 母材质 `M_TG_Texture`（静态开关 `RockShellBevel` 守卫）并给
   `MI_rocky_terrain` 打开开关 —— 演示关卡的地面 `RockShellMaterial` 就是它，运行时岩壳由此得到倒角。

材质构建在 `BuildRockShellBevelMaterial.py`（本脚本只是把三步串起来）。
机制、通道字典 v2、参数含义见 `Docs/TinyGlade/RockShell_EdgeBevel.md`。

用法::

    UnrealEditor-Cmd.exe <uproject> -ExecutePythonScript="<本文件>" -unattended -nosplash -stdout

日志自诊断：最后一行是 `ROCKBEVEL SETUP DONE` 或 `ROCKBEVEL SETUP FAILED`。
"""

import os

import unreal

SCRIPTS = os.path.dirname(os.path.abspath(__file__))
BEVEL_GLB = os.path.normpath(os.path.join(
    SCRIPTS, "..", "Docs", "TinyGlade", "geo", "bevel", "rocky_terrain_shell.glb"))

# 首轮踩坑的清理：源文件曾叫 *_bevel.glb，Interchange 按文件名建夹导去了这里。
STRAY_FOLDER = "/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/rocky_terrain_shell/rocky_terrain_shell_bevel"


def log(msg):
    unreal.log("ROCKBEVEL %s" % msg)


# =============================================================================
# 1) 重导图案（COLOR_0 = 通道字典 v2）
# =============================================================================

if os.environ.get("ROCKBEVEL_SKIP_REIMPORT"):
    log("pattern re-import skipped (ROCKBEVEL_SKIP_REIMPORT)")
else:
    if not os.path.isfile(BEVEL_GLB):
        unreal.log_error("ROCKBEVEL 找不到 %s —— 先跑 BakeRockShellBevelChannels.py" % BEVEL_GLB)
        log("SETUP FAILED")
        raise SystemExit(1)

    if unreal.EditorAssetLibrary.does_directory_exist(STRAY_FOLDER):
        unreal.EditorAssetLibrary.delete_directory(STRAY_FOLDER)
        log("stray folder deleted: %s" % STRAY_FOLDER)

    os.environ["ROCKSHELL_SRC_GLB"] = BEVEL_GLB
    exec(open(os.path.join(SCRIPTS, "TinyGladeImportRockShell.py"), encoding="utf-8").read())
    log("pattern re-imported from %s" % BEVEL_GLB)

# =============================================================================
# 2) + 3) 消费材质（共享构建器：BuildRockShellBevelMaterial.py）
# =============================================================================

ROCKBEVEL_AS_LIBRARY = True
exec(open(os.path.join(SCRIPTS, "BuildRockShellBevelMaterial.py"), encoding="utf-8").read())
ok_standalone = build_rockshell_bevel_material() is not None
ok_master = graft_rockshell_bevel_into_master()
log("standalone=%s master=%s" % (ok_standalone, ok_master))
log("SETUP DONE" if (ok_standalone and ok_master) else "SETUP FAILED")
