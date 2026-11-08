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

  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_CornerRound_R300_H400  300 x 300 x 400  uf_round_w300_l300 (quarter cylinder, ribbon window)
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_CornerRound_R600_H400  600 x 600 x 400  uf_round_w900_l900, ribbon window
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_CornerRoundSolid_R300_H400            plain quarter cylinder
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_CornerChamfer_W300_H400 300 x 300 x 400 wall_w300_h400_a45 (5k), cut off at 45 degrees
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_RoofRound_R300, _R600, SM_NC_RoofChamfer_W300   ceiling_r300 / corner_round_floor
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_CorniceRound_R300, _R600, SM_NC_CorniceChamfer_W300   facade_top_corner_round
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_CornerInnerRound_R150  150 x 150 x 400  corner_round_concave (a solid fillet)
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_Soffit_W600, _W300     a_bottom_cover (ribbed)
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_Column_W150_H450, _W200  column_a (182 x 230)
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_WindowSmall_W600_H400  punched windows for a heavy transfer storey
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_Plinth_W300_H100       under the walls, stretched down to the ground
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_PlinthPillar_H100, _PlinthWrap_H100, _PlinthRound_R300, _R600, _PlinthChamfer_W300
                                                                     the same under each kind of corner
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_Platform_W300          the floor of the platform in front
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_Stair_W300_D150_H75    a flight of five steps down from its edge

  DA_NCSimple_Concrete   3 m bays, concrete panels, pillars, cornice, shop fronts on a 450 cm ground storey
  DA_NCSimple_Glass      6 m bays, curtain walls in pairs of columns, wrap-round corners, parapet
  DA_NCSimple_Round      3 m bays, round corners a bay in radius with ribbon windows, roof and cornice to match, soft inner
                         corners; columns and soffit for overhangs
  DA_NCSimple_RoundLarge 3 m bays, round corners two bays in radius
  DA_NCSimple_Chamfer    3 m bays, corners cut off at 45 degrees
  DA_NCSimple_Pilotis    6 m bays, heavy concrete with punched windows, upper storeys standing a bay out past the base
                         on 2 m columns over a ribbed soffit
  DA_NCSimple_Styles     3 m bays, pilasters of its own, and chamfered, round and large round corners for the city's field to
                         ask for, corner by corner; columns and soffit for overhangs
  L_NCSimpleShapes       the first two kits on every footprint (box, L, U, T, courtyard) and stacking (none, setback,
                         podium and tower), one building each
  L_NCBuildingTypes      the others: round and cut-off corners on each footprint and stacking, drums, every overhang, a
                         street of overhangs, and a street on a slope (level buildings stepping up beside it); made here,
                         photographed by NCBuildingTypesShots.py
  L_NCSlopeStreets       buildings beside sloping streets, three of them on tilted ground: each stands level at the top of
                         the ground along its front, with a plinth, platform and steps (GroundJoin_Plan.md 4, 11.1);
                         photographed by NCSlopeShots.py
  L_NCSlopeRoad          the same on sloping roads of a network: batch buildings along one, a street hung under the other
  L_NCFieldTest          a street of DA_NCSimple_Styles under the city's field (ACSCityFieldActor), painted a stretch at a time:
                         overhangs, round corners, cut-off corners with overhangs on one side, and nothing;
                         photographed by NCSlopeShots.py
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_Slab_W300              300 x 300 x 50  a slab of ground (sidewalk slabs, 3 and 6 m)
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_RetainWall_W300_H100   300 x 64 x 100  where the ground drops
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_Planter_W300_D150, _PlanterTree_W200   planters, rim at 100 cm (Night City's are about 103)
  /PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_CartShop_W240, _W300   a cart and a stall, their fronts +Y; boxes, no original was found by name
  DA_NCSimple_Ground     the ground from them (UCSCityGroundKit::Default)
  L_NCBlocksHill         four blocks of roads on a hill with the ground instanced over them, and a field saying what the free ground has in
                         each: planters, a night market, half of each with the middle raised 3 m, nothing; photographed by NCSlopeShots.py

Facade modules have their outside at Y = 0 and their thickness towards -Y, run along +X from 0 and up from Z = 0
(QuarterTurns 0). The outer corner's box starts at the corner and reaches into the building (its outside faces +Y and
-X); the inner corner's box starts at the corner and reaches out into the open square (+X, +Y). Roof tiles span X and Y
from 0 with their tops at Z = 0. Materials are instances of the plugin's M_FacadeKit.

A round corner is a quarter cylinder in the box (0, -R) .. (R, 0), from (R, 0), where the wall along +X starts, round to
(0, -R), where the other wall ends, centred on (R, -R); a chamfer is the straight line between the same points; their roof
and cornice pieces lie in the same box.

  UnrealEditor-Cmd.exe <project> -ExecutePythonScript=<this file> -unattended -RenderOffscreen

Makes the meshes that do not exist (delete one to make it again), updates the kits in place, and makes each level only
when it does not exist.
"""
import math

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
    "Paving": ((0.36, 0.36, 0.35), 0.95),
    "Soil": ((0.09, 0.06, 0.04), 1.0),
    "Leaf": ((0.10, 0.24, 0.07), 0.8),
    "Awning": ((0.62, 0.12, 0.08), 0.7),
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


def face(desc, group, corners, normal, axes):
    """A flat polygon of its own vertices (a fan from its first), wound so that it faces along normal."""
    a, b, c = corners[0], corners[1], corners[2]
    e1 = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
    e2 = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
    cross = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
    if cross[0] * normal[0] + cross[1] * normal[1] + cross[2] * normal[2] > 0.0:
        corners = [corners[0]] + list(reversed(corners[1:]))
    instances = []
    for p in corners:
        vertex = desc.create_vertex()
        desc.set_vertex_position(vertex, unreal.Vector(p[0], p[1], p[2]))
        instance = desc.create_vertex_instance(vertex)
        desc.set_vertex_instance_uv(instance, unreal.Vector2D(p[axes[0]] / 100.0, p[axes[1]] / 100.0), 0)
        instances.append(instance)
    for k in range(1, len(instances) - 1):
        desc.create_triangle(group, [instances[0], instances[k], instances[k + 1]])


def faces(part, polygons):
    """An entry of a module's list that is no box: (part, "faces", [(corners, normal, uv axes), ...])."""
    return (part, "faces", polygons)


def arc_path(radius, segments=24):
    """The round corner's line, from (R, 0) round to (0, -R): (x, y, outward normal x, outward normal y)."""
    path = []
    for k in range(segments + 1):
        angle = math.radians(90.0 + 90.0 * k / segments)
        nx, ny = math.cos(angle), math.sin(angle)
        path.append((radius + radius * nx, -radius + radius * ny, nx, ny))
    return path


