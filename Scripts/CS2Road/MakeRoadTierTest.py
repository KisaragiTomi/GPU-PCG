# -*- coding: utf-8 -*-
"""Creates /PCGPlugins/CSRoadNet/L_RoadTierTest: how small roads and main roads meet, in one small district.

  UnrealEditor-Cmd.exe <project> -ExecutePythonScript=<this file> -unattended -RenderOffscreen
  python <this file> --preview <png>     (no engine: the layout as a picture, and its distances checked)

A grid of small roads (tier Alley: alleys east-west, small roads north-south) about 500 x 240 m on flat ground,
and over it the main roads below, each somewhere of its own. Dragging one of them shows the small roads under it cut
and joining it, and the houses along them going and coming back. Docs/CS2RoadsGpu.md, "小路与大路测试关卡", says what
to look for:

- A Crossing: straight across four alleys, in the middle of the blocks: a crossing each;
- B Slant: on a slant over two alleys and a small road, ending inside a block;
- C Drawn: along the grid's west side, the alleys ending on its points: T junctions kept as drawn;
- D Along: 3 m off a small road along it: the stretch under it goes, an alley ending there joins it;
- E Over: a highway 12 m over an alley, on ramps: the alley stays whole;
- F Drag me: clear of everything, for dragging into the grid.

The batch buildings stand on the plots along every road as GPU instances of Nanite meshes: copies of six whole houses
of the Hong Kong pack (/Game/Hong_Kong_Street) with no translucent slot, which Nanite cannot draw, in
/PCGPlugins/CSRoadNet/Houses with Nanite on. Their materials get the Nanite usage flag (the pack's own houses stay
as they are). Refuses to overwrite the level; delete it first to make it again.
"""
import json
import math
import sys

try:
    import unreal
except ImportError:
    unreal = None

LEVEL = "/PCGPlugins/CSRoadNet/L_RoadTierTest"
FOLDER = "/PCGPlugins/CSRoadNet"
GROUND_MATERIAL = FOLDER + "/M_SplineRoadDemoGround"
TEMPLATE = "/Engine/Maps/Templates/Template_Default"
SOURCE_HOUSES = "/Game/Hong_Kong_Street/Models/Assembled_Houses"
HOUSES = FOLDER + "/Houses"
# The pack's whole houses with opaque materials only, fronts towards +Y (the network's default), 12 m wide but the last.
# SM_House_10 is opaque too, but at 16 m wider than the 14 m plots.
HOUSE_NAMES = ("SM_House_04", "SM_House_08", "SM_House_09", "SM_House_11", "SM_House_12", "SM_House_14")
TILE = 128.0

MAIN = "Medium Road"
ALLEY = "Alley"
STREET = "Small Road"
HIGHWAY = "Highway Twoway - 2 lanes"
# Half widths (m) for the preview's picture and checks: the network reads the real ones from the prefabs.
HALF_WIDTHS = {MAIN: 12.0, ALLEY: 4.0, STREET: 8.0, HIGHWAY: 7.0}
# A main road covers a small road this far past their half widths (SplineSettings.AlleyClearance).
CLEARANCE = 1.0

COLUMNS = (-100.0, 0.0, 100.0, 200.0)
ROWS = (-120.0, -40.0, 40.0, 120.0)
WEST = -300.0
# Through the middles of the block edges: over the alley at y -120 at x -50, the small road at x 0 at y -80, the alley at y -40 at x 50.
SLANT = (100.0 / 128.06, 80.0 / 128.06)


class Road:
    """A spline road in UE metres (x, y, z up): its points joined straight, or smooth (the spline's automatic tangents)."""

    def __init__(self, label, prefab, points, tier_small=False, smooth=False):
        self.label = label
        self.prefab = prefab
        self.points = [tuple(float(v) for v in (p if len(p) == 3 else (p[0], p[1], 0.0))) for p in points]
        self.tier_small = tier_small
        self.smooth = smooth


