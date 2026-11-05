# -*- coding: utf-8 -*-
"""Simplified versions of the facade modules Night City places most (NightCity_BuildingKit.md section 5), made of boxes
in the plugin, two facade kits from them and a level with every massing the GPU facade can draw.

The sizes are those of the Night City archetypes they stand for (placements over the whole city, Scripts/NightCity/
nc_rank.py); the shapes are boxes, nothing of Night City's meshes is copied.

  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_Wall_W600_H400        600 x 50 x 400   wall_w600_h400 (53k placements)
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_Wall_W300_H400        300 x 50 x 400   wall_w300_h400 (48k)
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_Window_W600_H400      600 x 50 x 400   wall_window_w600_h400 (12k)
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_Window_W300_H400      300 x 50 x 400   wall_window_w300_h400, window_w300_h400
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_GroundShop_W600_H450   600 x 50 x 450   wall_gf_w600_h400 (16k, 450 tall), shop front
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_GroundWall_W600_H450   600 x 50 x 450   the same, shutter down
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_Door_W300_H400        300 x 50 x 400   a wall with a closed 150 x 250 door
                                                                                    (Night City's leaves are separate props)
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_Pillar_H400            50 x 50 x 400   wall_pillar (32k)
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_CornerWrap_H400       350 x 350 x 400  wall_w300_h400_corner_convex (14k)
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_CornerInner_H400      100 x 100 x 400  corner_concave (10k)
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_Cornice_W600          600 x 52 x 60    roof_cornice_w_600 (16k)
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_Parapet_W600_H110     600 x 40 x 110   facade_top_w600 (9k)
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_Roof_W600             600 x 600 x 20   roof_w600 (25k)
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_Roof_W300             300 x 300 x 20   roof_w300, roof_l300_w300 (37k)
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_Band_W600_H200        600 x 90 x 200   wall_w600_h200 (23k)            not in a kit yet
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_Balcony_W300          300 x 70 x 400   balcony_w300 (14k)              not in a kit yet
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_Fin_W600              600 x 108 x 400  shield_a_6m (7k)                not in a kit yet

  DA_NCSimple_Concrete   3 m bays, concrete panels, pillars, cornice, shop fronts on a 450 cm ground storey
  DA_NCSimple_Glass      6 m bays, curtain walls in pairs of columns, wrap-round corners, parapet
  L_NCSimpleShapes       both kits on every footprint (box, L, U, T, courtyard) and stacking (none, setback, podium
                         and tower), one building each

Facade modules have their outside at Y = 0 and their thickness towards -Y, run along +X from 0 and up from Z = 0
(QuarterTurns 0). The outer corner's box starts at the corner and reaches into the building (its outside faces +Y and
-X); the inner corner's box starts at the corner and reaches out into the open square (+X, +Y). Roof tiles span X and Y
from 0 with their tops at Z = 0. Materials are instances of the plugin's M_FacadeKit.

  UnrealEditor-Cmd.exe <project> -ExecutePythonScript=<this file> -unattended -RenderOffscreen

Builds the meshes and kits again in place every run; makes the level only when it does not exist.
"""
import unreal

FOLDER = "/PCGPlugins/CSRoadNet/Houses/NCSimple"
PARENT = "/PCGPlugins/CSRoadNet/Houses/Facade/M_FacadeKit"
LEVEL = FOLDER + "/L_NCSimpleShapes"
TEMPLATE = "/Engine/Maps/Templates/Template_Default"
NEVER = unreal.PropertyAccessChangeNotifyMode.NEVER

ASSETS = unreal.EditorAssetLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()
EDIT = unreal.MaterialEditingLibrary
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
LEVELS = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)