def chamfer_path(width):
    """The cut-off corner's line, the same ends as the round one's. Its ends' normals are along the box's edges, a root 2
    long, so that a wall's inner face stays parallel to the line and its ends on the box."""
    n = math.sqrt(2.0)
    return [(width, 0.0, 0.0, n), (0.0, -width, -n, 0.0)]


def path_wall(part, path, inset0, inset1, z0, z1, from_at=0.0, to_at=1.0):
    """A wall along the stretch from_at .. to_at of a corner's line, from inset0 to inset1 in from it, z0 to z1 up."""
    last = len(path) - 1
    def at(u):
        t = min(max(u, 0.0), 1.0) * last
        k = min(int(t), last - 1)
        f = t - k
        a, b = path[k], path[k + 1]
        return [a[j] + (b[j] - a[j]) * f for j in range(4)]
    stops = [from_at] + [k / float(last) for k in range(last + 1) if from_at < k / float(last) < to_at] + [to_at]
    points = [at(u) for u in stops]
    polygons = []
    for a, b in zip(points, points[1:]):
        n = ((a[2] + b[2]) * 0.5, (a[3] + b[3]) * 0.5, 0.0)
        ao = (a[0] - a[2] * inset0, a[1] - a[3] * inset0)
        bo = (b[0] - b[2] * inset0, b[1] - b[3] * inset0)
        ai = (a[0] - a[2] * inset1, a[1] - a[3] * inset1)
        bi = (b[0] - b[2] * inset1, b[1] - b[3] * inset1)
        polygons.append(([(ao[0], ao[1], z0), (bo[0], bo[1], z0), (bo[0], bo[1], z1), (ao[0], ao[1], z1)], n, (0, 2)))
        polygons.append(([(ai[0], ai[1], z0), (bi[0], bi[1], z0), (bi[0], bi[1], z1), (ai[0], ai[1], z1)], (-n[0], -n[1], 0.0), (0, 2)))
        polygons.append(([(ao[0], ao[1], z1), (bo[0], bo[1], z1), (bi[0], bi[1], z1), (ai[0], ai[1], z1)], (0, 0, 1), (0, 1)))
        polygons.append(([(ao[0], ao[1], z0), (bo[0], bo[1], z0), (bi[0], bi[1], z0), (ai[0], ai[1], z0)], (0, 0, -1), (0, 1)))
    for p, sign in ((points[0], 1.0), (points[-1], -1.0)):
        o = (p[0] - p[2] * inset0, p[1] - p[3] * inset0)
        i = (p[0] - p[2] * inset1, p[1] - p[3] * inset1)
        # The end's normal runs along the line, away from the wall.
        tangent = (points[1][0] - points[0][0], points[1][1] - points[0][1]) if sign > 0.0 else (points[-1][0] - points[-2][0], points[-1][1] - points[-2][1])
        polygons.append(([(o[0], o[1], z0), (i[0], i[1], z0), (i[0], i[1], z1), (o[0], o[1], z1)], (-sign * tangent[0], -sign * tangent[1], 0.0), (1, 2)))
    return faces(part, polygons)


def path_plate(part, path, centre, z0, z1):
    """A flat piece between a corner's line and the point centre (the box's corner inside the building), z0 to z1 up."""
    outline = [(centre[0], centre[1])] + [(p[0], p[1]) for p in path]
    top = [(x, y, z1) for x, y in outline]
    bottom = [(x, y, z0) for x, y in outline]
    polygons = [(top, (0, 0, 1), (0, 1)), (bottom, (0, 0, -1), (0, 1))]
    for a, b in zip(path, path[1:]):
        polygons.append(([(a[0], a[1], z0), (b[0], b[1], z0), (b[0], b[1], z1), (a[0], a[1], z1)], (a[2] + b[2], a[3] + b[3], 0.0), (0, 2)))
    return faces(part, polygons)


def glazed_corner(path, radius):
    """A corner of the same floors as the panels beside it: a slab edge, a sill and a head, piers at its ends, and a ribbon
    window round it with mullions."""
    out = [path_wall("DarkConcrete", path, 0.0, 50.0, 0.0, 20.0), path_wall("Concrete", path, 0.0, 50.0, 20.0, 100.0), path_wall("Concrete", path, 0.0, 50.0, 330.0, 400.0)]
    pier = 0.1
    out += [path_wall("Concrete", path, 0.0, 50.0, 100.0, 330.0, 0.0, pier), path_wall("Concrete", path, 0.0, 50.0, 100.0, 330.0, 1.0 - pier, 1.0)]
    out.append(path_wall("Glass", path, 20.0, 24.0, 100.0, 330.0, pier, 1.0 - pier))
    mullions = max(2, int(radius / 100.0))
    for k in range(1, mullions):
        u = pier + (1.0 - 2.0 * pier) * k / float(mullions)
        out.append(path_wall("Frame", path, 14.0, 26.0, 100.0, 330.0, u - 0.008, u + 0.008))
    out += [path_wall("Frame", path, 14.0, 26.0, 100.0, 106.0, pier, 1.0 - pier), path_wall("Frame", path, 14.0, 26.0, 324.0, 330.0, pier, 1.0 - pier)]
    return out