def roads():
    out = []
    for y in ROWS:
        # The row over which E passes runs on east of the grid to the highway and past it.
        east = 320.0 if y == 40.0 else COLUMNS[-1]
        out.append(Road("Alley y{:.0f}".format(y), ALLEY, [(WEST, y), (east, y)], tier_small=True))
    for x in COLUMNS:
        out.append(Road("Small Road x{:.0f}".format(x), STREET, [(x, ROWS[0]), (x, ROWS[-1])], tier_small=True))
    out.append(Road("A Crossing", MAIN, [(-200.0, -190.0), (-200.0, 190.0)]))
    start = (-50.0 - 64.03 * SLANT[0], -120.0 - 64.03 * SLANT[1])
    end = (50.0 + 30.0 * SLANT[0], -40.0 + 30.0 * SLANT[1])
    out.append(Road("B Slant", MAIN, [start, end]))
    # A point for each alley ending here: that T junction is drawn, and stays.
    out.append(Road("C Drawn", MAIN, [(WEST, -170.0)] + [(WEST, y) for y in ROWS] + [(WEST, 170.0)]))
    out.append(Road("D Along", MAIN, [(203.0, -80.0), (203.0, 0.0)]))
    out.append(Road("E Over", HIGHWAY, [(260.0, -70.0, 0.0), (260.0, 0.0, 12.0), (260.0, 80.0, 12.0), (260.0, 150.0, 0.0)], smooth=True))
    out.append(Road("F Drag me", MAIN, [(-60.0, 170.0), (40.0, 170.0), (140.0, 170.0)], smooth=True))
    return out


# What each main road is there for, and where its label lies (m).
LABELS = (
    ("A Crossing\nacross four alleys", (-200.0, -212.0)),
    ("B Slant\nends inside a block", (-112.0, -186.0)),
    ("C Drawn\nalleys end on its points", (-352.0, 0.0)),
    ("D Along\n3 m off a small road", (246.0, -100.0)),
    ("E Over\n12 m over an alley", (260.0, 168.0)),
    ("F Drag me\ninto the grid", (40.0, 200.0)),
)


def polyline(road, steps=12):
    """The road sampled: straight between its points, or as Catmull-Rom like the spline's automatic tangents."""
    p = road.points
    if not road.smooth:
        return p
    out = []
    for i in range(len(p) - 1):
        before = p[max(i - 1, 0)]
        after = p[min(i + 2, len(p) - 1)]
        m0 = tuple((b - a) * (1.0 if i == 0 else 0.5) for a, b in zip(before, p[i + 1]))
        m1 = tuple((b - a) * (1.0 if i + 2 == len(p) else 0.5) for a, b in zip(p[i], after))
        for k in range(0 if not out else 1, steps + 1):
            t = k / steps
            h00, h10, h01, h11 = 2 * t ** 3 - 3 * t ** 2 + 1, t ** 3 - 2 * t ** 2 + t, -2 * t ** 3 + 3 * t ** 2, t ** 3 - t ** 2
            out.append(tuple(h00 * a + h10 * u + h01 * d + h11 * v for a, u, d, v in zip(p[i], m0, p[i + 1], m1)))
    return out


def crossings(a, b):
    """Where two polylines cross in plan: (x, y, height apart, angle degrees between them, 0..90)."""
    out = []
    pa, pb = polyline(a), polyline(b)
    for i in range(len(pa) - 1):
        for j in range(len(pb) - 1):
            ax, ay, az = pa[i]
            dx, dy, dz = pa[i + 1][0] - ax, pa[i + 1][1] - ay, pa[i + 1][2] - az
            bx, by, bz = pb[j]
            ex, ey, ez = pb[j + 1][0] - bx, pb[j + 1][1] - by, pb[j + 1][2] - bz
            cross = dx * ey - dy * ex
            if abs(cross) < 1e-9:
                continue
            s = ((bx - ax) * ey - (by - ay) * ex) / cross
            t = ((bx - ax) * dy - (by - ay) * dx) / cross
            if 0.0 <= s <= 1.0 and 0.0 <= t <= 1.0:
                angle = math.degrees(math.asin(min(1.0, abs(cross) / (math.hypot(dx, dy) * math.hypot(ex, ey)))))
                out.append((ax + s * dx, ay + s * dy, abs(az + s * dz - (bz + t * ez)), angle))
    return out


