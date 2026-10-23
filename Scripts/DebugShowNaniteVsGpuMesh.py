"""
DebugShowNaniteVsGpuMesh.py -- 临时可视化：编辑器世界里哪些图元走 Nanite、哪些走 gpumesh（自研 GPU 网格）。

只读 + 纯临时显示：不 spawn actor、不改组件 / 材质 / 资产属性、不存盘。显示手段两层：
  1. 引擎自带 Nanite Mask（控制台 CVar r.Nanite.Visualize Mask）：逐像素，Nanite = 绿、其它有深度的像素 = 红、天空变黑。
     这是渲染器的真值（含"资产开了 Nanite 但实际退回回退网格"的情况 -> 红）。
  2. 按类别上色的持久化 debug 线框（UKismetSystemLibrary::DrawDebugBox，编辑器世界可见）+ 前景色块（DrawDebugPoint）：
     每个 actor 每个类别画一个包围盒（该类别组件包围盒的并集），色块钉在盒子顶面中心。
     ⚠️ GPU 实例族的包围盒是生产者交接的"容量包围盒"，通常比实例实际分布大（地面上的族 = 整块地面）。

颜色图例（线框 / 色块）：
  绿   Nanite            —— StaticMeshComponent / ISM，网格开了 Nanite、组件没禁、渲染数据里有 Nanite 数据、材质过审
  白   Nanite 回退       —— 资产开了 Nanite，但引擎实际建的是非 Nanite 代理（渲染数据里没有 Nanite 数据 / 材质审计不过 /
                             r.Nanite=0）。判据照抄 FNaniteResourcesHelper::ShouldCreateNaniteProxy 里 Python 看得见的几条
  青   GPU实例 Nanite 路 —— UCSGpuInstancedMeshComponent，BaseMesh 开了 Nanite（IsNaniteRenderPath），
                             实际由替身 UCSGpuInstancedNaniteComponent 交给引擎 Nanite 画
  品红 gpumesh-UCSMesh   —— UCSMeshRenderComponent / URoadMeshComponent（地面、岩壳、房体、柱、藤管……）
  黄   gpumesh-GPU实例经典路 —— UCSGpuInstancedMeshComponent 的 GPU 剔除路（BaseMesh 没开 Nanite / GPU 数据喂的网格）
  橙   普通非 Nanite     —— 资产没开 Nanite / 组件 bDisallowNanite 的静态网格、骨骼网格等

用法（编辑器底部 Cmd 框，或 Output Log 切到 Python）：
  py "<本文件路径>"          show：分类 + 画线框 + 开 Nanite Mask（重复运行 = 刷新）
  py "<本文件路径>" nomask   只画线框，不开 Nanite Mask
  py "<本文件路径>" list     只打印分类表，不画任何东西
  py "<本文件路径>" clear    清除：清持久化 debug 线 + 关 Nanite Mask
  py "<本文件路径>" toggle   切换：Nanite Mask 开着就 clear，关着就 show（快捷键蓝图
                             /PCGPlugins/EditorShortcuts_BPFL/BPFL_NaniteVisualize 的 ToggleNaniteVisualize 调的就是它）
exec 方式（如 MCPUnreal）：先在同一段里写 NANITE_VIZ_MODE = 'clear'，再 exec(open(...).read())。
可选全局：NANITE_VIZ_JSON = r'<路径>'（把分类结果另存 JSON）、NANITE_VIZ_READ_GPU = False（不回读 GPU 实例计数）。

手动清除（不跑脚本）：控制台输入  r.Nanite.Visualize off  ；线框用 Python
  unreal.SystemLibrary.flush_persistent_debug_lines(unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world())
⚠️ flush 清的是编辑器世界里**全部**持久化 debug 线（本关卡里没有别的来源）；切关卡线框也会自然消失。
⚠️ r.Nanite.Visualize 是全局 CVar：所有视图（含 SceneCapture 的彩色输出）都会被染色，用完请 clear。
"""

import json
import os
import sys
import traceback

import unreal

SCRIPT_NAME = 'DebugShowNaniteVsGpuMesh'