def corner_cornice(path):
    """The cornice's section (Cornice_W600) round a corner's line: the course set back, the coping flush."""
    return [path_wall("DarkConcrete", path, 12.0, 40.0, 0.0, 30.0), path_wall("DarkConcrete", path, 0.0, 52.0, 30.0, 60.0)]


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
    # Round and cut-off corners, with the roof and cornice pieces that go with them.
    for radius in (300.0, 600.0):
        tag = "R{:.0f}".format(radius)
        arc = arc_path(radius, 24 if radius < 400.0 else 36)
        m["CornerRound_{}_H400".format(tag)] = glazed_corner(arc, radius)
        m["RoofRound_" + tag] = [path_plate("Roof", arc, (radius, -radius), -20.0, 0.0)]
        m["CorniceRound_" + tag] = corner_cornice(arc)
    arc = arc_path(300.0)
    m["CornerRoundSolid_R300_H400"] = [path_wall("DarkConcrete", arc, 0.0, 50.0, 0.0, 20.0), path_wall("Concrete", arc, 0.0, 50.0, 20.0, 400.0)]
    cut = chamfer_path(300.0)
    m["CornerChamfer_W300_H400"] = glazed_corner(cut, 300.0)
    m["RoofChamfer_W300"] = [path_plate("Roof", cut, (300.0, -300.0), -20.0, 0.0)]
    m["CorniceChamfer_W300"] = corner_cornice(cut)
    # A soft inner corner: a solid fillet in the open square, its surface a quarter circle about (R, R).
    fillet_r = 150.0
    fillet = [(fillet_r - fillet_r * math.cos(math.radians(a)), fillet_r - fillet_r * math.sin(math.radians(a))) for a in (90.0 * k / 16.0 for k in range(17))]
    fillet_polygons = []
    outline = [(0.0, 0.0)] + fillet
    for z, n in ((400.0, 1), (0.0, -1)):
        fillet_polygons.append(([(x, y, z) for x, y in outline], (0, 0, n), (0, 1)))
    for a, b in zip(fillet, fillet[1:]):
        mx, my = (a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5
        fillet_polygons.append(([(a[0], a[1], 0.0), (b[0], b[1], 0.0), (b[0], b[1], 400.0), (a[0], a[1], 400.0)], (fillet_r - mx, fillet_r - my, 0.0), (0, 2)))
    m["CornerInnerRound_R150"] = [faces("Concrete", fillet_polygons)]
    # What is under an overhang: a ribbed soffit and columns.
    for width in (300, 600):
        soffit = [("DarkConcrete", (0, 0, -25), (width, width, 0))]
        for k in range(1, 3):
            y = width * k / 3.0
            soffit.append(("Concrete", (0, y - 20, -75), (width, y + 20, -25)))
        m["Soffit_W{}".format(width)] = soffit
    m["Column_W150_H450"] = [("Concrete", (0, -150, 0), (150, 0, 450))]
    m["Column_W200_H450"] = [("Concrete", (0, -200, 30), (200, 0, 450)), ("DarkConcrete", (0, -200, 0), (200, 0, 30))]
    # A heavy storey with small punched windows.
    small = [("DarkConcrete", (0, -50, 0), (600, 0, 20))]
    panel_with_openings(small, "Concrete", 600, 20, 400, 50, [(180, 420, 150, 300)])
    glazing(small, 180, 420, 150, 300, mullions=1)
    m["WindowSmall_W600_H400"] = small
    # Under the base, where the ground falls away: a plinth stretched down to it, the floor of a platform in front, and flights
    # of steps from that floor's edge, each a storey's riser of five steps, its top at the box's top at -Y.
    m["Plinth_W300_H100"] = [("DarkConcrete", (0, -50, 0), (300, 0, 100))]
    m["PlinthPillar_H100"] = [("DarkConcrete", (0, -50, 0), (50, 0, 100))]
    m["PlinthWrap_H100"] = [("DarkConcrete", (0, -50, 0), (350, 0, 100)), ("DarkConcrete", (0, -350, 0), (50, -50, 100))]
    for radius in (300.0, 600.0):
        m["PlinthRound_R{:.0f}".format(radius)] = [path_wall("DarkConcrete", arc_path(radius, 24 if radius < 400.0 else 36), 0.0, 50.0, 0.0, 100.0)]
    m["PlinthChamfer_W300"] = [path_wall("DarkConcrete", cut, 0.0, 50.0, 0.0, 100.0)]
    m["Platform_W300"] = [("Concrete", (0, 0, -15), (300, 300, 0))]
    m["Stair_W300_D150_H75"] = [("Concrete", (0, k * 30, -75), (300, (k + 1) * 30, -k * 15)) for k in range(5)]
    # The ground: a slab of paving, stretched over its cell, its top at Z = 0, deep enough that its sides hide any step too small for a wall; a
    # retaining wall, its outside at Y = 0, its top at the box's top; and what stands on free ground, its bottom at Z = 0, its front +Y.
    m["Slab_W300"] = [("Paving", (0, 0, -50), (300, 300, 0))]
    m["RetainWall_W300_H100"] = [("Concrete", (0, -60, 0), (300, -4, 90)), ("DarkConcrete", (0, -64, 90), (300, 0, 100))]
    def planter(width, depth, rim=100.0, wall=15.0):
        return [("Concrete", (0, 0, 0), (width, wall, rim)), ("Concrete", (0, depth - wall, 0), (width, depth, rim)),
                ("Concrete", (0, wall, 0), (wall, depth - wall, rim)), ("Concrete", (width - wall, wall, 0), (width, depth - wall, rim)),
                ("Soil", (wall, wall, 0), (width - wall, depth - wall, rim - 12.0))]
    m["Planter_W300_D150"] = planter(300, 150) + [("Leaf", (30, 30, 85), (110, 120, 150)), ("Leaf", (110, 25, 85), (200, 125, 170)), ("Leaf", (200, 30, 85), (270, 120, 145))]
    m["PlanterTree_W200"] = planter(200, 200) + [("Frame", (90, 90, 85), (110, 110, 320)), ("Leaf", (30, 30, 300), (170, 170, 430)), ("Leaf", (55, 55, 430), (145, 145, 500)),
                                                 ("Leaf", (10, 60, 320), (190, 140, 400)), ("Leaf", (60, 10, 320), (140, 190, 400))]
    m["CartShop_W240"] = [("Frame", (30, 35, 0), (60, 115, 30)), ("Frame", (180, 35, 0), (210, 115, 30)), ("Door", (10, 20, 30), (230, 130, 110)),
                          ("Concrete", (5, 15, 110), (235, 135, 118)), ("Sign", (40, 60, 118), (110, 110, 145)), ("Shop", (130, 60, 118), (200, 110, 140)),
                          ("Frame", (12, 20, 118), (20, 28, 230)), ("Frame", (220, 20, 118), (228, 28, 230)), ("Frame", (12, 122, 118), (20, 130, 230)), ("Frame", (220, 122, 118), (228, 130, 230)),
                          ("Awning", (0, 0, 230), (240, 150, 240)), ("Awning", (0, 0, 240), (240, 75, 250)), ("Sign", (40, 60, 250), (200, 72, 285))]
    m["CartShop_W300"] = [("Shutter", (0, 0, 0), (300, 10, 240)), ("Door", (0, 10, 0), (10, 150, 100)), ("Door", (290, 10, 0), (300, 150, 100)), ("Concrete", (10, 120, 0), (290, 150, 100)),
                          ("Frame", (0, 142, 100), (8, 150, 240)), ("Frame", (292, 142, 100), (300, 150, 240)), ("Awning", (0, 0, 240), (300, 150, 250)), ("Awning", (0, 0, 250), (300, 80, 258)),
                          ("Sign", (30, 140, 205), (270, 150, 240)), ("Frame", (20, 12, 150), (280, 30, 156)), ("Leaf", (30, 12, 156), (120, 30, 185)), ("Sign", (140, 12, 156), (260, 30, 180))]
    return m


def build_mesh(name, boxes, materials):
    path = "{}/SM_NC_{}".format(FOLDER, name)
    # Building a mesh again in place trips FRenderResource's check once it is loaded and has render data.
    if ASSETS.does_asset_exist(path):
        return ASSETS.load_asset(path)
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
        if lo == "faces":
            for corners, normal, axes in hi:
                face(desc, groups[part], corners, normal, axes)
            continue
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
                  "ground_wall": "GroundWall_W600_H450", "ground_window": "GroundShop_W600_H450",
                  "plinth": "Plinth_W300_H100", "plinth_corner": "PlinthPillar_H100", "platform": "Platform_W300", "stair": "Stair_W300_D150_H75"},
        "bay_width": 300.0, "ground_storey_height": 450.0, "window_chance": 0.55, "column_group": 1, "ground_window_chance": 0.7,
    },
    "DA_NCSimple_Glass": {
        "slots": {"wall": "Wall_W600_H400", "window": "Window_W600_H400", "door": "Door_W300_H400", "corner": "CornerWrap_H400",
                  "parapet": "Parapet_W600_H110", "roof": "Roof_W600", "corner_inner": "CornerInner_H400",
                  "ground_wall": "GroundWall_W600_H450", "ground_window": "GroundShop_W600_H450",
                  "plinth": "Plinth_W300_H100", "plinth_corner": "PlinthWrap_H100", "platform": "Platform_W300", "stair": "Stair_W300_D150_H75"},
        "bay_width": 600.0, "ground_storey_height": 450.0, "window_chance": 0.7, "column_group": 2, "ground_window_chance": 0.8,
        "stair_every": 2,
    },
    "DA_NCSimple_Round": {
        "slots": {"wall": "Wall_W300_H400", "window": "Window_W300_H400", "door": "Door_W300_H400", "corner": "CornerRound_R300_H400",
                  "parapet": "Cornice_W600", "roof": "Roof_W300", "corner_inner": "CornerInnerRound_R150",
                  "ground_wall": "GroundWall_W600_H450", "ground_window": "GroundShop_W600_H450",
                  "roof_corner": "RoofRound_R300", "parapet_corner": "CorniceRound_R300", "soffit": "Soffit_W300", "column": "Column_W150_H450",
                  "plinth": "Plinth_W300_H100", "plinth_corner": "PlinthRound_R300", "platform": "Platform_W300", "stair": "Stair_W300_D150_H75"},
        "bay_width": 300.0, "ground_storey_height": 450.0, "window_chance": 0.55, "column_group": 1, "ground_window_chance": 0.7,
        "column_every": 2,
    },
    "DA_NCSimple_RoundLarge": {
        "slots": {"wall": "Wall_W300_H400", "window": "Window_W300_H400", "door": "Door_W300_H400", "corner": "CornerRound_R600_H400",
                  "parapet": "Cornice_W600", "roof": "Roof_W300", "corner_inner": "CornerInnerRound_R150",
                  "ground_wall": "GroundWall_W600_H450", "ground_window": "GroundShop_W600_H450",
                  "roof_corner": "RoofRound_R600", "parapet_corner": "CorniceRound_R600",
                  "plinth": "Plinth_W300_H100", "plinth_corner": "PlinthRound_R600", "platform": "Platform_W300", "stair": "Stair_W300_D150_H75"},
        "bay_width": 300.0, "ground_storey_height": 450.0, "window_chance": 0.55, "column_group": 1, "ground_window_chance": 0.7,
    },
    "DA_NCSimple_Chamfer": {
        "slots": {"wall": "Wall_W300_H400", "window": "Window_W300_H400", "door": "Door_W300_H400", "corner": "CornerChamfer_W300_H400",
                  "parapet": "Cornice_W600", "roof": "Roof_W300", "corner_inner": "CornerInner_H400",
                  "ground_wall": "GroundWall_W600_H450", "ground_window": "GroundShop_W600_H450",
                  "roof_corner": "RoofChamfer_W300", "parapet_corner": "CorniceChamfer_W300",
                  "plinth": "Plinth_W300_H100", "plinth_corner": "PlinthChamfer_W300", "platform": "Platform_W300", "stair": "Stair_W300_D150_H75"},
        "bay_width": 300.0, "ground_storey_height": 450.0, "window_chance": 0.55, "column_group": 1, "ground_window_chance": 0.7,
    },
    "DA_NCSimple_Pilotis": {
        "slots": {"wall": "Wall_W600_H400", "window": "WindowSmall_W600_H400", "door": "Door_W300_H400", "corner": "Pillar_H400",
                  "parapet": "Cornice_W600", "roof": "Roof_W600", "corner_inner": "CornerInner_H400",
                  "ground_wall": "GroundWall_W600_H450", "ground_window": "GroundShop_W600_H450",
                  "soffit": "Soffit_W600", "column": "Column_W200_H450",
                  "plinth": "Plinth_W300_H100", "plinth_corner": "PlinthPillar_H100", "platform": "Platform_W300", "stair": "Stair_W300_D150_H75"},
        "bay_width": 600.0, "ground_storey_height": 450.0, "window_chance": 0.55, "column_group": 2, "ground_window_chance": 0.8,
        "column_every": 2, "column_inset": 40.0, "overhang_bays": 1, "stair_every": 2,
    },
    "DA_NCSimple_Styles": {
        "slots": {"wall": "Wall_W300_H400", "window": "Window_W300_H400", "door": "Door_W300_H400", "corner": "Pillar_H400",
                  "parapet": "Cornice_W600", "roof": "Roof_W300", "corner_inner": "CornerInner_H400",
                  "ground_wall": "GroundWall_W600_H450", "ground_window": "GroundShop_W600_H450", "soffit": "Soffit_W300", "column": "Column_W150_H450",
                  "plinth": "Plinth_W300_H100", "plinth_corner": "PlinthPillar_H100", "platform": "Platform_W300", "stair": "Stair_W300_D150_H75"},
        "styles": {
            "chamfer_corners": {"corner": "CornerChamfer_W300_H400", "roof_corner": "RoofChamfer_W300", "parapet_corner": "CorniceChamfer_W300", "plinth_corner": "PlinthChamfer_W300"},
            "round_corners": {"corner": "CornerRound_R300_H400", "roof_corner": "RoofRound_R300", "parapet_corner": "CorniceRound_R300", "plinth_corner": "PlinthRound_R300"},
            "round_large_corners": {"corner": "CornerRound_R600_H400", "roof_corner": "RoofRound_R600", "parapet_corner": "CorniceRound_R600", "plinth_corner": "PlinthRound_R600"},
        },
        "bay_width": 300.0, "ground_storey_height": 450.0, "window_chance": 0.55, "column_group": 1, "ground_window_chance": 0.7,
        "column_every": 2,
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
        for prop, pieces in spec.get("styles", {}).items():
            pieces_set = unreal.CSCityCornerSet()
            for piece, key in pieces.items():
                module = unreal.CSCityFacadeModule()
                module.set_editor_property("mesh", meshes[key])
                module.set_editor_property("quarter_turns", 0)
                pieces_set.set_editor_property(piece, module)
            kit.set_editor_property(prop, pieces_set)
        for prop in ("bay_width", "ground_storey_height", "window_chance", "column_group", "ground_window_chance"):
            kit.set_editor_property(prop, spec[prop])
        for prop in ("column_every", "column_inset", "overhang_bays", "stair_every"):
            if prop in spec:
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


# L_NCBuildingTypes, row by row: (kit, [(label, width, depth, height, shape, stack, overhang)]), buildings 50 m apart along
# X, rows 60 m apart along Y.
TYPES = (
    ("DA_NCSimple_Round", [
        ("Round box", 24, 18, 16, "BOX", "NONE", "AUTO"), ("Round L", 24, 18, 16, "L", "NONE", "AUTO"), ("Round U", 24, 18, 16, "U", "NONE", "AUTO"),
        ("Round courtyard", 27, 27, 16, "COURTYARD", "NONE", "AUTO"), ("Round setback", 24, 18, 36, "BOX", "SETBACK", "AUTO"), ("Round tower", 24, 24, 44, "BOX", "PODIUM_TOWER", "AUTO")]),
    ("DA_NCSimple_RoundLarge", [
        ("Large box", 24, 18, 16, "BOX", "NONE", "AUTO"), ("Large capsule", 24, 12, 28, "BOX", "NONE", "AUTO"), ("Large drum", 12, 12, 40, "BOX", "NONE", "AUTO"),
        ("Large L", 30, 24, 16, "L", "NONE", "AUTO"), ("Large setback", 30, 24, 36, "BOX", "SETBACK", "AUTO"), ("Large tower", 30, 30, 44, "BOX", "PODIUM_TOWER", "AUTO")]),
    ("DA_NCSimple_Chamfer", [
        ("Chamfer box", 24, 18, 16, "BOX", "NONE", "AUTO"), ("Chamfer L", 24, 18, 16, "L", "NONE", "AUTO"), ("Chamfer T", 24, 18, 16, "T", "NONE", "AUTO"),
        ("Chamfer courtyard", 27, 27, 16, "COURTYARD", "NONE", "AUTO"), ("Chamfer setback", 24, 18, 36, "BOX", "SETBACK", "AUTO"), ("Chamfer tower", 24, 24, 44, "BOX", "PODIUM_TOWER", "AUTO")]),
    ("DA_NCSimple_Pilotis", [
        ("Flat front", 36, 24, 24, "BOX", "OVERHANG", "FRONT"), ("Flat sides", 36, 24, 24, "BOX", "OVERHANG", "SIDES"), ("Flat front sides", 36, 24, 24, "BOX", "OVERHANG", "FRONT_AND_SIDES"),
        ("Flat all round", 36, 24, 24, "BOX", "OVERHANG", "ALL_ROUND"), ("Flat tall", 36, 24, 60, "BOX", "OVERHANG", "FRONT_AND_SIDES"), ("Flat L", 36, 24, 24, "L", "OVERHANG", "FRONT")]),
    ("DA_NCSimple_Round", [
        ("Round flat front", 24, 18, 20, "BOX", "OVERHANG", "FRONT"), ("Round flat sides", 24, 18, 20, "BOX", "OVERHANG", "SIDES"), ("Round flat front sides", 24, 18, 20, "BOX", "OVERHANG", "FRONT_AND_SIDES"),
        ("Round flat all round", 24, 18, 20, "BOX", "OVERHANG", "ALL_ROUND")]),
    # A big block: ten bays of 6 m, a street front on columns, its side walls out past the base as well.
    ("DA_NCSimple_Pilotis", [("Plaza flat", 60, 36, 28, "BOX", "OVERHANG", "FRONT_AND_SIDES")]),
)
TYPES_ROW = 6000.0
TYPES_COLUMN = 5000.0


def make_types_level(kits):
    path = FOLDER + "/L_NCBuildingTypes"
    if ASSETS.does_asset_exist(path):
        log("{} exists; delete it to make it again".format(path))
        return
    if not LEVELS.new_level_from_template(path, TEMPLATE):
        fail("could not create {} from {}".format(path, TEMPLATE))
        return
    for actor in ACTORS.get_all_level_actors():
        component = actor.get_component_by_class(unreal.StaticMeshComponent) if isinstance(actor, unreal.StaticMeshActor) else None
        mesh = component.get_editor_property("static_mesh") if component else None
        if mesh and mesh.get_name() == "SM_Template_Map_Floor":
            ACTORS.destroy_actor(actor)
    ground = ACTORS.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(12500.0, 20000.0, 0.0))
    ground.set_actor_label("Ground")
    ground.set_actor_scale3d(unreal.Vector(500.0, 700.0, 1.0))
    component = ground.get_component_by_class(unreal.StaticMeshComponent)
    component.set_editor_property("static_mesh", unreal.load_asset("/Engine/BasicShapes/Plane"))
    component.set_material(0, unreal.load_asset("/Engine/BasicShapes/BasicShapeMaterial"))
    building_class = unreal.load_class(None, "/Script/CSRoadNet.CSCityBuildingActor")
    for r, (kit_name, row) in enumerate(TYPES):
        for c, (label, width, depth, height, shape, stack, overhang) in enumerate(row):
            actor = ACTORS.spawn_actor_from_class(building_class, unreal.Vector(c * TYPES_COLUMN, r * TYPES_ROW, 0.0), unreal.Rotator(0.0, 0.0, 0.0))
            actor.set_actor_label(label)
            actor.set_editor_property("height", float(height), notify_mode=NEVER)
            actor.set_editor_property("kit", kits[kit_name], notify_mode=NEVER)
            actor.set_editor_property("mass_shape", getattr(unreal.CSCityMassShape, shape), notify_mode=NEVER)
            actor.set_editor_property("mass_stack", getattr(unreal.CSCityMassStack, stack), notify_mode=NEVER)
            actor.set_editor_property("mass_overhang", getattr(unreal.CSCityMassOverhang, overhang), notify_mode=NEVER)
            outline = actor.get_editor_property("outline")
            outline.clear_spline_points(False)
            half_x, half_y = width * 50.0, depth * 50.0
            for x, y in ((-half_x, -half_y), (half_x, -half_y), (half_x, half_y), (-half_x, half_y)):
                outline.add_spline_point(unreal.Vector(x, y, 0.0), unreal.SplineCoordinateSpace.LOCAL, False)
                outline.set_spline_point_type(outline.get_number_of_spline_points() - 1, unreal.SplinePointType.LINEAR, False)
            outline.set_closed_loop(True, False)
            outline.update_spline()
    # A street of overhangs.
    y = len(TYPES) * TYPES_ROW
    street = ACTORS.spawn_actor_from_class(unreal.CSCityStreetActor, unreal.Vector(0.0, y, 0.0))
    street.set_actor_label("Street of overhangs")
    street.set_editor_property("fabric", unreal.CSCityFabric.STREET_WALL, notify_mode=unreal.PropertyAccessChangeNotifyMode.ALWAYS)
    street.set_editor_property("kit", kits["DA_NCSimple_Pilotis"], notify_mode=NEVER)
    street.set_editor_property("frontage", 30.0, notify_mode=NEVER)
    street.set_editor_property("min_depth", 24.0, notify_mode=NEVER)
    street.set_editor_property("max_depth", 30.0, notify_mode=NEVER)
    street.set_editor_property("overhang_chance", 0.8, notify_mode=NEVER)
    street.set_editor_property("podium_chance", 0.0, notify_mode=NEVER)
    street.set_editor_property("street_width", 16.0, notify_mode=NEVER)
    line = street.get_editor_property("street")
    line.clear_spline_points(False)
    for x in (0.0, 15000.0):
        line.add_spline_point(unreal.Vector(x, y, 0.0), unreal.SplineCoordinateSpace.WORLD, False)
    line.update_spline()
    # A street rising 6 m over its 150 m on a tilted slab, and its buildings: each one stands level, a step up from the last.
    y = (len(TYPES) + 1) * TYPES_ROW
    slab = ACTORS.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(7500.0, y, 300.0), unreal.Rotator(roll=0.0, pitch=math.degrees(math.atan2(600.0, 15000.0)), yaw=0.0))
    slab.set_actor_label("Sloping ground")
    slab.set_actor_scale3d(unreal.Vector(160.0, 100.0, 1.0))
    component = slab.get_component_by_class(unreal.StaticMeshComponent)
    component.set_editor_property("static_mesh", unreal.load_asset("/Engine/BasicShapes/Plane"))
    component.set_material(0, unreal.load_asset("/Engine/BasicShapes/BasicShapeMaterial"))
    climb = ACTORS.spawn_actor_from_class(unreal.CSCityStreetActor, unreal.Vector(0.0, y, 0.0))
    climb.set_actor_label("Street on a slope")
    climb.set_editor_property("fabric", unreal.CSCityFabric.MID_MIXED, notify_mode=unreal.PropertyAccessChangeNotifyMode.ALWAYS)
    climb.set_editor_property("kit", kits["DA_NCSimple_Round"], notify_mode=NEVER)
    climb.set_editor_property("vacancy", 0.0, notify_mode=NEVER)
    climb.set_editor_property("street_width", 16.0, notify_mode=NEVER)
    line = climb.get_editor_property("street")
    line.clear_spline_points(False)
    for x, z in ((0.0, 0.0), (15000.0, 600.0)):
        line.add_spline_point(unreal.Vector(x, y, z), unreal.SplineCoordinateSpace.WORLD, False)
    line.update_spline()
    if not LEVELS.save_current_level():
        fail("could not save " + path)
        return
    log("saved " + path)


