# -*- coding: utf-8 -*-
"""
给地被里的花配「聚散」（2026-09-29 用户："让花的分布有一些聚散"，参考图
`Docs/TinyGlade/img/tiny-glade-ref-flower-clusters-20260929.png`：低矮的花一片一片地密、片与片之间只剩零星几朵；
成丛的高花三五株一丛）。

**只动聚散那四个参数与密度**，花的其余设置（材质、缩放、高度偏移、手调过的密度……）原样保留 ——
所以不重跑 `TinyGladeSetupGroundCover.py`（它会把关卡里手调的值冲回表里的默认）。

密度怎么改：开聚散之后平均保留比例 = `Sparse + (1 − Sparse) × Coverage`，把原密度除以它 ⇒ 总株数与改之前差不多，
只是重新分布成一片一片的。**幂等**：只在这一行花还没开聚散（`PatchSize == 0`）时才除，重跑不会越除越密。
想退回均匀散布：把 `PatchSize` 设回 0、密度乘回去（日志里打了原值）。

改三处：`BP_TinyGladeGround` 的 CDO（新拖进关卡的实例），`L_TerrainOpsDemo` / `L_HouseGroundDemo` 里的地面实例。
⚠️ 在**用户开着的编辑器**里跑（MCPUnreal execute_script）；会切关卡并**保存**那两张关卡 —— 跑之前确认没有未存的手工修改。
"""
import unreal

PKG = "/PCGPlugins/HouseTest"
LEVELS = ("L_TerrainOpsDemo", "L_HouseGroundDemo")

# 按网格名配：(PatchSize cm, Coverage, Softness, SparseDensity)
PATCHES = {
    # 低矮的单株花（参考图里粉色那种）：一片 2–4 m、三成多的地面在片里，片外还留零星几朵。
    "lowpoly_flower": (350.0, 0.35, 0.35, 0.12),
    # 成丛的高花（参考图里蓝紫色的风信子那种）：一丛 1 m 上下、片外几乎没有 ⇒ 读成"三五株一丛"。
    "garden_flower_01_lavender": (120.0, 0.10, 0.15, 0.02),
}
# 没在表里的花：一个温和的默认。
DEFAULT_PATCH = (300.0, 0.35, 0.3, 0.1)

FAILED = []


def log(msg):
    unreal.log("FLOWERPATCH %s" % msg)


def patch_flowers(obj, where):
    flowers = list(obj.get_editor_property("Flowers"))
    changed = 0
    for index, species in enumerate(flowers):
        mesh = species.get_editor_property("Mesh")
        name = mesh.get_name() if mesh else "<none>"
        size, coverage, softness, sparse = PATCHES.get(name, DEFAULT_PATCH)
        old_size = species.get_editor_property("PatchSize")
        old_density = species.get_editor_property("DensityPerSqM")
        density = old_density
        if old_size <= 0.0:
            keep = sparse + (1.0 - sparse) * coverage
            density = old_density / max(keep, 1e-3)
        species.set_editor_property("PatchSize", size)
        species.set_editor_property("PatchCoverage", coverage)
        species.set_editor_property("PatchSoftness", softness)
        species.set_editor_property("PatchSparseDensity", sparse)
        species.set_editor_property("DensityPerSqM", density)
        flowers[index] = species
        changed += 1
        log("%-40s 花 %d %-26s 片 %.0f cm / 覆盖 %.2f / 片边 %.2f / 片外 %.2f；密度 %.3f -> %.3f 株/m²%s"
            % (where, index, name, size, coverage, softness, sparse, old_density, density,
               "" if old_size <= 0.0 else "（已开过聚散，密度不再除）"))
    # ⚠️ TArray 属性经 Python 写回**不发**变更通知（单个结构体属性会发）⇒ 地面不会自己重散，下面显式叫一次。
    obj.set_editor_property("Flowers", flowers)
    return changed


bp = unreal.EditorAssetLibrary.load_asset("%s/BP_TinyGladeGround" % PKG)
if bp:
    patch_flowers(unreal.get_default_object(bp.generated_class()), "CDO")
    unreal.EditorAssetLibrary.save_loaded_asset(bp)
else:
    FAILED.append("BP_TinyGladeGround 不存在")

actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
for level in LEVELS:
    unreal.EditorLoadingAndSavingUtils.load_map("%s/%s" % (PKG, level))
    count = 0
    for actor in actors.get_all_level_actors():
        class_name = actor.get_class().get_name()
        if "Ground" not in class_name or "Shaper" in class_name:
            continue
        if patch_flowers(actor, "%s/%s" % (level, actor.get_actor_label())):
            actor.call_method("RebuildGroundCover")
            flowers = actor.get_editor_property("Flowers")
            for index in range(len(flowers)):
                # 物种下标：0 = 草，1.. = 花（草没网格时花从 0 起）
                species_index = index + (1 if actor.get_editor_property("Grass").get_editor_property("Mesh") else 0)
                log("  %s 花 %d 实例数 = %s" % (actor.get_actor_label(), index,
                                               actor.call_method("DebugReadGroundCoverCountGpuSync", (species_index,))))
        count += 1
    unreal.EditorLoadingAndSavingUtils.save_current_level()
    log("%s -> %d 个地面" % (level, count))

log("FAILED=%d %s" % (len(FAILED), "; ".join(FAILED)))
log("DONE")