# Part -> (colour, roughness).
PARTS = {
    "Concrete": ((0.52, 0.51, 0.49), 0.9),
    "DarkConcrete": ((0.28, 0.28, 0.29), 0.85),
    "Glass": ((0.03, 0.05, 0.08), 0.08),
    "Frame": ((0.10, 0.10, 0.11), 0.45),
    "Shop": ((0.10, 0.18, 0.22), 0.1),
    "Shutter": ((0.42, 0.42, 0.44), 0.6),
    "Door": ((0.20, 0.22, 0.25), 0.4),
    "Sign": ((0.78, 0.52, 0.16), 0.5),
    "Roof": ((0.20, 0.20, 0.21), 0.95),
}


def log(text):
    unreal.log("NCSIMPLE " + text)


def fail(text):
    unreal.log_error("NCSIMPLE " + text)


def part_materials():
    parent = ASSETS.load_asset(PARENT)
    if not parent:
        fail("no {} (run Scripts/CS2Road/MakeFacadeKit.py first)".format(PARENT))
        return None
    out = {}
    for name, (rgb, roughness) in PARTS.items():
        path = "{}/MI_NC_{}".format(FOLDER, name)
        if ASSETS.does_asset_exist(path):
            made = ASSETS.load_asset(path)
        else:
            made = TOOLS.create_asset("MI_NC_" + name, FOLDER, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
            log("made " + path)
        EDIT.set_material_instance_parent(made, parent)
        wanted = unreal.LinearColor(rgb[0], rgb[1], rgb[2], 1.0)
        # The setters report False even when they took the value: read it back instead.
        EDIT.set_material_instance_vector_parameter_value(made, "Colour", wanted)
        EDIT.set_material_instance_scalar_parameter_value(made, "Roughness", roughness)
        EDIT.update_material_instance(made)
        got = EDIT.get_material_instance_vector_parameter_value(made, "Colour")
        if abs(got.r - wanted.r) > 1e-3 or abs(got.g - wanted.g) > 1e-3 or abs(got.b - wanted.b) > 1e-3:
            fail("{}: Colour reads back {}".format(path, got))
        ASSETS.save_loaded_asset(made, False)
        out[name] = made
    return out


# ---------------------------------------------------------------------------
# Geometry
# ---------------------------------------------------------------------------

def quad(desc, group, corners, normal, axes):
    """A face of its own four vertices (flat normals), wound so that it faces along normal (UE: a triangle
    a, b, c faces away from cross(b - a, c - a))."""
    a, b, c = corners[0], corners[1], corners[2]
    e1 = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
    e2 = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
    cross = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
    if cross[0] * normal[0] + cross[1] * normal[1] + cross[2] * normal[2] > 0.0:
        corners = [corners[0], corners[3], corners[2], corners[1]]
    instances = []
    for p in corners:
        vertex = desc.create_vertex()
        desc.set_vertex_position(vertex, unreal.Vector(p[0], p[1], p[2]))
        instance = desc.create_vertex_instance(vertex)
        desc.set_vertex_instance_uv(instance, unreal.Vector2D(p[axes[0]] / 100.0, p[axes[1]] / 100.0), 0)
        instances.append(instance)
    desc.create_triangle(group, [instances[0], instances[1], instances[2]])
    desc.create_triangle(group, [instances[0], instances[2], instances[3]])


def box(desc, group, lo, hi):
    """The six faces of the box lo..hi (cm)."""
    x0, y0, z0 = lo
    x1, y1, z1 = hi
    quad(desc, group, [(x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1)], (1, 0, 0), (1, 2))
    quad(desc, group, [(x0, y0, z0), (x0, y1, z0), (x0, y1, z1), (x0, y0, z1)], (-1, 0, 0), (1, 2))
    quad(desc, group, [(x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1)], (0, 1, 0), (0, 2))
    quad(desc, group, [(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)], (0, -1, 0), (0, 2))
    quad(desc, group, [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)], (0, 0, 1), (0, 1))
    quad(desc, group, [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0)], (0, 0, -1), (0, 1))


