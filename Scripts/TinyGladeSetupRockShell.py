# -*- coding: utf-8 -*-
"""披挂岩壳（链 B）的演示关卡接线 + 逐项核对。可重跑。

给两张演示关卡的地面填 `RockShellMaterial`，然后在地形演示关卡上核对槽位池与可画性。

⚠️ 材质这一条是 2026-08 那个教训的直接执行面 —— GPU 石阶的 `StairMesh`/`StairMaterial` 在两张
演示关卡里一直是 NULL，石阶在画面里是一撮黑块，而单测 53/53、回归 55 条全绿。
岩壳的 `IsRockShellDrawable()` 因此把"有没有材质"做成了显式判据，这里必须真的填上。

2026-09-30 起图案是**程序化**的（`Docs/TinyGlade/RockShell_Sparse.md`，用户定 M = 16）：运行时不再读
TG 原件 `rocky_terrain_shell`，本脚本原来那一半「图案资产收尾」随之删掉；原件只留作离线对照
（`Scripts/PrototypeRockShellSparse.py` 直接读 .glb）。

用法::

    UnrealEditor-Cmd.exe <uproject> -ExecutePythonScript="<本文件>" -unattended -nopause -nosplash -abslog=<日志>
"""

import unreal

PKG = "/PCGPlugins/HouseTest"
SHELL_MAT = "/PCGPlugins/HouseTest/TinyGladeAsset/Materials/MI_rocky_terrain"

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
FAILS = []


def log(msg):
    unreal.log("ROCKSHELL %s" % msg)


def check(label, ok, detail=""):
    if ok:
        unreal.log("[PASS] %s %s" % (label, detail))
    else:
        FAILS.append(label)
        unreal.log_error("[FAIL] %s %s" % (label, detail))


def find(label):
    return next((a for a in ACTORS.get_all_level_actors() if a.get_actor_label() == label), None)


def wire_level(level, shell_mat):
    unreal.EditorLoadingAndSavingUtils.load_map("%s/%s" % (PKG, level))
    ground = find("Ground_Demo")
    if not ground:
        check("%s 有 Ground_Demo" % level, False)
        return None
    ground.set_editor_property("bRockShell", True)
    ground.set_editor_property("RockShellMaterial", shell_mat)
    ground.call_method("RebuildGroundMesh")   # 内部会重扫石阶并重披挂岩壳
    unreal.EditorLoadingAndSavingUtils.save_current_level()
    log("%s: RockShellMaterial = %s" % (level, shell_mat.get_name() if shell_mat else None))
    return ground


def pool_stats(ground):
    """(活跃胞腔, 池容量, 每槽三角)。UE Python 把「bool 返回值 + out 参数」收成：成功时只回
    out 参数的元组、失败时回 None；个别版本会把返回值也带上 —— 按长度分支，别写死下标。"""
    vals = ground.get_rock_shell_pool_stats()
    log("pool stats raw: %r" % (vals,))
    if vals is None:
        return None
    vals = list(vals) if isinstance(vals, (tuple, list)) else [vals]
    if len(vals) == 4:
        vals = vals[1:]
    if len(vals) != 3:
        check("get_rock_shell_pool_stats 的返回形状可识别", False, repr(vals))
        return None
    return int(vals[0]), int(vals[1]), int(vals[2])


def verify_pool(ground):
    stats = pool_stats(ground)
    check("槽位池已建", stats is not None and stats[1] > 0, repr(stats))
    if stats:
        active, capacity, per_cell = stats
        m = ground.get_editor_property("RockShellBoundaryPoints")
        log("pool: active=%d capacity=%d perCell=%d M=%d cellSize=%.1f cm"
            % (active, capacity, per_cell, m, ground.get_editor_property("RockShellCellSize")))
        check("每槽 5M 个三角", per_cell == 5 * m, "perCell=%d M=%d" % (per_cell, m))
        check("演示土台激活了胞腔", 0 < active <= capacity, "active=%d capacity=%d" % (active, capacity))
        gpu_indices = ground.debug_read_rock_shell_draw_index_count_gpu_sync()
        check("间接绘制的索引数 = 池容量 × 5M × 3", gpu_indices == capacity * per_cell * 3,
              "gpuIndices=%d want=%d" % (gpu_indices, capacity * per_cell * 3))

    # ⚠️ 坑 ⑩：一律调 `get_rock_shell_undrawable_reason()`，**不要** `is_rock_shell_drawable()` ——
    # 后者不可画时返回 `None`，`str(None) == "None"` 会伪造出一句像模像样的原因。
    # **空串 = 可画**（C++ 侧只在失败时才写原因）。⚠️ 别写成 `bool(返回值)` ——
    # 空串是 False，判据正好反过来，而"报 FAIL 但详情是空的"看起来像别的 bug（实测踩过一次）。
    why = str(ground.get_rock_shell_undrawable_reason())
    check("岩壳会被画出来（组件/网格/流/材质逐环检查）", why == "", why)


def main():
    shell_mat = unreal.EditorAssetLibrary.load_asset(SHELL_MAT)
    check("岩壳材质存在", shell_mat is not None, SHELL_MAT)

    ground = wire_level("L_TerrainOpsDemo", shell_mat)
    wire_level("L_HouseGroundDemo", shell_mat)

    if ground:
        # 回到地形演示关卡再核对（wire_level 之后关卡已经切走了）
        unreal.EditorLoadingAndSavingUtils.load_map("%s/L_TerrainOpsDemo" % PKG)
        ground = find("Ground_Demo")
        if ground:
            verify_pool(ground)

    unreal.log("========== ROCKSHELL SETUP SUMMARY ==========")
    for f in FAILS:
        unreal.log_error("  failed: %s" % f)
    unreal.log("ROCKSHELL SETUP FAILED" if FAILS else "ROCKSHELL SETUP OK")


try:
    main()
except Exception:
    # 准备阶段抛异常 ⇒ -ExecCmds 那条 Quit 再也执行不到，任务看着像挂死（踩过的坑）。
    import traceback
    unreal.log_error("ROCKSHELL SETUP EXCEPTION:\n%s" % traceback.format_exc())
    unreal.log("ROCKSHELL SETUP FAILED")
