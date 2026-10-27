# -*- coding: utf-8 -*-
"""建 /PCGPlugins/ClothSim/L_ClothDemo：四块 GPU 布料，打开关卡就在编辑器视口里模拟（不进 PIE）。

    UnrealEditor-Cmd.exe <工程> -ExecutePythonScript=<本文件> -unattended -RenderOffscreen

关卡从引擎的 Basic 模板起（太阳、天空、雾、80 m 见方的地板），太阳转到 +X 一侧（从 +X 往回看的布面受光）。
沿 Y 排开四个 ACSClothActor：

  Cloth_Curtain        −X 边钉在帘杆上：从水平摆下来垂成竖帘，自己起褶
  Cloth_Swag           只钉 −X 边的两个角：两角之间垂成弧
  Cloth_Tablecloth     不钉：落到球上、四角往地板垂；球的碰撞体跟着 Ball 走，拖球布当场响应
  Cloth_WindyLaundry   −X 边钉在晾衣杆上：+X 的风加湍流把它吹鼓，一直在飘

杆和立柱挂在各自的布料 actor 下面，拖布料 actor 时钉边连杆一起走、布面靠惯性摆过来。布用 M_ClothDemoFabric
（UV 方格，两种颜色与格数走参数）的四个材质实例，方格看得出褶皱和拉伸。

GPU 数据不进存档：打开关卡后每块布在第一次编辑器 tick 时重新建，从水平的 rest 姿态落下来，所以每次打开都会
看到它们摆下来。编辑器窗口退到后台时布停在那一帧（「Use Less CPU when in Background」），回到前台接着摆。
已有同名关卡就不建，要重建先删掉它；材质已在就沿用。
"""
import math

import unreal

FOLDER = "/PCGPlugins/ClothSim"
LEVEL = FOLDER + "/L_ClothDemo"
FABRIC = FOLDER + "/M_ClothDemoFabric"
TEMPLATE = "/Engine/Maps/Templates/Template_Default"
CYLINDER = "/Engine/BasicShapes/Cylinder"   # 高 100、直径 100，轴沿 Z，原点在中心
SPHERE = "/Engine/BasicShapes/Sphere"       # 直径 100
NEVER = unreal.PropertyAccessChangeNotifyMode.NEVER
KEEP = unreal.AttachmentRule.KEEP_WORLD

ASSETS = unreal.EditorAssetLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()
EDIT = unreal.MaterialEditingLibrary
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
LEVELS = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)

SIZE = 150.0                              # 三块挂着的布的边长（cm）；钉边都在 actor 局部 x = −SIZE/2
TOP = 260.0                               # 钉边离地板顶面的高度
RESOLUTION = 48                           # 每边格数：格距 3.1 cm，与 ACSClothActor 默认的 2 m / 64 格相当
LANES = (-390.0, -130.0, 130.0, 390.0)    # 四块布的 Y
EYE = (900.0, 60.0, 330.0)                # 视口机位（高度相对地板顶面）：从 +X 往 −X 看，四块布都在画面里
AIM = (-40.0, 0.0, 130.0)


def log(text):
    unreal.log("CLOTHDEMO " + text)


def look_at(eye, aim):
    """⚠️ unreal.Rotator 的位置参数序是 (roll, pitch, yaw)，只用关键字参数。"""
    dx, dy, dz = aim.x - eye.x, aim.y - eye.y, aim.z - eye.z
    return unreal.Rotator(roll=0.0, pitch=math.degrees(math.atan2(dz, max(math.hypot(dx, dy), 1e-3))),
                          yaw=math.degrees(math.atan2(dy, dx)))