# 线框寿命（秒）。Kismet 的 DrawDebugBox 只有"寿命 > 0 才进持久化线批"这一条路；给一个足够大的数 = 直到清除。
LINE_LIFETIME = 1.0e6

CATEGORIES = [
    # key,               显示名,                              线框颜色
    ('nanite',          'Nanite',                            unreal.LinearColor(0.0, 1.0, 0.0, 1.0)),
    ('nanite_fallback', '资产开了 Nanite 但实际回退(非 Nanite)', unreal.LinearColor(1.0, 1.0, 1.0, 1.0)),
    ('gpuinst_nanite',  'GPU实例 Nanite 路(替身)',            unreal.LinearColor(0.0, 1.0, 1.0, 1.0)),
    ('gpumesh_csmesh',  'gpumesh-UCSMesh',                   unreal.LinearColor(1.0, 0.0, 1.0, 1.0)),
    ('gpuinst_classic', 'gpumesh-GPU实例经典路',              unreal.LinearColor(1.0, 1.0, 0.0, 1.0)),
    ('non_nanite',      '普通非 Nanite',                      unreal.LinearColor(1.0, 0.5, 0.0, 1.0)),
]
CAT_NAME = {k: n for k, n, _ in CATEGORIES}
CAT_COLOR = {k: c for k, _, c in CATEGORIES}
COLOR_WORD = {'nanite': '绿', 'nanite_fallback': '白', 'gpuinst_nanite': '青', 'gpumesh_csmesh': '品红',
              'gpuinst_classic': '黄', 'non_nanite': '橙'}

# 已知部件的中文角色提示（按组件名 / 网格名；只用于表格可读性，不参与分类）。
ROLE_BY_COMPONENT_NAME = {
    'TinyGladeMesh': '主体网格',
    'RockShellMesh': '岩壳',
    'PillarMesh': '承重柱',
    'VineTubeMesh': '藤管',
    'FlatRoofParapet': '平顶女儿墙砖',
    'DoorStepBricks': '门前台阶砖',
}
ROLE_BY_MESH_PREFIX = [
    ('SM_TinyGladeRoofTile', '屋面瓦'),
    ('stairs_step', '石阶'),
    ('stairs_pebble', '石阶小石子'),
    ('SM_TG_GrassBlade', '草（近，单根）'),
    ('SM_TG_GrassClump', '草（远，组合）'),
    ('lowpoly_flower', '花'),
    ('garden_flower', '花'),
    ('bush_body', '灌木'),
    ('bush_flowers', '灌木花'),
    ('balcony_door', '门扇'),
    ('mid_distance_tree', '树'),
    ('decorators_window', '窗框'),
    ('setdressing_window_lintel', '窗楣'),
    ('setdressing_window_sill', '窗台'),
    ('window_', '窗玻璃'),
]
# 由 actor 上 VisibleAnywhere 的组件属性直接认出来的角色（Transient 的那些 Python 读不到）。
ROLE_BY_ACTOR_PROPERTY = [
    ('pillar_brick_component', '柱砖'),
    ('flat_roof_parapet_component', '平顶女儿墙砖'),
    ('pillar_mesh_component', '承重柱'),
    ('tiny_glade_mesh_component', '主体网格'),
]
# GPU 数据喂的网格（没有 BaseMesh 资产）按材质名认角色。
ROLE_BY_MATERIAL_SUBSTR = [
    ('IvyBranch', '藤枝'),
    ('IvyLeaf', '藤叶'),
    ('IvyFlower', '藤花'),
]


def _pop_global(name, default=None):
    g = globals()
    return g.pop(name) if name in g else default


def _resolve_mode():
    mode = _pop_global('NANITE_VIZ_MODE')
    if mode:
        return str(mode).strip().lower()
    # `py <file> <arg>`：UE 把参数放进 sys.argv；只在确实是以本文件身份运行时采信（exec 进来时 argv 可能是上一次 py 的残留）。
    argv = list(getattr(sys, 'argv', None) or [])
    this_file = globals().get('__file__')
    if len(argv) > 1 and this_file and os.path.basename(str(argv[0])).lower() == os.path.basename(str(this_file)).lower():
        return str(argv[1]).strip().lower()
    return 'show'