def panel_with_openings(boxes, part, width, bottom, top, depth, openings):
    """A wall panel width wide from Z bottom to top, depth thick (Y from -depth to 0), with rectangular openings
    (x0, x1, z0, z1) cut through it, as boxes: piers between the openings, and the pieces above and below each."""
    x = 0.0
    for x0, x1, z0, z1 in sorted(openings):
        if x0 > x:
            boxes.append((part, (x, -depth, bottom), (x0, 0, top)))
        if z0 > bottom:
            boxes.append((part, (x0, -depth, bottom), (x1, 0, z0)))
        if z1 < top:
            boxes.append((part, (x0, -depth, z1), (x1, 0, top)))
        x = x1
    if x < width:
        boxes.append((part, (x, -depth, bottom), (width, 0, top)))


def glazing(boxes, x0, x1, z0, z1, mullions=0, frame=6.0, recess=18.0, depth=50.0):
    """A recessed pane in the opening x0..x1 by z0..z1: a frame round it and mullions."""
    y0, y1 = -recess - 8.0, -recess
    boxes.append(("Frame", (x0, y0, z0), (x0 + frame, y1, z1)))
    boxes.append(("Frame", (x1 - frame, y0, z0), (x1, y1, z1)))
    boxes.append(("Frame", (x0 + frame, y0, z0), (x1 - frame, y1, z0 + frame)))
    boxes.append(("Frame", (x0 + frame, y0, z1 - frame), (x1 - frame, y1, z1)))
    step = (x1 - x0) / float(mullions + 1)
    for k in range(1, mullions + 1):
        mx = x0 + step * k
        boxes.append(("Frame", (mx - frame * 0.5, y0, z0 + frame), (mx + frame * 0.5, y1, z1 - frame)))
    boxes.append(("Glass", (x0 + frame, y0 + 2.0, z0 + frame), (x1 - frame, y0 + 4.0, z1 - frame)))