def new_level(path):
    """An empty level at path, its floor gone; None when it exists."""
    if ASSETS.does_asset_exist(path):
        log("{} exists; delete it to make it again".format(path))
        return False
    if not LEVELS.new_level_from_template(path, TEMPLATE):
        fail("could not create {} from {}".format(path, TEMPLATE))
        return False
    for actor in ACTORS.get_all_level_actors():
        component = actor.get_component_by_class(unreal.StaticMeshComponent) if isinstance(actor, unreal.StaticMeshActor) else None
        mesh = component.get_editor_property("static_mesh") if component else None
        if mesh and mesh.get_name() == "SM_Template_Map_Floor":
            ACTORS.destroy_actor(actor)
    return True


def sloping_ground(label, y, length, rise, width=120.0, below=0.0):
    """Ground rising rise (cm) over length (cm) along +X from (0, y, -below), width metres across."""
    slab = ACTORS.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(length * 0.5, y, rise * 0.5 - below), unreal.Rotator(roll=0.0, pitch=math.degrees(math.atan2(rise, length)), yaw=0.0))
    slab.set_actor_label(label)
    # Past both ends of the street, along the slope.
    slab.set_actor_scale3d(unreal.Vector((length + 6000.0) * 0.01, width, 1.0))
    component = slab.get_component_by_class(unreal.StaticMeshComponent)
    component.set_editor_property("static_mesh", unreal.load_asset("/Engine/BasicShapes/Plane"))
    component.set_material(0, unreal.load_asset("/Engine/BasicShapes/BasicShapeMaterial"))
    return slab