def fabric_material():
    """UV 方格：Lerp(ColorA, ColorB, floor(u·N) + floor(v·N) 的奇偶)，粗糙度 0.85。"""
    if ASSETS.does_asset_exist(FABRIC):
        return ASSETS.load_asset(FABRIC)
    material = TOOLS.create_asset("M_ClothDemoFabric", FOLDER, unreal.Material, unreal.MaterialFactoryNew())

    def node(cls, x, y, **properties):
        expression = EDIT.create_material_expression(material, cls, x, y)
        for name, value in properties.items():
            expression.set_editor_property(name, value)
        return expression

    def wire(source, target, pin=""):
        if not EDIT.connect_material_expressions(source, "", target, pin):
            raise RuntimeError("could not connect {} to {}.{}".format(source.get_name(), target.get_name(), pin))

    uv = node(unreal.MaterialExpressionTextureCoordinate, -1400, 0)
    checks = node(unreal.MaterialExpressionScalarParameter, -1400, 160, parameter_name="Checks", default_value=8.0)
    scaled = node(unreal.MaterialExpressionMultiply, -1200, 0)
    wire(uv, scaled, "A")
    wire(checks, scaled, "B")
    cell = node(unreal.MaterialExpressionFloor, -1050, 0)
    wire(scaled, cell)
    ones = node(unreal.MaterialExpressionConstant2Vector, -1050, 160, r=1.0, g=1.0)
    total = node(unreal.MaterialExpressionDotProduct, -900, 0)
    wire(cell, total, "A")
    wire(ones, total, "B")
    half = node(unreal.MaterialExpressionMultiply, -750, 0, const_b=0.5)
    wire(total, half, "A")
    parity = node(unreal.MaterialExpressionFrac, -600, 0)     # 偶数格 0、奇数格 0.5
    wire(half, parity)
    alpha = node(unreal.MaterialExpressionMultiply, -450, 0, const_b=2.0)
    wire(parity, alpha, "A")
    color_a = node(unreal.MaterialExpressionVectorParameter, -450, -400, parameter_name="ColorA",
                   default_value=unreal.LinearColor(0.5, 0.5, 0.5, 1.0))
    color_b = node(unreal.MaterialExpressionVectorParameter, -450, -200, parameter_name="ColorB",
                   default_value=unreal.LinearColor(0.8, 0.8, 0.8, 1.0))
    mix = node(unreal.MaterialExpressionLinearInterpolate, -200, 0)
    wire(color_a, mix, "A")
    wire(color_b, mix, "B")
    wire(alpha, mix, "Alpha")
    rough = node(unreal.MaterialExpressionConstant, -200, 200, r=0.85)
    EDIT.connect_material_property(mix, "", unreal.MaterialProperty.MP_BASE_COLOR)
    EDIT.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    EDIT.recompile_material(material)
    ASSETS.save_loaded_asset(material, False)
    return material