def modules():
    m = {}
    # Plain panels: a recessed joint between panels and a slab edge at the floor line.
    w = [("DarkConcrete", (0, -50, 0), (600, 0, 20))]
    w += [("Concrete", (0, -50, 20), (296, 0, 400)), ("Concrete", (304, -50, 20), (600, 0, 400)), ("DarkConcrete", (296, -50, 20), (304, -10, 400))]
    m["Wall_W600_H400"] = w
    m["Wall_W300_H400"] = [("DarkConcrete", (0, -50, 0), (300, 0, 20)), ("Concrete", (0, -50, 20), (300, 0, 400))]
    # Windows: openings with recessed panes.
    win = [("DarkConcrete", (0, -50, 0), (600, 0, 20))]
    panel_with_openings(win, "Concrete", 600, 20, 400, 50, [(40, 560, 100, 340)])
    glazing(win, 40, 560, 100, 340, mullions=3)
    m["Window_W600_H400"] = win
    win3 = [("DarkConcrete", (0, -50, 0), (300, 0, 20))]
    panel_with_openings(win3, "Concrete", 300, 20, 400, 50, [(50, 250, 90, 330)])
    glazing(win3, 50, 250, 90, 330, mullions=1)
    m["Window_W300_H400"] = win3
    # Ground storey, 450 tall: a shop front (kick plate, glass, sign band) and the same with its shutter down.
    shop = [("DarkConcrete", (0, -50, 0), (30, 0, 450)), ("DarkConcrete", (570, -50, 0), (600, 0, 450)),
            ("DarkConcrete", (30, -50, 0), (570, -20, 40)), ("Sign", (30, -50, 340), (570, 0, 390)), ("DarkConcrete", (30, -50, 390), (570, 0, 450))]
    glazing(shop, 30, 570, 40, 340, mullions=2, recess=25.0)
    m["GroundShop_W600_H450"] = shop
    shut = [("DarkConcrete", (0, -50, 0), (30, 0, 450)), ("DarkConcrete", (570, -50, 0), (600, 0, 450)),
            ("Sign", (30, -50, 340), (570, 0, 390)), ("DarkConcrete", (30, -50, 390), (570, 0, 450))]
    shut.append(("Shutter", (30, -30, 0), (570, -24, 340)))
    for k in range(17):
        z = 10 + k * 19.5
        shut.append(("Frame", (30, -24, z), (570, -22, z + 3)))
    m["GroundWall_W600_H450"] = shut
    # A door: a closed 150 x 250 leaf in a frame, the wall round it.
    door = [("DarkConcrete", (0, -50, 0), (75, 0, 20)), ("DarkConcrete", (225, -50, 0), (300, 0, 20))]
    panel_with_openings(door, "Concrete", 300, 20, 400, 50, [(75, 225, 20, 260)])
    door += [("Frame", (75, -36, 0), (83, -20, 260)), ("Frame", (217, -36, 0), (225, -20, 260)), ("Frame", (83, -36, 252), (217, -20, 260))]
    door.append(("Door", (83, -32, 0), (217, -26, 252)))
    door.append(("Frame", (190, -26, 110), (200, -20, 125)))           # handle
    m["Door_W300_H400"] = door
    # Corners. Nothing may stand out past Y = 0 or X = 0: the box's outside is what goes on the outline.
    m["Pillar_H400"] = [("DarkConcrete", (0, -50, 0), (50, 0, 400))]
    wrap = [("Concrete", (0, -50, 20), (350, 0, 400)), ("Concrete", (0, -350, 20), (50, -50, 400)),
            ("Glass", (60, -3, 100), (330, -1, 340)), ("Glass", (1, -330, 100), (3, -60, 340)),
            ("DarkConcrete", (0, -50, 0), (350, 0, 20)), ("DarkConcrete", (0, -350, 0), (50, -50, 20))]
    m["CornerWrap_H400"] = wrap
    m["CornerInner_H400"] = [("DarkConcrete", (0, 0, 0), (100, 12, 400)), ("DarkConcrete", (0, 12, 0), (12, 100, 400))]
    # Tops: the coping flush with the facade, the course under it set back so that it reads as a cornice.
    m["Cornice_W600"] = [("DarkConcrete", (0, -40, 0), (600, -12, 30)), ("DarkConcrete", (0, -52, 30), (600, 0, 60))]
    m["Parapet_W600_H110"] = [("Concrete", (0, -40, 0), (600, -5, 100)), ("DarkConcrete", (0, -45, 100), (600, 0, 110))]
    m["Roof_W600"] = [("Roof", (0, 0, -20), (600, 600, 0))]
    m["Roof_W300"] = [("Roof", (0, 0, -20), (300, 300, 0))]
    # Library pieces the kits do not use yet.
    m["Band_W600_H200"] = [("DarkConcrete", (0, -90, 0), (600, 0, 200))]
    m["Balcony_W300"] = [("Concrete", (0, 0, 0), (300, 70, 20)), ("Frame", (0, 64, 20), (300, 70, 110)), ("Frame", (0, 0, 20), (6, 70, 110)), ("Frame", (294, 0, 20), (300, 70, 110)),
                         ("Concrete", (0, -50, 20), (300, 0, 400))]
    m["Fin_W600"] = [("DarkConcrete", (0, 0, 0), (600, 8, 400)), ("DarkConcrete", (0, 8, 0), (8, 108, 400)), ("DarkConcrete", (592, 8, 0), (600, 108, 400))]
    return m