def check(all_roads):
    """The layout's distances, as the junctions need them. A main road on a small road flatter than 30 degrees, or a junction
    near a grid node, leaves a sliver of a junction, a known way to hang the GPU: returns what is too close."""
    problems = []
    small = [r for r in all_roads if r.tier_small]
    mains = [r for r in all_roads if not r.tier_small]
    nodes = [(x, y) for x in COLUMNS for y in ROWS]
    for main in mains:
        for road in small:
            for x, y, apart, angle in crossings(main, road):
                if apart > 2.0:
                    print("  {} over {} at ({:.0f}, {:.0f}): {:.1f} m above, no junction".format(main.label, road.label, x, y, apart))
                    continue
                near = min(math.hypot(x - nx, y - ny) for nx, ny in nodes)
                print("  {} on {} at ({:.0f}, {:.0f}): {:.0f} degrees, {:.0f} m from the nearest grid node".format(main.label, road.label, x, y, angle, near))
                if angle < 30.0:
                    problems.append("{} on {}: {:.0f} degrees".format(main.label, road.label, angle))
                if near < 30.0:
                    problems.append("{} on {}: {:.0f} m from a grid node".format(main.label, road.label, near))
    for i, a in enumerate(mains):
        for b in mains[i + 1:]:
            if crossings(a, b):
                problems.append("main roads {} and {} cross".format(a.label, b.label))
    return problems


