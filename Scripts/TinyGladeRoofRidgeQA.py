# -*- coding: utf-8 -*-
"""脊长可调 / 双坡 + 山墙（2026-10-02）的实景出图：同一栋房、同三个机位，只换屋脊三件套。

用法（整个编辑器离屏跑，打开演示关卡）::

    UnrealEditor.exe <uproject> /PCGPlugins/HouseTest/L_HouseGroundDemo \\
        -ExecCmds="py <本文件>" -RenderOffscreen -unattended -nosplash -AbsLog=<独立日志>

拍的是 `House_Pillar`（600 × 400）。逐档改 `RoofRidgeLength01` / `RoofRidgeAxis` / `GableRoofStyle`，每档等画面收敛后
导出三张：斜视（看得到山墙那一头）、正对山墙的正视、俯视。最后一档 `handles` 进拉尺寸模式，**真的拖一次脊端箭头**
（挪 actor → `ConsumeDragToHost`，与编辑器 gizmo 走的是同一条路），拖到头变成双坡再出图。
**什么都不存盘**：结束时三件套还原、退出拉尺寸模式，关卡不保存。

产物在 `Saved/RoofRidgeGable_20261002/`：`<档名>-{oblique,front,top}.png`、`report.json`（每档的脊长、是否山墙、
瓦数、尖顶数；`handles` 档另记拖拽前后的脊长）。日志自诊断：最后一行是 `ROOF RIDGE QA COMPLETE` 或带 traceback 的错误。
"""
import json
import math
import traceback
from pathlib import Path

import unreal

PKG = '/PCGPlugins/HouseTest'
OUT = Path(unreal.Paths.project_saved_dir()) / 'RoofRidgeGable_20261002'
OUT.mkdir(parents=True, exist_ok=True)
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

# (档名, RoofRidgeLength01, 脊向, 山墙样式)。-1 = 自动（等坡度四坡，改动之前的样子）。
STEPS = [
    ('auto', -1.0, unreal.CSRoofRidgeAxis.X, unreal.CSGableRoofStyle.STEPPED),
    ('ridge-000', 0.0, unreal.CSRoofRidgeAxis.X, unreal.CSGableRoofStyle.STEPPED),
    ('ridge-060', 0.6, unreal.CSRoofRidgeAxis.X, unreal.CSGableRoofStyle.STEPPED),
    ('ridge-095', 0.95, unreal.CSRoofRidgeAxis.X, unreal.CSGableRoofStyle.STEPPED),
    ('gable-stepped', 1.0, unreal.CSRoofRidgeAxis.X, unreal.CSGableRoofStyle.STEPPED),
    ('gable-overhang', 1.0, unreal.CSRoofRidgeAxis.X, unreal.CSGableRoofStyle.OVERHANG),
    ('gable-stepped-y', 1.0, unreal.CSRoofRidgeAxis.Y, unreal.CSGableRoofStyle.STEPPED),
    # 最后一档：从 0.6 起，在拉尺寸模式里把脊端箭头往外拖到头。
    ('handles', 0.6, unreal.CSRoofRidgeAxis.X, unreal.CSGableRoofStyle.STEPPED),
]
SETTLE = 140          # 每档等这么多帧（Lumen / Nanite 流送 / 实例交接收敛）
STATE = {'ticks': 0, 'rigs': [], 'step': -1, 'next': 10, 'report': {}}


