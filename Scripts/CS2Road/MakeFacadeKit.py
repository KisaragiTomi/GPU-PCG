# -*- coding: utf-8 -*-
"""Creates the facade kit the CS2 road network and the city buildings put their buildings together from when none is
set (UCSCityFacadeKit::Default, CSCityFacadeKit.h): six modules made of plain boxes, Nanite on, so that their copies
go into GPU-Scene and no building makes or uploads a mesh of its own.

  /PCGPlugins/CSRoadNet/Houses/Facade/SM_Facade_Wall      300 x 25 x 400   plain bay
  /PCGPlugins/CSRoadNet/Houses/Facade/SM_Facade_Window    300 x 25 x 400   bay with a framed, recessed pane
  /PCGPlugins/CSRoadNet/Houses/Facade/SM_Facade_Door      300 x 25 x 400   bay with a door
  /PCGPlugins/CSRoadNet/Houses/Facade/SM_Facade_Corner     40 x 40 x 400   pilaster up each corner
  /PCGPlugins/CSRoadNet/Houses/Facade/SM_Facade_Parapet   300 x 25 x 110   wall round the roof with a coping
  /PCGPlugins/CSRoadNet/Houses/Facade/SM_Facade_Roof      300 x 300 x 20   roof tile
  /PCGPlugins/CSRoadNet/Houses/Facade/M_FacadeKit + MI_Facade_*            one colour per part
  /PCGPlugins/CSRoadNet/Houses/Facade/DA_FacadeKit_Simple                   the kit

Every facade module has its outside at Y = 0 and its thickness towards -Y, runs along +X from 0 and up from Z = 0, so
QuarterTurns is 0 throughout (the kit fits modules by their bounds, so only which side is out matters). The roof tile
spans X and Y from 0 with its top at Z = 0.

  UnrealEditor-Cmd.exe <project> -ExecutePythonScript=<this file> -unattended -RenderOffscreen

Builds the meshes again in place every run (the kit and the levels keep pointing at the same objects).
"""
import unreal

FOLDER = "/PCGPlugins/CSRoadNet/Houses/Facade"
KIT = "DA_FacadeKit_Simple"
PARENT = "M_FacadeKit"

ASSETS = unreal.EditorAssetLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()
EDIT = unreal.MaterialEditingLibrary

# Part -> (colour, roughness).
PARTS = {
    "Plaster": ((0.70, 0.66, 0.58), 0.85),
    "Trim": ((0.36, 0.35, 0.34), 0.7),
    "Glass": ((0.05, 0.08, 0.11), 0.15),
    "Door": ((0.30, 0.18, 0.10), 0.6),
    "Roof": ((0.22, 0.22, 0.24), 0.9),
}


def log(text):
    unreal.log("CS2FACADEKIT " + text)


def fail(text):
    unreal.log_error("CS2FACADEKIT " + text)


# ---------------------------------------------------------------------------
# Materials
# ---------------------------------------------------------------------------

def parent_material():
    path = FOLDER + "/" + PARENT
    if ASSETS.does_asset_exist(path):
        return ASSETS.load_asset(path)
    made = TOOLS.create_asset(PARENT, FOLDER, unreal.Material, unreal.MaterialFactoryNew())
    colour = EDIT.create_material_expression(made, unreal.MaterialExpressionVectorParameter, -400, 0)
    colour.set_editor_property("parameter_name", "Colour")
    colour.set_editor_property("default_value", unreal.LinearColor(0.7, 0.66, 0.58, 1.0))
    EDIT.connect_material_property(colour, "", unreal.MaterialProperty.MP_BASE_COLOR)
    rough = EDIT.create_material_expression(made, unreal.MaterialExpressionScalarParameter, -400, 250)
    rough.set_editor_property("parameter_name", "Roughness")
    rough.set_editor_property("default_value", 0.85)
    EDIT.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    # Without these the instances draw with the default material in a cooked build.
    made.set_editor_property("used_with_instanced_static_meshes", True)
    made.set_editor_property("used_with_nanite", True)
    EDIT.recompile_material(made)
    ASSETS.save_loaded_asset(made, False)
    log("made " + path)
    return made