def preview(path):
    """The layout from above without the engine: the roads at their widths, what the main roads cover and the tile grid."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    all_roads = roads()
    problems = check(all_roads)
    xs = [p[0] for r in all_roads for p in r.points]
    ys = [p[1] for r in all_roads for p in r.points]
    lo_x, hi_x, lo_y, hi_y = min(xs) - 110.0, max(xs) + 50.0, min(ys) - 50.0, max(ys) + 50.0
    figure, axes = plt.subplots(figsize=(14, 14 * (hi_y - lo_y) / (hi_x - lo_x)), dpi=110)
    for k in range(math.floor(lo_x / TILE), math.ceil(hi_x / TILE) + 1):
        axes.axvline(k * TILE, color="#dddddd", linewidth=0.7, zorder=0)
    for k in range(math.floor(lo_y / TILE), math.ceil(hi_y / TILE) + 1):
        axes.axhline(k * TILE, color="#dddddd", linewidth=0.7, zorder=0)
    metres = 110.0 * 14 / (hi_x - lo_x) * 72.0 / 110.0
    for road in all_roads:
        if not road.tier_small:
            line = polyline(road)
            axes.plot([p[0] for p in line], [p[1] for p in line], color="#fde68a", linewidth=(HALF_WIDTHS[road.prefab] + HALF_WIDTHS[ALLEY] + CLEARANCE) * 2.0 * metres, solid_capstyle="round", zorder=1)
    for road in all_roads:
        line = polyline(road)
        half = HALF_WIDTHS[road.prefab]
        colour = "#d97706" if road.tier_small else ("#2563eb" if max(p[2] for p in line) > 2.0 else "#222222")
        axes.plot([p[0] for p in line], [p[1] for p in line], color=colour, linewidth=half * 2.0 * metres, solid_capstyle="butt", alpha=0.85, zorder=3 if not road.tier_small else 2)
        axes.plot([p[0] for p in road.points], [p[1] for p in road.points], "o", color="white", markeredgecolor=colour, markersize=3, zorder=4)
    for caption, (x, y) in LABELS:
        axes.text(x, y, caption, ha="center", va="center", fontsize=8, linespacing=1.1, zorder=5, bbox=dict(boxstyle="round,pad=0.2", facecolor="white", edgecolor="none", alpha=0.8))
    axes.set_xlim(lo_x, hi_x)
    axes.set_ylim(hi_y, lo_y)
    axes.set_aspect("equal")
    axes.set_title("L_RoadTierTest: {} spline roads, {:.0f} x {:.0f} m (grey lines: {:.0f} m tiles; orange: small roads; black: main roads; blue: over 2 m up; yellow: what a main road covers of an alley)".format(len(all_roads), max(xs) - min(xs), max(ys) - min(ys), TILE), fontsize=8)
    figure.tight_layout()
    figure.savefig(path)
    print("preview {}: {} roads".format(path, len(all_roads)))
    print("too close: {}".format("; ".join(problems) or "nothing"))


def log(text):
    unreal.log("CS2ROADTIER " + text)


def nanite_houses():
    """The houses as Nanite meshes, copied once. Their materials must say they are used with Nanite, or it draws them in the default material."""
    assets = unreal.EditorAssetLibrary
    meshes = []
    flagged = set()
    for name in HOUSE_NAMES:
        path = "{}/{}".format(HOUSES, name)
        if not assets.does_asset_exist(path) and not assets.duplicate_asset("{}/{}".format(SOURCE_HOUSES, name), path):
            unreal.log_error("CS2ROADTIER could not copy {}/{}".format(SOURCE_HOUSES, name))
            continue
        mesh = assets.load_asset(path)
        for slot in mesh.get_editor_property("static_materials"):
            material = slot.get_editor_property("material_interface")
            base = material.get_base_material() if material else None
            if not base or base.get_path_name() in flagged:
                continue
            flagged.add(base.get_path_name())
            if base.get_editor_property("blend_mode") not in (unreal.BlendMode.BLEND_OPAQUE, unreal.BlendMode.BLEND_MASKED):
                unreal.log_error("CS2ROADTIER {} of {} is translucent: Nanite cannot draw it".format(base.get_name(), name))
            if not base.get_editor_property("bUsedWithNanite"):
                unreal.MaterialEditingLibrary.set_material_usage(base, unreal.MaterialUsage.MATUSAGE_NANITE)
                unreal.MaterialEditingLibrary.recompile_material(base)
                assets.save_loaded_asset(base, False)
                log("{} now used with Nanite".format(base.get_path_name()))
        settings = mesh.get_editor_property("nanite_settings")
        if not settings.get_editor_property("enabled"):
            settings.set_editor_property("enabled", True)
            # Through set_editor_property: PostEditChangeProperty builds the mesh.
            mesh.set_editor_property("nanite_settings", settings)
            assets.save_loaded_asset(mesh, False)
        box = mesh.get_bounding_box()
        size = box.max - box.min
        log("{}: Nanite {}, {:.1f} x {:.1f} x {:.1f} m".format(name, mesh.get_editor_property("nanite_settings").get_editor_property("enabled"), size.x / 100.0, size.y / 100.0, size.z / 100.0))
        meshes.append(mesh)
    return meshes


def build():
    never = unreal.PropertyAccessChangeNotifyMode.NEVER
    keep = unreal.AttachmentRule.KEEP_WORLD
    assets = unreal.EditorAssetLibrary
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if assets.does_asset_exist(LEVEL):
        unreal.log_error("CS2ROADTIER {} exists; delete it to make it again".format(LEVEL))
        return
    meshes = nanite_houses()
    if len(meshes) != len(HOUSE_NAMES):
        unreal.log_error("CS2ROADTIER {} of {} houses; nothing saved".format(len(meshes), len(HOUSE_NAMES)))
        return
    if not levels.new_level_from_template(LEVEL, TEMPLATE):
        unreal.log_error("CS2ROADTIER could not create {} from {}".format(LEVEL, TEMPLATE))
        return

    # The template's floor gives way to a ground 0.25 m under the roads.
    for actor in actors.get_all_level_actors():
        component = actor.get_component_by_class(unreal.StaticMeshComponent) if isinstance(actor, unreal.StaticMeshActor) else None
        mesh = component.get_editor_property("static_mesh") if component else None
        if mesh and mesh.get_name() == "SM_Template_Map_Floor":
            actors.destroy_actor(actor)
    all_roads = roads()
    xs = [p[0] for r in all_roads for p in r.points]
    ys = [p[1] for r in all_roads for p in r.points]
    lo_x, hi_x, lo_y, hi_y = min(xs) - 300.0, max(xs) + 300.0, min(ys) - 300.0, max(ys) + 300.0
    ground = actors.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector((lo_x + hi_x) * 50.0, (lo_y + hi_y) * 50.0, -25.0))
    ground.set_actor_label("Ground")
    ground.set_actor_scale3d(unreal.Vector(hi_x - lo_x, hi_y - lo_y, 1.0))
    component = ground.get_component_by_class(unreal.StaticMeshComponent)
    component.set_editor_property("static_mesh", unreal.load_asset("/Engine/BasicShapes/Plane"))
    component.set_material(0, assets.load_asset(GROUND_MATERIAL))

    network = actors.spawn_actor_from_class(unreal.load_class(None, "/Script/CSRoadNet.CSRoadNetworkActor"), unreal.Vector(0.0, 0.0, 0.0))
    network.set_actor_label("Road Tier Test")
    network.set_editor_property("local_update", True, notify_mode=never)
    network.set_editor_property("tile_size", TILE, notify_mode=never)
    network.set_editor_property("batch_buildings", True, notify_mode=never)
    network.set_editor_property("building_meshes", meshes, notify_mode=never)
    network.set_editor_property("build_bridge_piers", True, notify_mode=never)

    road_class = unreal.load_class(None, "/Script/CSRoadNet.CSRoadSplineActor")
    local = unreal.SplineCoordinateSpace.LOCAL

    def centimetres(v):
        return unreal.Vector(v[0] * 100.0, v[1] * 100.0, v[2] * 100.0)

    for road in all_roads:
        actor = actors.spawn_actor_from_class(road_class, unreal.Vector(0.0, 0.0, 0.0))
        actor.set_actor_label(road.label)
        actor.attach_to_actor(network, "", keep, keep, keep, False)
        spline = actor.get_editor_property("spline")
        spline.set_editor_property("prefab", road.prefab, notify_mode=never)
        if road.tier_small:
            spline.set_editor_property("tier", unreal.CSRoadTier.ALLEY, notify_mode=never)
        spline.clear_spline_points(False)
        for point in road.points:
            spline.add_spline_point(centimetres(point), local, False)
        if not road.smooth:
            for i in range(len(road.points)):
                spline.set_spline_point_type(i, unreal.SplinePointType.LINEAR, False)
        spline.update_spline()

    # What each main road is there for, flat on the ground beside it.
    for caption, (x, y) in LABELS:
        try:
            text = actors.spawn_actor_from_class(unreal.TextRenderActor, unreal.Vector(x * 100.0, y * 100.0, 10.0), unreal.Rotator(roll=0.0, pitch=90.0, yaw=180.0))
            text.set_actor_label("Label " + caption.split("\n")[0])
            render = text.get_editor_property("text_render")
            render.set_editor_property("text", unreal.Text(caption))
            render.set_editor_property("world_size", 500.0)
            render.set_editor_property("horizontal_alignment", unreal.HorizTextAligment.EHTA_CENTER)
        except Exception as error:
            unreal.log_warning("CS2ROADTIER label {}: {}".format(caption, error))

    network.rebuild()
    keys = ("num_splines", "num_roads", "num_tiles", "pending_tiles", "num_buildings", "num_bridge_piers", "skipped_bridge_piers", "last_error")
    stats = {k: network.get_editor_property(k) for k in keys}
    log("build " + json.dumps(stats))
    # The roads are all in the splines and the level builds them when opened: a build that fails here (no CS2 prefab data on
    # this machine, say) costs the level nothing.
    if stats["last_error"]:
        unreal.log_warning("CS2ROADTIER the build failed, the level is saved all the same: " + stats["last_error"])
    if not levels.save_current_level():
        unreal.log_error("CS2ROADTIER could not save " + LEVEL)
        return
    log("saved " + LEVEL)


if unreal:
    build()
elif "--preview" in sys.argv:
    preview(sys.argv[sys.argv.index("--preview") + 1])
else:
    print(__doc__)