def build_mesh(name, boxes, materials):
    path = "{}/SM_NC_{}".format(FOLDER, name)
    desc = unreal.StaticMesh.create_static_mesh_description()
    parts = []
    for part, _, _ in boxes:
        if part not in parts:
            parts.append(part)
    groups = {}
    for part in parts:
        group = desc.create_polygon_group()
        desc.set_polygon_group_material_slot_name(group, part)
        groups[part] = group
    for part, lo, hi in boxes:
        if hi[0] - lo[0] <= 0.01 or hi[1] - lo[1] <= 0.01 or hi[2] - lo[2] <= 0.01:
            continue
        box(desc, groups[part], lo, hi)
    mesh = ASSETS.load_asset(path) if ASSETS.does_asset_exist(path) else TOOLS.create_asset("SM_NC_" + name, FOLDER, unreal.StaticMesh, None)
    if not mesh:
        fail("could not make " + path)
        return None
    mesh.set_editor_property("static_materials", [unreal.StaticMaterial(material_interface=materials[part], material_slot_name=part) for part in parts])
    # Nanite before the build; and never a fast build (memory: ue-staticmesh-fastbuild-no-nanite).
    nanite = mesh.get_editor_property("nanite_settings")
    nanite.set_editor_property("enabled", True)
    nanite.set_editor_property("fallback_target", unreal.NaniteFallbackTarget.RELATIVE_ERROR)
    nanite.set_editor_property("fallback_relative_error", 0.0)
    mesh.set_editor_property("nanite_settings", nanite)
    mesh.build_from_static_mesh_descriptions([desc], False, False)
    mesh.set_editor_property("light_map_resolution", 16)
    ASSETS.save_loaded_asset(mesh, False)
    b = mesh.get_bounding_box()
    log("{:28s} {:5d} triangles, box ({:.0f}, {:.0f}, {:.0f}) .. ({:.0f}, {:.0f}, {:.0f}), Nanite {}".format(
        name, desc.get_triangle_count(), b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z, mesh.get_editor_property("nanite_settings").get_editor_property("enabled")))
    return mesh


KITS = {
    "DA_NCSimple_Concrete": {
        "slots": {"wall": "Wall_W300_H400", "window": "Window_W300_H400", "door": "Door_W300_H400", "corner": "Pillar_H400",
                  "parapet": "Cornice_W600", "roof": "Roof_W300", "corner_inner": "CornerInner_H400",
                  "ground_wall": "GroundWall_W600_H450", "ground_window": "GroundShop_W600_H450"},
        "bay_width": 300.0, "ground_storey_height": 450.0, "window_chance": 0.55, "column_group": 1, "ground_window_chance": 0.7,
    },
    "DA_NCSimple_Glass": {
        "slots": {"wall": "Wall_W600_H400", "window": "Window_W600_H400", "door": "Door_W300_H400", "corner": "CornerWrap_H400",
                  "parapet": "Parapet_W600_H110", "roof": "Roof_W600", "corner_inner": "CornerInner_H400",
                  "ground_wall": "GroundWall_W600_H450", "ground_window": "GroundShop_W600_H450"},
        "bay_width": 600.0, "ground_storey_height": 450.0, "window_chance": 0.7, "column_group": 2, "ground_window_chance": 0.8,
    },
}


def make_kits(meshes):
    kits = {}
    for name, spec in KITS.items():
        path = FOLDER + "/" + name
        if ASSETS.does_asset_exist(path):
            kit = ASSETS.load_asset(path)
        else:
            factory = unreal.DataAssetFactory()
            factory.set_editor_property("data_asset_class", unreal.CSCityFacadeKit.static_class())
            kit = TOOLS.create_asset(name, FOLDER, unreal.CSCityFacadeKit, factory)
            log("made " + path)
        if not kit:
            fail("could not make " + path)
            continue
        for slot, key in spec["slots"].items():
            module = unreal.CSCityFacadeModule()
            module.set_editor_property("mesh", meshes[key])
            module.set_editor_property("quarter_turns", 0)
            kit.set_editor_property(slot, module)
        for prop in ("bay_width", "ground_storey_height", "window_chance", "column_group", "ground_window_chance"):
            kit.set_editor_property(prop, spec[prop])
        kit.set_editor_property("storey_height", 400.0)
        ASSETS.save_loaded_asset(kit, False)
        for slot, key in spec["slots"].items():
            if kit.get_editor_property(slot).get_editor_property("mesh") != meshes[key]:
                fail("{}: slot {} reads back wrong".format(path, slot))
        kits[name] = kit
        log("kit " + kit.get_path_name())
    return kits