def part_materials(parent):
    out = {}
    for name, (rgb, roughness) in PARTS.items():
        path = "{}/MI_Facade_{}".format(FOLDER, name)
        if ASSETS.does_asset_exist(path):
            made = ASSETS.load_asset(path)
        else:
            made = TOOLS.create_asset("MI_Facade_" + name, FOLDER, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
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
# Meshes
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


T = 25.0      # wall thickness
H = 400.0     # storey
W = 300.0     # bay


def wall_with_opening(boxes, x0, x1, z0, z1):
    """A bay's wall round the opening x0..x1 by z0..z1."""
    boxes.append(("Plaster", (0, -T, 0), (x0, 0, H)))
    boxes.append(("Plaster", (x1, -T, 0), (W, 0, H)))
    if z0 > 0.0:
        boxes.append(("Plaster", (x0, -T, 0), (x1, 0, z0)))
    boxes.append(("Plaster", (x0, -T, z1), (x1, 0, H)))


def frame(boxes, x0, x1, z0, z1, width, y0, y1, bottom=True):
    """A frame just inside an opening, its depth y0..y1."""
    boxes.append(("Trim", (x0, y0, z0), (x0 + width, y1, z1)))
    boxes.append(("Trim", (x1 - width, y0, z0), (x1, y1, z1)))
    boxes.append(("Trim", (x0 + width, y0, z1 - width), (x1 - width, y1, z1)))
    if bottom:
        boxes.append(("Trim", (x0 + width, y0, z0), (x1 - width, y1, z0 + width)))


def module_boxes():
    out = {}
    # A floor band at the bottom, flush with the face, so that the storeys read.
    out["Wall"] = [("Plaster", (0, -T, 12), (W, 0, H)),
                   ("Trim", (0, -T, 0), (W, 0, 12))]
    window = []
    wall_with_opening(window, 75, 225, 92, 320)
    window.append(("Trim", (75, -T, 92), (225, 0, 100)))           # sill
    frame(window, 75, 225, 100, 320, 8, -18, -4)
    window.append(("Glass", (83, -14, 108), (217, -12, 312)))
    out["Window"] = window
    door = []
    wall_with_opening(door, 90, 210, 0, 270)
    frame(door, 90, 210, 0, 270, 10, -20, -2, bottom=False)
    door.append(("Door", (100, -20, 0), (200, -14, 260)))
    out["Door"] = door
    out["Corner"] = [("Trim", (0, -40, 0), (40, 0, H))]
    out["Parapet"] = [("Plaster", (0, -T, 0), (W, 0, 100)),
                      ("Trim", (0, -T, 100), (W, 0, 110))]           # coping
    out["Roof"] = [("Roof", (0, 0, -20), (W, W, 0))]
    return out


def build_mesh(name, boxes, materials):
    path = "{}/SM_Facade_{}".format(FOLDER, name)
    desc = unreal.StaticMesh.create_static_mesh_description()
    # One polygon group per part, in the order of the mesh's material slots (a group the builder finds no slot
    # name for takes the slot of its index).
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
        box(desc, groups[part], lo, hi)

    mesh = ASSETS.load_asset(path) if ASSETS.does_asset_exist(path) else TOOLS.create_asset("SM_Facade_" + name, FOLDER, unreal.StaticMesh, None)
    if not mesh:
        fail("could not make " + path)
        return None
    mesh.set_editor_property("static_materials", [unreal.StaticMaterial(material_interface=materials[part], material_slot_name=part) for part in parts])
    # Nanite before the build; and never a fast build, which keeps no source geometry to build Nanite from
    # (memory: ue-staticmesh-fastbuild-no-nanite).
    nanite = mesh.get_editor_property("nanite_settings")
    nanite.set_editor_property("enabled", True)
    nanite.set_editor_property("fallback_target", unreal.NaniteFallbackTarget.RELATIVE_ERROR)
    nanite.set_editor_property("fallback_relative_error", 0.0)
    mesh.set_editor_property("nanite_settings", nanite)
    mesh.build_from_static_mesh_descriptions([desc], False, False)
    mesh.set_editor_property("light_map_resolution", 16)
    ASSETS.save_loaded_asset(mesh, False)
    bounds = mesh.get_bounding_box()
    log("{}: {} triangles, {} slots, box {} .. {}, Nanite {}".format(path, desc.get_triangle_count(), mesh.get_num_sections(0), bounds.min, bounds.max,
                                                                   mesh.get_editor_property("nanite_settings").get_editor_property("enabled")))
    return mesh


def make_kit(meshes):
    path = FOLDER + "/" + KIT
    if ASSETS.does_asset_exist(path):
        kit = ASSETS.load_asset(path)
    else:
        factory = unreal.DataAssetFactory()
        factory.set_editor_property("data_asset_class", unreal.CSCityFacadeKit.static_class())
        kit = TOOLS.create_asset(KIT, FOLDER, unreal.CSCityFacadeKit, factory)
        log("made " + path)
    if not kit:
        fail("could not make " + path)
        return None
    for slot in ("Wall", "Window", "Door", "Corner", "Parapet", "Roof"):
        module = unreal.CSCityFacadeModule()
        module.set_editor_property("mesh", meshes[slot])
        module.set_editor_property("quarter_turns", 0)
        kit.set_editor_property(slot.lower(), module)
    kit.set_editor_property("bay_width", W)
    kit.set_editor_property("storey_height", H)
    kit.set_editor_property("window_chance", 0.6)
    ASSETS.save_loaded_asset(kit, False)
    for slot in ("Wall", "Window", "Door", "Corner", "Parapet", "Roof"):
        got = kit.get_editor_property(slot.lower()).get_editor_property("mesh")
        if got != meshes[slot]:
            fail("{}: slot {} reads back {}".format(path, slot, got))
    return kit


def run():
    materials = part_materials(parent_material())
    meshes = {}
    for name, boxes in module_boxes().items():
        meshes[name] = build_mesh(name, boxes, materials)
        if not meshes[name]:
            return
    kit = make_kit(meshes)
    if kit:
        log("kit " + kit.get_path_name())


run()