def _script_path():
    this_file = globals().get('__file__')
    if this_file and os.path.basename(str(this_file)).lower() == SCRIPT_NAME.lower() + '.py':
        return os.path.abspath(str(this_file)).replace('\\', '/')
    try:
        return os.path.abspath(os.path.join(unreal.Paths.project_plugins_dir(), 'PCGPlugins', 'Scripts', SCRIPT_NAME + '.py')).replace('\\', '/')
    except Exception:
        return SCRIPT_NAME + '.py'


def _editor_world():
    return unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()


def _dirty_state():
    return (sorted(p.get_name() for p in unreal.EditorLoadingAndSavingUtils.get_dirty_map_packages()),
            sorted(p.get_name() for p in unreal.EditorLoadingAndSavingUtils.get_dirty_content_packages()))


def _cls(name):
    return getattr(unreal, name, None)


def _isinst(obj, cls_name):
    c = _cls(cls_name)
    return c is not None and isinstance(obj, c)


def _prop(obj, name, default=None):
    try:
        return obj.get_editor_property(name)
    except Exception:
        return default


def _mesh_nanite_enabled(sm):
    """UStaticMesh::IsNaniteEnabled 的 Python 可读等价：资产 NaniteSettings.bEnabled 或 r.Nanite.ForceEnableMeshes。"""
    if sm is None:
        return False
    settings = _prop(sm, 'nanite_settings')
    enabled = bool(_prop(settings, 'enabled', False)) if settings is not None else False
    try:
        forced = unreal.SystemLibrary.get_console_variable_int_value('r.Nanite.ForceEnableMeshes') != 0
    except Exception:
        forced = False
    return enabled or forced


_NANITE_TRI_CACHE = {}


def _nanite_triangles(sm):
    """渲染数据里的 Nanite 三角数：UStaticMesh::GetNumNaniteTriangles()（没有有效 Nanite 数据时为 0）。
    Python 没有直接入口，借活对象现算的资产注册表标签 NaniteTriangles（FAssetData(UObject*) 调 GetAssetRegistryTags，只读）。
    读不到返回 None（不据此判回退）。"""
    key = sm.get_path_name()
    if key not in _NANITE_TRI_CACHE:
        value = None
        try:
            tag = unreal.AssetRegistryHelpers.create_asset_data(sm).get_tag_value('NaniteTriangles')
            if tag not in (None, ''):
                value = int(str(tag))
        except Exception:
            value = None
        _NANITE_TRI_CACHE[key] = value
    return _NANITE_TRI_CACHE[key]


def _effective_blend_mode(m):
    """材质实例链上最近一个勾了 override_blend_mode 的取它，否则取母材质的 blend_mode。"""
    cur = m
    while cur is not None and isinstance(cur, unreal.MaterialInstance):
        bpo = _prop(cur, 'base_property_overrides')
        if bpo is not None and _prop(bpo, 'override_blend_mode', False):
            return _prop(bpo, 'blend_mode')
        cur = _prop(cur, 'parent')
    base = m.get_base_material() if m is not None else None
    return _prop(base, 'blend_mode') if base is not None else None