def capture(name, loc, target, fov):
    delta = target - loc
    rot = unreal.Rotator(pitch=math.degrees(math.atan2(delta.z, math.hypot(delta.x, delta.y))),
                         yaw=math.degrees(math.atan2(delta.y, delta.x)), roll=0)
    cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, loc, rot)
    comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
    rt = unreal.RenderingLibrary.create_render_target2d(STATE['world'], 1400, 1000,
                                                        unreal.TextureRenderTargetFormat.RTF_RGBA8)
    for key, value in dict(texture_target=rt, capture_source=unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR,
                           capture_every_frame=False, capture_on_movement=False,
                           always_persist_rendering_state=True, fov_angle=fov).items():
        comp.set_editor_property(key, value)
    pp = unreal.PostProcessSettings()
    pp.set_editor_property('override_dynamic_global_illumination_method', True)
    pp.set_editor_property('dynamic_global_illumination_method', unreal.DynamicGlobalIlluminationMethod.LUMEN)
    comp.set_editor_property('post_process_settings', pp)
    # 树从捕获里摘掉：+X 偏 −Y 那棵的树冠正好罩在山墙前面。⚠️ 只能走 `hide_actor_components`
    # （`hidden_actors` 在默认子对象上改不了，见 `TinyGladeShotSeam.py`）。
    for tree in STATE['trees']:
        comp.hide_actor_components(tree, True)
    STATE['rigs'].append((name, cap, comp, rt))


def setup():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    assert world.get_path_name().startswith(PKG + '/L_HouseGroundDemo.'), world.get_path_name()
    STATE['world'] = world
    everyone = ACTORS.get_all_level_actors()
    houses = {a.get_actor_label(): a for a in everyone if isinstance(a, unreal.CSHouseActor)}
    house = houses['House_Pillar']
    STATE['house'] = house
    STATE['trees'] = [a for a in everyone if a.get_actor_label().startswith('mid_distance_tree')]
    STATE['saved'] = (house.get_editor_property('roof_ridge_length01'), house.get_editor_property('roof_ridge_axis'),
                      house.get_editor_property('gable_roof_style'))

    origin = house.get_actor_location()
    yaw = math.radians(house.get_actor_rotation().yaw)

    def to_world(x, y, z):
        return origin + unreal.Vector(x * math.cos(yaw) - y * math.sin(yaw), x * math.sin(yaw) + y * math.cos(yaw), z)

    wall = house.get_editor_property('wall_height')
    # 机位都在 +X 那一头（1 号边，脊沿 X 时的山墙；−X 那头贴着 House_Road）。
    # 斜视：站在 +X +Y 的斜上方，山墙三角与北坡都看得到。正视：正对那面山墙。俯视：正上方略偏。
    capture('oblique', to_world(1300, 640, wall + 430), to_world(20, 0, wall - 70), 45)
    capture('front', to_world(1500, 0, wall + 130), to_world(0, 0, wall + 10), 40)
    capture('top', to_world(0, -160, wall + 1500), to_world(0, 0, wall), 42)
    unreal.log('ROOF RIDGE QA SETUP house=%s size=%s trees=%d' % (
        house.get_actor_label(), house.get_editor_property('footprint_size'), len(STATE['trees'])))


def describe(house):
    return {
        'ridge_length01': house.get_editor_property('roof_ridge_length01'),
        'ridge_axis': str(house.get_editor_property('roof_ridge_axis')),
        'gable_style': str(house.get_editor_property('gable_roof_style')),
        'ridge_length_cm': house.get_roof_ridge_length(), 'is_gable': house.is_gable_roof(),
        'roof_rise_cm': house.get_roof_rise(), 'tiles': house.get_roof_tile_count(),
        'finials': house.get_roof_finial_count(),
        # 阶梯山墙上沿的露砖 / 全部包边砖 / 整个砖组件（撞没撞容量看最后一个）。
        'gable_bricks': house.get_trim_gable_brick_count(), 'trim_bricks': house.get_trim_brick_count(),
        'frame_bricks': house.get_frame_brick_count(),
    }