def fabric(name, color_a, color_b, checks):
    """M_ClothDemoFabric 的材质实例。只改参数、不带静态开关，共用父材质的 shader。"""
    path = FOLDER + "/" + name
    if ASSETS.does_asset_exist(path):
        return ASSETS.load_asset(path)
    instance = TOOLS.create_asset(name, FOLDER, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    EDIT.set_material_instance_parent(instance, fabric_material())
    EDIT.set_material_instance_vector_parameter_value(instance, "ColorA", unreal.LinearColor(*color_a, 1.0))
    EDIT.set_material_instance_vector_parameter_value(instance, "ColorB", unreal.LinearColor(*color_b, 1.0))
    EDIT.set_material_instance_scalar_parameter_value(instance, "Checks", checks)
    EDIT.update_material_instance(instance)
    ASSETS.save_loaded_asset(instance, False)
    return instance


def prepare_template():
    """太阳转到 +X 一侧，返回模板地板的顶面高度（实测 z = −0.5）。"""
    floor_z = None
    for actor in ACTORS.get_all_level_actors():
        if isinstance(actor, unreal.DirectionalLight):
            # 同 ClothSim.VisualCapture 的太阳：光从 +X 斜上方来，从 +X 往回看的布面受光。
            actor.set_actor_rotation(unreal.Rotator(roll=0.0, pitch=-35.0, yaw=200.0), False)
        elif isinstance(actor, unreal.StaticMeshActor):
            mesh = actor.static_mesh_component.get_editor_property("static_mesh")
            if mesh and mesh.get_name() == "SM_Template_Map_Floor":
                origin, extent = actor.get_actor_bounds(False)
                floor_z = origin.z + extent.z
                log("floor centre ({:.0f}, {:.0f}), half size ({:.0f}, {:.0f}), top z = {:.1f}".format(
                    origin.x, origin.y, extent.x, extent.y, floor_z))
    if floor_z is None:
        log("template floor not found; building on z = 0")
        floor_z = 0.0
    return floor_z


def prop(mesh, label, location, scale, rotation=None, parent=None):
    actor = ACTORS.spawn_actor_from_class(unreal.StaticMeshActor, location, rotation or unreal.Rotator())
    actor.set_actor_label(label)
    actor.set_actor_scale3d(scale)
    component = actor.static_mesh_component
    component.set_editor_property("static_mesh", unreal.load_asset(mesh))
    # 布料 actor 的根组件是可动的：静态组件挂不上去，球也要能拖着走。
    component.set_mobility(unreal.ComponentMobility.MOVABLE)
    if parent:
        actor.attach_to_actor(parent, "", KEEP, KEEP, KEEP, False)
    return actor


def posts(cloth, label, x, ys, floor_z):
    """从地板立到钉边高度的立柱（半径 2 cm）。"""
    for index, y in enumerate(ys):
        prop(CYLINDER, "{}Post{}".format(label, index + 1), unreal.Vector(x, y, floor_z + TOP * 0.5),
             unreal.Vector(0.04, 0.04, TOP / 100.0), None, cloth)


def rod(cloth, label, y, floor_z, span):
    """钉边上沿 Y 的横杆，两端各一根立柱。"""
    x = -SIZE * 0.5
    prop(CYLINDER, label + "Rod", unreal.Vector(x, y, floor_z + TOP), unreal.Vector(0.04, 0.04, span / 100.0),
         unreal.Rotator(roll=90.0, pitch=0.0, yaw=0.0), cloth)
    posts(cloth, label, x, (y - span * 0.5, y + span * 0.5), floor_z)


def collider(kind, center, axis=None, radius=50.0, follow=None):
    shape = unreal.CSClothCollider()
    shape.set_editor_property("type", kind)
    shape.set_editor_property("center", center)
    if axis:
        shape.set_editor_property("axis", axis)
    shape.set_editor_property("radius", radius)
    shape.set_editor_property("follow_actor", follow)
    return shape


def cloth(label, y, z, size, pin, material, **settings):
    """程序化网格布。属性先写好（不通知、不重建），杆和碰撞体都接好以后再统一 rebuild_cloth。"""
    actor = ACTORS.spawn_actor_from_class(unreal.CSClothActor, unreal.Vector(0.0, y, z))
    actor.set_actor_label(label)
    actor.set_editor_property("grid_size", unreal.Vector2D(size, size), notify_mode=NEVER)
    actor.set_editor_property("grid_resolution", unreal.IntPoint(RESOLUTION, RESOLUTION), notify_mode=NEVER)
    actor.set_editor_property("grid_pin", pin, notify_mode=NEVER)
    actor.set_editor_property("material", material, notify_mode=NEVER)
    simulation = actor.get_editor_property("cloth_component")
    for name, value in settings.items():
        simulation.set_editor_property(name, value, notify_mode=NEVER)
    return actor


def build():
    if ASSETS.does_asset_exist(LEVEL):
        unreal.log_error("CLOTHDEMO {} exists; delete it to make it again".format(LEVEL))
        return
    fabrics = {
        "curtain": fabric("MI_ClothDemo_Curtain", (0.32, 0.02, 0.03), (0.20, 0.01, 0.02), 12.0),
        "swag": fabric("MI_ClothDemo_Swag", (0.62, 0.42, 0.06), (0.80, 0.74, 0.58), 6.0),
        "tablecloth": fabric("MI_ClothDemo_Tablecloth", (0.60, 0.04, 0.04), (0.85, 0.85, 0.82), 10.0),
        "laundry": fabric("MI_ClothDemo_Laundry", (0.05, 0.16, 0.48), (0.85, 0.86, 0.88), 8.0),
    }
    # get_statistics 里会 FinishCompilation：等父材质编完再建关卡，打开就是方格而不是默认材质。
    EDIT.get_statistics(fabric_material())

    if not LEVELS.new_level_from_template(LEVEL, TEMPLATE):
        unreal.log_error("CLOTHDEMO could not create {} from {}".format(LEVEL, TEMPLATE))
        return
    floor_z = prepare_template()
    pins = unreal.CSClothGridPin
    shapes = unreal.CSClothColliderType

    curtain = cloth("Cloth_Curtain", LANES[0], floor_z + TOP, SIZE, pins.EDGE_MIN_X, fabrics["curtain"])
    rod(curtain, "Curtain", LANES[0], floor_z, SIZE + 20.0)

    swag = cloth("Cloth_Swag", LANES[1], floor_z + TOP, SIZE, pins.CORNERS_MIN_X, fabrics["swag"])
    posts(swag, "Swag", -SIZE * 0.5, (LANES[1] - SIZE * 0.5, LANES[1] + SIZE * 0.5), floor_z)

    # 球半径 50；碰撞体跟着 Ball 走（局部原点、半径按它的缩放放大），另加一块地板平面接住垂下来的角。
    ball = prop(SPHERE, "Ball", unreal.Vector(0.0, LANES[2], floor_z + 50.0), unreal.Vector(1.0, 1.0, 1.0))
    tablecloth = cloth("Cloth_Tablecloth", LANES[2], floor_z + 130.0, 160.0, pins.NONE, fabrics["tablecloth"],
                       colliders=[collider(shapes.SPHERE, unreal.Vector(0.0, 0.0, 0.0), radius=50.0, follow=ball),
                                  collider(shapes.PLANE, unreal.Vector(0.0, 0.0, floor_z), axis=unreal.Vector(0.0, 0.0, 1.0))],
                       friction=0.5, damping=0.5)

    # 帘子挂下来以后法线沿 X：+X 的风正对着布面吹，把它吹鼓（同 ClothSim.Wind 的设置，再加湍流）。
    laundry = cloth("Cloth_WindyLaundry", LANES[3], floor_z + TOP, SIZE, pins.EDGE_MIN_X, fabrics["laundry"],
                    wind_velocity=unreal.Vector(700.0, 0.0, 0.0), wind_turbulence=400.0,
                    wind_turbulence_scale=150.0, wind_turbulence_speed=0.8)
    rod(laundry, "Laundry", LANES[3], floor_z, SIZE + 20.0)

    for actor in (curtain, swag, tablecloth, laundry):
        actor.rebuild_cloth()
        simulation = actor.get_editor_property("cloth_component")
        log("{}: ready={}, {} particles, {} constraints".format(
            actor.get_actor_label(), simulation.is_simulation_ready(), simulation.get_particle_count(),
            simulation.get_constraint_count()))

    if not LEVELS.save_current_level():
        unreal.log_error("CLOTHDEMO could not save " + LEVEL)
        return
    log("saved " + LEVEL)

    # 交互式编辑器里跑的话顺手把视口摆好；无头（-Cmd）没有关卡视口，这一句什么都不做。
    eye = unreal.Vector(EYE[0], EYE[1], floor_z + EYE[2])
    aim = unreal.Vector(AIM[0], AIM[1], floor_z + AIM[2])
    unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).set_level_viewport_camera_info(eye, look_at(eye, aim))


build()