def _nanite_fallback_reason(world, comp, sm, r_nanite):
    """资产开了 Nanite、组件没禁时，引擎仍会建非 Nanite 代理的原因；空串 = 应当真走 Nanite。
    对照 FNaniteResourcesHelper::ShouldCreateNaniteProxy / AuditMaterials（NaniteResourcesHelper.h:112/168）里 Python 看得见的条件：
    r.Nanite、HasValidNaniteData、混合模式只许 Opaque/Masked、SingleLayerWater、天空材质、Masked 需允许、Nanite 用途标记。
    看不见的（bForceDisableNanite、bDisplayNaniteFallbackMesh、平台不支持）以 Nanite Mask 画面为准。"""
    if r_nanite == 0:
        return 'r.Nanite=0'
    tris = _nanite_triangles(sm)
    if tris == 0:
        return '渲染数据里没有 Nanite 数据（NaniteTriangles=0 ⇒ HasValidNaniteData=false）'
    try:
        masking_allowed = unreal.SystemLibrary.get_console_variable_int_value('r.Nanite.AllowMaskedMaterials') != 0
    except Exception:
        masking_allowed = True
    ws = world.get_world_settings()
    ns = _prop(ws, 'nanite_settings') if ws is not None else None
    world_allows = bool(_prop(ns, 'allow_masked_materials', True)) if ns is not None else True
    masking_allowed = masking_allowed and (world_allows or bool(_prop(comp, 'force_nanite_for_masked', False)))
    try:
        n = comp.get_num_materials()
    except Exception:
        n = 0
    for i in range(n):
        try:
            m = comp.get_material(i)
        except Exception:
            m = None
        if m is None:
            continue   # 空槽引擎换默认材质，不算错
        base = m.get_base_material()
        blend = _effective_blend_mode(m)
        blend_s = str(blend)
        if blend is not None and 'OPAQUE' not in blend_s and 'MASKED' not in blend_s:
            return '材质 %s 混合模式 %s 不被 Nanite 支持' % (m.get_name(), blend_s)
        if blend is not None and 'MASKED' in blend_s and not masking_allowed:
            return '材质 %s 是 Masked，但本世界 / CVar 不允许 Nanite Masked' % m.get_name()
        if base is not None:
            if 'SINGLE_LAYER_WATER' in str(_prop(base, 'shading_model', '')):
                return '材质 %s 是 SingleLayerWater' % m.get_name()
            if _prop(base, 'is_sky', False):
                return '材质 %s 是天空材质' % m.get_name()
            if not _prop(base, 'used_with_nanite', True) and not _prop(base, 'automatically_set_usage_in_editor', True):
                return '材质 %s 没勾 Used with Nanite 且不允许编辑器自动补' % m.get_name()
    return ''


def _bounds(comp):
    try:
        o, e, r = unreal.SystemLibrary.get_component_bounds(comp)
        return (o.x - e.x, o.y - e.y, o.z - e.z, o.x + e.x, o.y + e.y, o.z + e.z), r
    except Exception:
        return None, 0.0


def _is_rendering_geometry(comp):
    """只统计画几何的图元：网格类 + 自研 gpumesh 类。辅助图元（sprite/箭头/体积刷/碰撞专用……）不算。"""
    if _isinst(comp, 'CSGroundCollisionComponent'):
        return False
    if _isinst(comp, 'CSGpuMeshComponent') or _isinst(comp, 'MeshComponent'):
        return True
    if _isinst(comp, 'LandscapeComponent'):
        return True
    return False


def _material_hint(comp):
    try:
        m = comp.get_material(0)
        return m.get_name() if m else ''
    except Exception:
        return ''


def _role_hint(actor, comp, mesh_name, prop_roles, material_name):
    role = prop_roles.get(comp.get_name())
    if role:
        return role
    role = ROLE_BY_COMPONENT_NAME.get(comp.get_name())
    if role:
        return role
    is_house = 'House' in actor.get_class().get_name()
    if mesh_name:
        # 房子上除柱砖 / 女儿墙之外那个 brick 族 = FrameComponent（门框砖 + 接缝砖 + 角石 + 包边 + 砖层共用），它是 Transient 读不到，只能排除法。
        if is_house and mesh_name == 'brick' and _isinst(comp, 'CSGpuInstancedMeshComponent'):
            return '门框/接缝/角石砖（推断）'
        for prefix, r in ROLE_BY_MESH_PREFIX:
            if mesh_name.startswith(prefix):
                return r
    if material_name:
        for sub, r in ROLE_BY_MATERIAL_SUBSTR:
            if sub in material_name:
                return r
        if is_house and _isinst(comp, 'CSGpuInstancedMeshComponent'):
            return '摆件（推断）'
    return ''