# L_NCSlopeStreets: (kit, y (m), rise (m) over 120 m, frontage (m), setback (m), overhang chance).
SLOPE_LENGTH = 12000.0
SLOPE_STREETS = (
    ("Round 4%", "DA_NCSimple_Round", 0.0, 4.8, 24.0, 4.0, 0.0),
    ("Pilotis 4%", "DA_NCSimple_Pilotis", 130.0, 4.8, 36.0, 4.0, 0.8),
    ("Chamfer 8%", "DA_NCSimple_Chamfer", 260.0, 9.6, 24.0, 6.0, 0.0),
)
# L_NCSlopeRoad: two parallel roads rising 6 m over 120 m, 100 m apart.
SLOPE_ROADS = (("Batch buildings", 0.0), ("Street under a road", 13000.0))


def make_slope_streets(kits):
    path = FOLDER + "/L_NCSlopeStreets"
    if not new_level(path):
        return
    for label, kit_name, y, rise, frontage, setback, overhang in SLOPE_STREETS:
        sloping_ground("Ground " + label, y * 100.0, SLOPE_LENGTH, rise * 100.0)
        street = ACTORS.spawn_actor_from_class(unreal.CSCityStreetActor, unreal.Vector(0.0, y * 100.0, 0.0))
        street.set_actor_label(label)
        street.set_editor_property("fabric", unreal.CSCityFabric.MID_MIXED, notify_mode=unreal.PropertyAccessChangeNotifyMode.ALWAYS)
        for prop, value in (("kit", kits[kit_name]), ("vacancy", 0.0), ("street_width", 16.0), ("frontage", frontage), ("gap", 0.0), ("setback", setback),
                            ("min_depth", 18.0), ("max_depth", 24.0), ("overhang_chance", overhang), ("podium_chance", 0.0)):
            street.set_editor_property(prop, value, notify_mode=NEVER)
        line = street.get_editor_property("street")
        line.clear_spline_points(False)
        for x, z in ((0.0, 0.0), (SLOPE_LENGTH, rise * 100.0)):
            line.add_spline_point(unreal.Vector(x, y * 100.0, z), unreal.SplineCoordinateSpace.WORLD, False)
        line.update_spline()
    if not LEVELS.save_current_level():
        fail("could not save " + path)
        return
    log("saved " + path)