def drag_ridge_handle(house, entry):
    """进拉尺寸模式，把 0 号脊端箭头沿脊向往外拖：先 50 cm（脊长应 +100），再一把拖过头（应钳成双坡）。"""
    house.enter_resize_mode()
    handles = house.get_resize_handles()
    ridge = [house.get_ridge_handle(0), house.get_ridge_handle(1)]
    assert ridge[0] and ridge[1], 'ridge handles missing'
    entry['handle_count'] = len(handles)
    before = house.get_roof_ridge_length()

    handle = ridge[0]
    handle.add_actor_world_offset(handle.get_outward_world() * 50.0, False, False)
    applied = handle.consume_drag_to_host(False)
    middle = house.get_roof_ridge_length()
    assert abs(applied - 100.0) < 0.05 and abs(middle - before - 100.0) < 0.05, (before, applied, middle)
    # 另一只箭头跟着走（脊恒居中）：它离房心的距离也多了 50。
    other = ridge[1].get_actor_location() - house.get_actor_location()
    entry['other_handle_local_x'] = other.x

    handle.add_actor_world_offset(handle.get_outward_world() * 400.0, False, False)
    handle.consume_drag_to_host(True)
    assert house.is_gable_roof() and abs(house.get_editor_property('roof_ridge_length01') - 1.0) < 1e-9
    entry.update(drag_before_cm=before, drag_after_50cm=middle, drag_to_end_cm=house.get_roof_ridge_length())

    # 抓手的网格在游戏视图里是藏着的（`SetHiddenInGame(true)`），捕获走的是游戏口径 —— 只为出图掀开。
    for each in house.get_resize_handles():
        for mesh in each.get_components_by_class(unreal.StaticMeshComponent):
            mesh.set_hidden_in_game(False)


def apply(index):
    name, ridge, axis, style = STEPS[index]
    house = STATE['house']
    house.set_editor_property('roof_ridge_length01', ridge)
    house.set_editor_property('roof_ridge_axis', axis)
    house.set_editor_property('gable_roof_style', style)
    house.rebuild_house()
    reason = house.get_roof_tile_undrawable_reason()
    assert not reason, (name, reason)
    entry = {}
    if name == 'handles':
        drag_ridge_handle(house, entry)
    entry.update(describe(house))
    STATE['report'][name] = entry
    unreal.log('ROOF RIDGE QA STEP %s %s' % (name, entry))


def export(index):
    name = STEPS[index][0]
    for rig, cap, comp, rt in STATE['rigs']:
        unreal.RenderingLibrary.export_render_target(STATE['world'], rt, str(OUT), '%s-%s.png' % (name, rig))


def finish():
    house = STATE['house']
    house.exit_resize_mode()
    ridge, axis, style = STATE['saved']
    house.set_editor_property('roof_ridge_length01', ridge)
    house.set_editor_property('roof_ridge_axis', axis)
    house.set_editor_property('gable_roof_style', style)
    house.rebuild_house()
    for rig, cap, comp, rt in STATE['rigs']:
        ACTORS.destroy_actor(cap)
    (OUT / 'report.json').write_text(json.dumps(STATE['report'], indent=2, ensure_ascii=False), encoding='utf-8')
    unreal.log('ROOF RIDGE QA COMPLETE %s' % json.dumps(STATE['report'], ensure_ascii=False))


def tick(dt):
    STATE['ticks'] += 1
    t = STATE['ticks']
    try:
        if t == STATE['next']:
            if STATE['step'] < 0:
                setup()
            else:
                export(STATE['step'])
            STATE['step'] += 1
            if STATE['step'] >= len(STEPS):
                finish()
                unreal.unregister_slate_post_tick_callback(STATE['handle'])
                unreal.SystemLibrary.quit_editor()
                return
            apply(STATE['step'])
            STATE['next'] = t + SETTLE
        elif STATE['step'] >= 0 and t % 2 == 0:
            for rig, cap, comp, rt in STATE['rigs']:
                comp.capture_scene()
    except Exception:
        unreal.log_error(traceback.format_exc())
        unreal.unregister_slate_post_tick_callback(STATE['handle'])
        unreal.SystemLibrary.quit_editor()


STATE['handle'] = unreal.register_slate_post_tick_callback(tick)