def classify_world(world, read_gpu_counts=True):
    rows = []
    helpers = 0
    r_nanite = 1
    try:
        r_nanite = unreal.SystemLibrary.get_console_variable_int_value('r.Nanite')
    except Exception:
        pass

    for actor in unreal.GameplayStatics.get_all_actors_of_class(world, unreal.Actor):
        try:
            if actor.is_hidden_ed():
                continue
        except Exception:
            pass
        label = actor.get_actor_label()

        prop_roles = {}
        for pname, role in ROLE_BY_ACTOR_PROPERTY:
            c = _prop(actor, pname)
            if c is not None:
                prop_roles[c.get_name()] = role

        comps = actor.get_components_by_class(unreal.PrimitiveComponent)
        # 替身（UCSGpuInstancedNaniteComponent）挂到本体那一行上，不单独算。
        substitutes = {}
        for c in comps:
            if _isinst(c, 'CSGpuInstancedNaniteComponent') or c.get_class().get_name() == 'CSGpuInstancedNaniteComponent':
                outer = c.get_outer()
                if outer is not None:
                    substitutes[outer.get_path_name()] = c

        for c in comps:
            cls_name = c.get_class().get_name()
            if cls_name == 'CSGpuInstancedNaniteComponent':
                continue
            if not _is_rendering_geometry(c):
                helpers += 1
                continue
            visible = _prop(c, 'visible')
            if visible is None:
                try:
                    visible = c.is_visible()
                except Exception:
                    visible = True
            if not visible or _prop(c, 'is_editor_only', False):
                continue

            row = {
                'actor': label,
                'actor_class': actor.get_class().get_name(),
                'component': c.get_name(),
                'component_class': cls_name,
                'mesh': '',
                'category': None,
                'empty': False,
                'count': None,        # 实例数（GPU 实例族 / ISM）
                'substitute': '',
                'note': '',
                'hidden_in_game': bool(_prop(c, 'hidden_in_game', False)),
            }
            box, radius = _bounds(c)
            row['box'] = box
            material_name = ''

            if _isinst(c, 'CSGpuInstancedMeshComponent'):
                bm = _prop(c, 'base_mesh')
                row['mesh'] = bm.get_name() if bm else '(GPU 数据喂的网格)'
                try:
                    nanite_path = bool(c.is_nanite_render_path())
                except Exception as e:
                    nanite_path = False
                    row['note'] += 'IsNaniteRenderPath 调用失败: %s; ' % e
                row['category'] = 'gpuinst_nanite' if nanite_path else 'gpuinst_classic'
                if read_gpu_counts:
                    try:
                        row['count'] = int(c.debug_read_drawn_instance_count_sync())
                    except Exception:
                        row['count'] = None
                sub = substitutes.get(c.get_path_name())
                if nanite_path:
                    if sub is None:
                        row['note'] += 'Nanite 路但没有替身组件; '
                    else:
                        row['substitute'] = sub.get_name()
                        sbox, srad = _bounds(sub)
                        if srad <= 0.0:
                            row['empty'] = True
                        else:
                            row['box'] = sbox
                    # 替身是 UStaticMeshComponent，照样过引擎的 ShouldCreateNaniteProxy；不过就退成 ISM 代理画回退网格
                    # （CSGpuInstancedNaniteComponent.cpp CreateStaticMeshSceneProxy，会打一次 LogCSGpuInstancedNanite 警告）。
                    reason = _nanite_fallback_reason(world, sub if sub is not None else c, bm, r_nanite) if bm else ''
                    if reason:
                        row['category'] = 'nanite_fallback'
                        row['note'] += '替身退成 ISM 回退代理: %s; ' % reason
                if row['count'] == 0:
                    row['empty'] = True
                if not bm:
                    material_name = _material_hint(c)
                    row['note'] += 'mat=%s; ' % material_name
            elif _isinst(c, 'CSGpuMeshComponent'):
                row['category'] = 'gpumesh_csmesh'
                try:
                    row['empty'] = not bool(c.has_generated_geometry())
                except Exception:
                    pass
            elif _isinst(c, 'StaticMeshComponent'):
                sm = _prop(c, 'static_mesh')
                row['mesh'] = sm.get_name() if sm else ''
                if sm is None:
                    row['empty'] = True
                asset_nanite = _mesh_nanite_enabled(sm)
                disallow = bool(_prop(c, 'disallow_nanite', False))
                if sm is None or not asset_nanite or disallow:
                    row['category'] = 'non_nanite'
                    if sm is not None and disallow and asset_nanite:
                        row['note'] += '资产开了 Nanite，但组件 bDisallowNanite=true; '
                    elif sm is not None:
                        row['note'] += '资产没开 Nanite; '
                else:
                    reason = _nanite_fallback_reason(world, c, sm, r_nanite)
                    row['category'] = 'nanite_fallback' if reason else 'nanite'
                    if reason:
                        row['note'] += reason + '; '
                if _isinst(c, 'InstancedStaticMeshComponent'):
                    try:
                        row['count'] = int(c.get_instance_count())
                        if row['count'] == 0:
                            row['empty'] = True
                    except Exception:
                        pass
            else:
                row['category'] = 'non_nanite'
                row['note'] += '非静态网格图元; '

            row['role'] = _role_hint(actor, c, row['mesh'], prop_roles, material_name)
            rows.append(row)
    return rows, helpers