def make_slope_road(kits):
    path = FOLDER + "/L_NCSlopeRoad"
    if not new_level(path):
        return
    rise = 600.0
    never = NEVER
    keep = unreal.AttachmentRule.KEEP_WORLD
    network = ACTORS.spawn_actor_from_class(unreal.load_class(None, "/Script/CSRoadNet.CSRoadNetworkActor"), unreal.Vector(0.0, 0.0, 0.0))
    network.set_actor_label("Roads on a slope")
    network.set_editor_property("local_update", True, notify_mode=never)
    network.set_editor_property("tile_size", 64.0, notify_mode=never)
    network.set_editor_property("batch_buildings", True, notify_mode=never)
    network.set_editor_property("building_kit", kits["DA_NCSimple_Concrete"], notify_mode=never)
    road_class = unreal.load_class(None, "/Script/CSRoadNet.CSRoadSplineActor")
    for label, y in SLOPE_ROADS:
        # 25 cm under the roads, as in L_RoadTierMini.
        sloping_ground("Ground " + label, y, SLOPE_LENGTH, rise, below=25.0)
        road = ACTORS.spawn_actor_from_class(road_class, unreal.Vector(0.0, 0.0, 0.0))
        road.set_actor_label(label)
        road.attach_to_actor(network, "", keep, keep, keep, False)
        spline = road.get_editor_property("spline")
        spline.set_editor_property("prefab", "Medium Road", notify_mode=never)
        spline.clear_spline_points(False)
        for x, z in ((0.0, 0.0), (SLOPE_LENGTH, rise)):
            spline.add_spline_point(unreal.Vector(x, y, z), unreal.SplineCoordinateSpace.LOCAL, False)
            spline.set_spline_point_type(spline.get_number_of_spline_points() - 1, unreal.SplinePointType.LINEAR, False)
        spline.update_spline()
        if label.startswith("Street"):
            street = ACTORS.spawn_actor_from_class(unreal.CSCityStreetActor, unreal.Vector(0.0, 0.0, 0.0))
            street.set_actor_label("Street hung under the road")
            street.attach_to_actor(road, "", keep, keep, keep, False)
            street.set_editor_property("fabric", unreal.CSCityFabric.MID_MIXED, notify_mode=unreal.PropertyAccessChangeNotifyMode.ALWAYS)
            for prop, value in (("kit", kits["DA_NCSimple_Round"]), ("vacancy", 0.0), ("frontage", 24.0), ("gap", 0.0), ("setback", 4.0), ("min_depth", 18.0), ("max_depth", 24.0),
                                ("overhang_chance", 0.0), ("podium_chance", 0.0), ("follow_from", 10.0), ("follow_to", 110.0)):
                street.set_editor_property(prop, value, notify_mode=never)
    network.rebuild()
    log("road: {} buildings, error '{}'".format(network.get_editor_property("num_buildings"), network.get_editor_property("last_error")))
    if not LEVELS.save_current_level():
        fail("could not save " + path)
        return
    log("saved " + path)


