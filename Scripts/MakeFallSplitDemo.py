# -*- coding: utf-8 -*-
"""建 /Game/ClothSplitDemo/L_FallSplitDemo：Quinn 播 Mage_GroundSlam_RM，身体按带状分块碎成布条。

    UnrealEditor-Cmd.exe <工程> -ExecutePythonScript=<本文件> -unattended -RenderOffscreen

Houdini 场景 AnimationFallSplit 的 UE 移植（Docs/HoudiniFallSplit_Port.md）。Houdini 那边的角色与动画本来就是从这个工程
导出去的（geo/Mage_GroundSlam_RM.FBX、UE5_Character.bgeo.sc），所以这里直接用第三人称模板的小白人：
/Game/Characters/Mannequins 下的 SKM_Quinn 与 Mage_GroundSlam_RM。资产属于工程，关卡也就放在工程里，不进插件。

放开时刻按 Houdini 的思路挑：那边条带带着 trail 算的动画速度 × 5 甩出去，地上那一圈条带说明是砸地的下劈段放开的。
这里在动画里采样骨盆高度，取向下最快的那一刻当 ReleaseDelay。条带材质仿 Houdini 渲染（CharVellumOFFix）：
深蓝、偏金属、菲涅尔边缘发光。已有同名关卡就不建，要重建先删掉它；材质已在就沿用。
"""
import math

import unreal

FOLDER = "/Game/ClothSplitDemo"
LEVEL = FOLDER + "/L_FallSplitDemo"
STRIPS = FOLDER + "/M_FallSplitStrips"
TEMPLATE = "/Engine/Maps/Templates/Template_Default"
CHARACTER = "/Game/Characters/Mannequins/Meshes/SKM_Quinn"
ANIMATION = "/Game/Characters/Mannequins/Animations/Quinn/Mage_GroundSlam_RM"
PELVIS = "pelvis"
NEVER = unreal.PropertyAccessChangeNotifyMode.NEVER

ASSETS = unreal.EditorAssetLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()
EDIT = unreal.MaterialEditingLibrary
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
LEVELS = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)

EYE = (420.0, -250.0, 190.0)   # 视口机位（相对地板顶面）
AIM = (0.0, 0.0, 95.0)


def log(text):
    unreal.log("FALLSPLIT " + text)


def look_at(eye, aim):
    """⚠️ unreal.Rotator 的位置参数序是 (roll, pitch, yaw)，只用关键字参数。"""
    dx, dy, dz = aim.x - eye.x, aim.y - eye.y, aim.z - eye.z
    return unreal.Rotator(roll=0.0, pitch=math.degrees(math.atan2(dz, max(math.hypot(dx, dy), 1e-3))),
                          yaw=math.degrees(math.atan2(dy, dx)))