def _union(a, b):
    if a is None:
        return b
    return (min(a[0], b[0]), min(a[1], b[1]), min(a[2], b[2]), max(a[3], b[3]), max(a[4], b[4]), max(a[5], b[5]))


def draw(world, rows):
    """每个 actor 每个类别画一个并集包围盒 + 盒顶一个前景色块。返回画了几组。"""
    groups = {}
    for r in rows:
        if r['empty'] or r['box'] is None:
            continue
        key = (r['actor'], r['category'])
        groups[key] = _union(groups.get(key), r['box'])

    fg = unreal.DrawDebugSceneDepthPriorityGroup.FOREGROUND if hasattr(unreal, 'DrawDebugSceneDepthPriorityGroup') else None
    for (actor, cat), b in sorted(groups.items()):
        cx, cy, cz = (b[0] + b[3]) * 0.5, (b[1] + b[4]) * 0.5, (b[2] + b[5]) * 0.5
        ex, ey, ez = (b[3] - b[0]) * 0.5, (b[4] - b[1]) * 0.5, (b[5] - b[2]) * 0.5
        # 同一个 actor 的几个类别常常共用一片空间：按类别微缩一点，免得线框完全重合互相盖住。
        shrink = {'gpuinst_nanite': 0.985, 'gpuinst_classic': 0.97}.get(cat, 1.0)
        ex, ey, ez = ex * shrink, ey * shrink, ez * shrink
        # Thickness 是世界单位（DrawTranslucentLine 非屏幕空间）：近景会变成粗条盖住画面，远景又细到断线。
        # 0 = 恒定 1 像素的普通线；辨认靠盒顶那个固定像素大小的前景色块。
        thickness = 0.0
        color = CAT_COLOR[cat]
        unreal.SystemLibrary.draw_debug_box(world, unreal.Vector(cx, cy, cz), unreal.Vector(ex, ey, ez), color,
                                            unreal.Rotator(0.0, 0.0, 0.0), LINE_LIFETIME, thickness)
        top = unreal.Vector(cx, cy, cz + ez)
        if fg is not None:
            unreal.SystemLibrary.draw_debug_point(world, top, 14.0, color, LINE_LIFETIME, fg)
        else:
            unreal.SystemLibrary.draw_debug_point(world, top, 14.0, color, LINE_LIFETIME)
    return len(groups)


def _set_mask(world, on):
    unreal.SystemLibrary.execute_console_command(world, 'r.Nanite.Visualize %s' % ('Mask' if on else 'off'))
    try:
        return unreal.SystemLibrary.get_console_variable_string_value('r.Nanite.Visualize')
    except Exception:
        return '?'


def _invalidate():
    try:
        unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).editor_invalidate_viewports()
    except Exception:
        pass


def _print_table(rows):
    order = [k for k, _, _ in CATEGORIES]
    lines = []
    for cat in order:
        sel = [r for r in rows if r['category'] == cat]
        if not sel:
            continue
        drawn = [r for r in sel if not r['empty']]
        lines.append('== %s（%s）: %d 个在画 / %d 个空 ==' % (CAT_NAME[cat], COLOR_WORD[cat], len(drawn), len(sel) - len(drawn)))
        for r in sorted(sel, key=lambda x: (x['empty'], x['actor'], x['component'])):
            extra = []
            if r['role']:
                extra.append(r['role'])
            if r['count'] is not None:
                extra.append('实例=%d' % r['count'])
            if r['substitute']:
                extra.append('替身=%s' % r['substitute'])
            if r['empty']:
                extra.append('空')
            if r['note']:
                extra.append(r['note'].strip('; '))
            lines.append('  %-30s %-32s %-30s %-30s %s' % (r['actor'], r['component'], r['component_class'], r['mesh'], ' | '.join(extra)))
    print('\n'.join(lines))