def make_field_test(kits):
    """A street 200 m long, its buildings drawn plain, under a field painted a stretch at a time."""
    path = FOLDER + "/L_NCFieldTest"
    if not new_level(path):
        return
    ground = ACTORS.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(10000.0, 0.0, 0.0))
    ground.set_actor_label("Ground")
    ground.set_actor_scale3d(unreal.Vector(300.0, 200.0, 1.0))
    component = ground.get_component_by_class(unreal.StaticMeshComponent)
    component.set_editor_property("static_mesh", unreal.load_asset("/Engine/BasicShapes/Plane"))
    component.set_material(0, unreal.load_asset("/Engine/BasicShapes/BasicShapeMaterial"))
    street = ACTORS.spawn_actor_from_class(unreal.CSCityStreetActor, unreal.Vector(0.0, 0.0, 0.0))
    street.set_actor_label("Street")
    street.set_editor_property("fabric", unreal.CSCityFabric.MID_MIXED, notify_mode=unreal.PropertyAccessChangeNotifyMode.ALWAYS)
    for prop, value in (("kit", kits["DA_NCSimple_Styles"]), ("vacancy", 0.0), ("street_width", 16.0), ("frontage", 24.0), ("gap", 2.0), ("setback", 3.0), ("min_depth", 18.0), ("max_depth", 24.0),
                        ("overhang_chance", 0.0), ("podium_chance", 0.0), ("setback_chance", 0.0)):
        street.set_editor_property(prop, value, notify_mode=NEVER)
    line = street.get_editor_property("street")
    line.clear_spline_points(False)
    for x in (0.0, 20000.0):
        line.add_spline_point(unreal.Vector(x, 0.0, 0.0), unreal.SplineCoordinateSpace.WORLD, False)
    line.update_spline()
    field = ACTORS.spawn_actor_from_class(unreal.load_class(None, "/Script/CSRoadNet.CSCityFieldActor"), unreal.Vector(10000.0, 0.0, 0.0))
    field.set_actor_label("City Field")
    # Fronts on both sides lie 11 m from the centre line (half the street, the setback), the backs 18 to 24 m beyond.
    def cm(x0, y0, x1, y1):
        return unreal.Vector2D(x0 * 100.0, y0 * 100.0), unreal.Vector2D(x1 * 100.0, y1 * 100.0)
    # 0 to 50 m: the fronts stand out.
    field.set_overhang_in_box(*cm(0.0, -14.0, 50.0, 14.0), unreal.CSCityOverhangField.ALWAYS)
    # 50 to 100 m: round corners.
    field.set_corner_style_in_box(*cm(50.0, -40.0, 100.0, 40.0), unreal.CSCityCornerStyle.ROUND)
    # 100 to 150 m: cut-off corners, the fronts stand out on one side of the street only.
    field.set_corner_style_in_box(*cm(100.0, -40.0, 150.0, 40.0), unreal.CSCityCornerStyle.CHAMFER)
    field.set_overhang_in_box(*cm(100.0, 8.0, 150.0, 14.0), unreal.CSCityOverhangField.ALWAYS)
    # 150 to 200 m: nothing painted.
    log("field: {} pages, {} bytes on the GPU".format(field.get_editor_property("num_pages"), field.gpu_bytes()))
    if not LEVELS.save_current_level():
        fail("could not save " + path)
        return
    log("saved " + path)