def strips_material():
    if ASSETS.does_asset_exist(STRIPS):
        return ASSETS.load_asset(STRIPS)
    material = TOOLS.create_asset("M_FallSplitStrips", FOLDER, unreal.Material, unreal.MaterialFactoryNew())

    def node(cls, x, y, **properties):
        expression = EDIT.create_material_expression(material, cls, x, y)
        for name, value in properties.items():
            expression.set_editor_property(name, value)
        return expression

    base = node(unreal.MaterialExpressionConstant3Vector, -500, -200, constant=unreal.LinearColor(0.012, 0.014, 0.06, 1.0))
    metallic = node(unreal.MaterialExpressionConstant, -500, 0, r=0.6)
    rough = node(unreal.MaterialExpressionConstant, -500, 100, r=0.25)
    fresnel = node(unreal.MaterialExpressionFresnel, -700, 300, exponent=3.0)
    rim = node(unreal.MaterialExpressionConstant3Vector, -700, 450, constant=unreal.LinearColor(0.3, 0.4, 2.4, 1.0))
    glow = node(unreal.MaterialExpressionMultiply, -500, 300)
    EDIT.connect_material_expressions(fresnel, "", glow, "A")
    EDIT.connect_material_expressions(rim, "", glow, "B")
    EDIT.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    EDIT.connect_material_property(metallic, "", unreal.MaterialProperty.MP_METALLIC)
    EDIT.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    EDIT.connect_material_property(glow, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    EDIT.recompile_material(material)
    ASSETS.save_loaded_asset(material, False)
    return material


def play_length(animation):
    for getter in (lambda: animation.get_play_length(), lambda: unreal.AnimationLibrary.get_sequence_length(animation)):
        try:
            return float(getter())
        except Exception:  # noqa: BLE001 - 引擎版本之间这两个接口有一个就行
            pass
    return 3.0


def slam_release_time(animation):
    """骨盆向下最快的那一刻（秒）。取不到骨骼就退回动画长度的 40%。"""
    length = play_length(animation)
    steps = 120
    heights = []
    for index in range(steps + 1):
        time = length * index / steps
        try:
            pose = unreal.AnimationLibrary.get_bone_pose_for_time(animation, PELVIS, time, False)
        except Exception:  # noqa: BLE001 - 骨骼名不对、接口变了都按退路走
            return length, length * 0.4
        heights.append((time, pose.translation.z))
    speeds = [((a[0] + b[0]) * 0.5, (b[1] - a[1]) / max(b[0] - a[0], 1e-6)) for a, b in zip(heights, heights[1:])]
    fastest = min(speeds, key=lambda item: item[1])
    log("animation {:.2f} s, pelvis falls fastest at {:.2f} s ({:.0f} cm/s)".format(length, fastest[0], fastest[1]))
    return length, fastest[0]


def build():
    if ASSETS.does_asset_exist(LEVEL):
        unreal.log_error("FALLSPLIT {} exists; delete it to make it again".format(LEVEL))
        return
    character = unreal.load_asset(CHARACTER)
    animation = unreal.load_asset(ANIMATION)
    if not character or not animation:
        unreal.log_error("FALLSPLIT needs {} and {} (third person template mannequin)".format(CHARACTER, ANIMATION))
        return
    material = strips_material()
    EDIT.get_statistics(material)   # FinishCompilation：打开就是条带材质而不是默认材质
    length, release = slam_release_time(animation)

    if not LEVELS.new_level_from_template(LEVEL, TEMPLATE):
        unreal.log_error("FALLSPLIT could not create {} from {}".format(LEVEL, TEMPLATE))
        return
    floor_z = 0.0
    for actor in ACTORS.get_all_level_actors():
        if isinstance(actor, unreal.DirectionalLight):
            actor.set_actor_rotation(unreal.Rotator(roll=0.0, pitch=-35.0, yaw=200.0), False)
        elif isinstance(actor, unreal.StaticMeshActor):
            mesh = actor.static_mesh_component.get_editor_property("static_mesh")
            if mesh and mesh.get_name() == "SM_Template_Map_Floor":
                origin, extent = actor.get_actor_bounds(False)
                floor_z = origin.z + extent.z

    split = ACTORS.spawn_actor_from_class(unreal.CSClothSplitActor, unreal.Vector(0.0, 0.0, floor_z), unreal.Rotator(yaw=-90.0))
    split.set_actor_label("FallSplit_Quinn")
    split.get_editor_property("body").set_skeletal_mesh_asset(character)
    split.set_editor_property("animation", animation, notify_mode=NEVER)
    split.set_editor_property("material", material, notify_mode=NEVER)
    # 放开后留足条带飞散、收绳（默认曲线 2.04 s 收完）的时间再从头来。
    split.set_editor_property("loop_period", max(length, release + 4.0), notify_mode=NEVER)
    split.get_editor_property("cloth_component").set_editor_property("release_delay", release, notify_mode=NEVER)
    split.restart()
    cloth = split.get_editor_property("cloth_component")
    log("{} bands, {} pieces, simulating = {}, release at {:.2f} s, loop {:.1f} s".format(
        split.get_num_bands(), split.get_num_pieces(), cloth.is_simulation_ready(), release,
        split.get_editor_property("loop_period")))

    if not LEVELS.save_current_level():
        unreal.log_error("FALLSPLIT could not save " + LEVEL)
        return
    log("saved " + LEVEL)
    eye = unreal.Vector(EYE[0], EYE[1], floor_z + EYE[2])
    aim = unreal.Vector(AIM[0], AIM[1], floor_z + AIM[2])
    unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).set_level_viewport_camera_info(eye, look_at(eye, aim))


build()