def _summary(rows):
    out = {}
    for k, _, _ in CATEGORIES:
        sel = [r for r in rows if r['category'] == k]
        out[k] = {'drawn': sum(1 for r in sel if not r['empty']), 'empty': sum(1 for r in sel if r['empty'])}
    return out


def main():
    mode = _resolve_mode()
    json_path = _pop_global('NANITE_VIZ_JSON')
    read_gpu = _pop_global('NANITE_VIZ_READ_GPU', True)
    result = {'mode': mode}
    try:
        world = _editor_world()
        result['world'] = world.get_path_name()
        before = _dirty_state()
        result['dirty_before'] = {'map': before[0], 'content': before[1]}

        if mode == 'toggle':
            # 以 Nanite Mask 是否开着为准（线框没有可查询的状态）：开着 ⇒ 清除，关着 ⇒ 显示。
            try:
                current = unreal.SystemLibrary.get_console_variable_string_value('r.Nanite.Visualize').strip().lower()
            except Exception:
                current = ''
            mode = 'clear' if current not in ('', 'off', 'none', '0') else 'show'
            result['mode'] = 'toggle->' + mode

        if mode == 'clear':
            unreal.SystemLibrary.flush_persistent_debug_lines(world)
            result['r.Nanite.Visualize'] = _set_mask(world, False)
            _invalidate()
            print('[%s] 已清除：持久化 debug 线已 flush，r.Nanite.Visualize=%s' % (SCRIPT_NAME, result['r.Nanite.Visualize']))
        else:
            rows, helpers = classify_world(world, read_gpu_counts=bool(read_gpu))
            result['helpers_skipped'] = helpers
            result['summary'] = _summary(rows)
            result['rows'] = rows
            _print_table(rows)
            if mode in ('show', 'nomask', 'mask'):
                unreal.SystemLibrary.flush_persistent_debug_lines(world)   # 重复运行 = 刷新，不叠旧框
                result['groups_drawn'] = draw(world, rows)
                if mode != 'nomask':
                    result['r.Nanite.Visualize'] = _set_mask(world, True)
                _invalidate()
            print('[%s] 图例：绿=Nanite  白=资产开了Nanite但实际回退  青=GPU实例Nanite路(替身)  品红=gpumesh-UCSMesh  '
                  '黄=gpumesh-GPU实例经典路  橙=普通非Nanite；Nanite Mask 里 绿=Nanite像素 红=非Nanite像素 天空变黑' % SCRIPT_NAME)
            print('[%s] 清除：py "%s" clear   （或控制台 r.Nanite.Visualize off + flush_persistent_debug_lines）'
                  % (SCRIPT_NAME, _script_path()))

        after = _dirty_state()
        result['dirty_after'] = {'map': after[0], 'content': after[1]}
        grew = sorted(set(after[0] + after[1]) - set(before[0] + before[1]))
        result['newly_dirty'] = grew
        if grew:
            unreal.log_warning('[%s] 本次运行后新增脏包: %s' % (SCRIPT_NAME, grew))
    except Exception:
        result['error'] = traceback.format_exc()
        unreal.log_error('[%s] %s' % (SCRIPT_NAME, result['error']))

    if json_path:
        try:
            with open(json_path, 'w', encoding='utf-8') as f:
                json.dump(result, f, ensure_ascii=False, indent=1)
        except Exception:
            unreal.log_warning('[%s] 写 JSON 失败: %s' % (SCRIPT_NAME, traceback.format_exc()))
    compact = {k: result.get(k) for k in ('mode', 'summary', 'groups_drawn', 'r.Nanite.Visualize', 'dirty_before', 'dirty_after', 'newly_dirty', 'error')}
    print('RESULT ' + json.dumps(compact, ensure_ascii=False))
    return result


main()