GROUND_KIT = "DA_NCSimple_Ground"
GROUND_SLOTS = {"slab": "Slab_W300", "wall": "RetainWall_W300_H100", "planter": "Planter_W300_D150", "planter2": "PlanterTree_W200", "cart_shop": "CartShop_W240", "cart_shop2": "CartShop_W300"}


def make_ground_kit(meshes):
    path = FOLDER + "/" + GROUND_KIT
    if ASSETS.does_asset_exist(path):
        kit = ASSETS.load_asset(path)
    else:
        factory = unreal.DataAssetFactory()
        factory.set_editor_property("data_asset_class", unreal.CSCityGroundKit.static_class())
        kit = TOOLS.create_asset(GROUND_KIT, FOLDER, unreal.CSCityGroundKit, factory)
        log("made " + path)
    if not kit:
        fail("could not make " + path)
        return None
    for slot, key in GROUND_SLOTS.items():
        module = unreal.CSCityFacadeModule()
        module.set_editor_property("mesh", meshes[key])
        module.set_editor_property("quarter_turns", 0)
        kit.set_editor_property(slot, module)
    ASSETS.save_loaded_asset(kit, False)
    for slot, key in GROUND_SLOTS.items():
        if kit.get_editor_property(slot).get_editor_property("mesh") != meshes[key]:
            fail("{}: slot {} reads back wrong".format(path, slot))
    log("kit " + kit.get_path_name())
    return kit


# L_NCBlocksHill: three streets each way, 100 m apart, closing in four blocks on a hill; the crossings' heights (m), row by row from y = 0.
BLOCK_GRID = (0.0, 100.0, 200.0)
BLOCK_HEIGHTS = ((0.0, 2.0, 4.0), (3.0, 8.0, 6.0), (6.0, 10.0, 12.0))


def make_blocks_hill(kits, ground):
    """Four blocks of a network on a hill: the buildings along their roads, instanced ground over everything, and the city's field saying what the
    ground left free has in each: planters; a night market; half of each with the middle raised 3 m; and nothing, the fronts standing out."""
    path = FOLDER + "/L_NCBlocksHill"
    if not new_level(path):
        return
    keep = unreal.AttachmentRule.KEEP_WORLD
    network = ACTORS.spawn_actor_from_class(unreal.load_class(None, "/Script/CSRoadNet.CSRoadNetworkActor"), unreal.Vector(0.0, 0.0, 0.0))
    network.set_actor_label("Roads on a hill")
    for prop, value in (("local_update", True), ("tile_size", 64.0), ("batch_buildings", True), ("building_kit", kits["DA_NCSimple_Styles"]), ("build_ground", True), ("ground_kit", ground)):
        network.set_editor_property(prop, value, notify_mode=NEVER)
    lots = network.get_editor_property("lots")
    for prop, value in (("frontage", 18.0), ("gap", 2.0), ("setback", 3.0), ("fill_depth", 22.0), ("min_depth", 12.0), ("max_depth", 18.0), ("min_height", 12.0), ("max_height", 28.0), ("vacancy", 0.25)):
        lots.set_editor_property(prop, value)
    network.set_editor_property("lots", lots, notify_mode=NEVER)
    road_class = unreal.load_class(None, "/Script/CSRoadNet.CSRoadSplineActor")

    def road(label, points):
        actor = ACTORS.spawn_actor_from_class(road_class, unreal.Vector(0.0, 0.0, 0.0))
        actor.set_actor_label(label)
        actor.attach_to_actor(network, "", keep, keep, keep, False)
        spline = actor.get_editor_property("spline")
        spline.set_editor_property("prefab", "Medium Road", notify_mode=NEVER)
        spline.clear_spline_points(False)
        for x, y, z in points:
            spline.add_spline_point(unreal.Vector(x * 100.0, y * 100.0, z * 100.0), unreal.SplineCoordinateSpace.LOCAL, False)
            spline.set_spline_point_type(spline.get_number_of_spline_points() - 1, unreal.SplinePointType.LINEAR, False)
        spline.update_spline()

    for j, y in enumerate(BLOCK_GRID):
        road("Street {:.0f}".format(y), [(x, y, BLOCK_HEIGHTS[j][i]) for i, x in enumerate(BLOCK_GRID)])
    for i, x in enumerate(BLOCK_GRID):
        road("Avenue {:.0f}".format(x), [(x, y, BLOCK_HEIGHTS[j][i]) for j, y in enumerate(BLOCK_GRID)])

    field = ACTORS.spawn_actor_from_class(unreal.load_class(None, "/Script/CSRoadNet.CSCityFieldActor"), unreal.Vector(10000.0, 10000.0, 1500.0))
    field.set_actor_label("City Field")

    def cm(x0, y0, x1, y1):
        return unreal.Vector2D(x0 * 100.0, y0 * 100.0), unreal.Vector2D(x1 * 100.0, y1 * 100.0)
    field.set_gap_style_in_box(*cm(0.0, 0.0, 100.0, 100.0), unreal.CSCityGapStyle.PLANTER)
    field.set_gap_style_in_box(*cm(100.0, 0.0, 200.0, 100.0), unreal.CSCityGapStyle.NIGHT_MARKET)
    field.set_corner_style_in_box(*cm(100.0, 0.0, 200.0, 100.0), unreal.CSCityCornerStyle.ROUND)
    field.set_gap_style_in_box(*cm(0.0, 100.0, 50.0, 200.0), unreal.CSCityGapStyle.PLANTER)
    field.set_gap_style_in_box(*cm(50.0, 100.0, 100.0, 200.0), unreal.CSCityGapStyle.NIGHT_MARKET)
    field.set_corner_style_in_box(*cm(0.0, 100.0, 100.0, 200.0), unreal.CSCityCornerStyle.CHAMFER)
    field.set_elevation_in_box(*cm(32.0, 132.0, 68.0, 168.0), 3.0)
    field.set_gap_style_in_box(*cm(100.0, 100.0, 200.0, 200.0), unreal.CSCityGapStyle.EMPTY)
    field.set_overhang_in_box(*cm(100.0, 100.0, 200.0, 200.0), unreal.CSCityOverhangField.ALWAYS)
    network.rebuild()
    log("blocks: {} buildings, {} ground tiles, error '{}'; field {} pages, {} bytes on the GPU".format(
        network.get_editor_property("num_buildings"), network.get_editor_property("num_ground_tiles"), network.get_editor_property("last_error"), field.get_editor_property("num_pages"), field.gpu_bytes()))
    if not LEVELS.save_current_level():
        fail("could not save " + path)
        return
    log("saved " + path)


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
    ground = make_ground_kit(meshes)
    if len(kits) == len(KITS) and ground:
        make_level(kits)
        make_types_level(kits)
        make_slope_streets(kits)
        make_slope_road(kits)
        make_field_test(kits)
        make_blocks_hill(kits, ground)


run()