SHAPES = ["BOX", "L", "U", "T", "COURTYARD"]
STACKS = [("NONE", 16.0), ("SETBACK", 36.0), ("PODIUM_TOWER", 44.0)]


def make_level(kits):
    if ASSETS.does_asset_exist(LEVEL):
        log("{} exists; delete it to make it again".format(LEVEL))
        return
    if not LEVELS.new_level_from_template(LEVEL, TEMPLATE):
        fail("could not create {} from {}".format(LEVEL, TEMPLATE))
        return
    for actor in ACTORS.get_all_level_actors():
        component = actor.get_component_by_class(unreal.StaticMeshComponent) if isinstance(actor, unreal.StaticMeshActor) else None
        mesh = component.get_editor_property("static_mesh") if component else None
        if mesh and mesh.get_name() == "SM_Template_Map_Floor":
            ACTORS.destroy_actor(actor)
    ground = ACTORS.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(8000.0, 13000.0, 0.0))
    ground.set_actor_label("Ground")
    ground.set_actor_scale3d(unreal.Vector(300.0, 400.0, 1.0))
    component = ground.get_component_by_class(unreal.StaticMeshComponent)
    component.set_editor_property("static_mesh", unreal.load_asset("/Engine/BasicShapes/Plane"))
    component.set_material(0, unreal.load_asset("/Engine/BasicShapes/BasicShapeMaterial"))

    building_class = unreal.load_class(None, "/Script/CSRoadNet.CSCityBuildingActor")

    def building(label, kit, centre, size, height, shape, stack):
        actor = ACTORS.spawn_actor_from_class(building_class, unreal.Vector(centre[0], centre[1], 0.0), unreal.Rotator(0.0, 0.0, 0.0))
        actor.set_actor_label(label)
        actor.set_editor_property("height", height, notify_mode=NEVER)
        actor.set_editor_property("kit", kit, notify_mode=NEVER)
        actor.set_editor_property("mass_shape", getattr(unreal.CSCityMassShape, shape), notify_mode=NEVER)
        actor.set_editor_property("mass_stack", getattr(unreal.CSCityMassStack, stack), notify_mode=NEVER)
        outline = actor.get_editor_property("outline")
        outline.clear_spline_points(False)
        half_x, half_y = size[0] * 0.5, size[1] * 0.5
        # The front is out of whichever hull side CSCity::HullBlock settles on (the box is the same for all four).
        for x, y in ((-half_x, -half_y), (half_x, -half_y), (half_x, half_y), (-half_x, half_y)):
            outline.add_spline_point(unreal.Vector(x, y, 0.0), unreal.SplineCoordinateSpace.LOCAL, False)
            outline.set_spline_point_type(outline.get_number_of_spline_points() - 1, unreal.SplinePointType.LINEAR, False)
        outline.set_closed_loop(True, False)
        outline.update_spline()
        return actor

    # Columns: footprints; rows: stackings; the concrete kit's block, then the glass kit's beyond it.
    for k, name in enumerate(("DA_NCSimple_Concrete", "DA_NCSimple_Glass")):
        for r, (stack, height) in enumerate(STACKS):
            for c, shape in enumerate(SHAPES):
                centre = (c * 4000.0, k * 15000.0 + r * 4500.0)
                building("{} {} {}".format(name.split("_")[-1], shape, stack), kits[name], centre, (2400.0, 1800.0), height, shape, stack)
    if not LEVELS.save_current_level():
        fail("could not save " + LEVEL)
        return
    log("saved " + LEVEL)


def run():
    materials = part_materials()
    if not materials:
        return
    meshes = {}
    for name, boxes in modules().items():
        meshes[name] = build_mesh(name, boxes, materials)
        if not meshes[name]:
            return
    kits = make_kits(meshes)
    if len(kits) == len(KITS):
        make_level(kits)


run()
