# -*- coding: utf-8 -*-
"""Creates /PCGPlugins/CSRoadNet/L_RoadTierMini: the smallest scene for how small roads give way to main roads.

  UnrealEditor-Cmd.exe <project> -ExecutePythonScript=<this file> -unattended -nosplash
  python <this file> --check     (no engine: the crossings' angles and distances checked)

Two by two blocks of 80 m on flat ground, bounded by small roads (tier Alley: alleys east-west, small roads
north-south), 2 m lanes through the western blocks and three main roads (Medium Road), 13 spline roads in all, in
64 m tiles. Everything comes from the plugin: the buildings are the network's own prisms, filling the blocks, so the
level opens the same on any machine. Docs/CS2RoadsGpu.md, "小路与大路小场景", says what to try:

- A Drag me: south of the blocks, clear of every small road: drag it into the grid and back;
- B Drawn: along the west side, each alley ending on one of its points: T junctions kept as drawn, until B moves;
- C Crossing: north-south through the middle of the eastern blocks: the three alleys cut, a crossing each;
- the lanes, Alley 2m (Data/CustomRoads.json: Alley with only its 1 m shoulders, which make its junctions; its plots
  half a metre from it), between the houses: D with two bends, E branching off it to the west street and G a dead end off it
  (south-west); F going on from D across the middle alley in an S, A crossing it when dragged in (north-west). The
  eastern blocks, which C narrows to some 14 m of plots a side, have none.
- H House with garage: a city building between the last alley and A, its garage facing A: the network joins it to A with
  a driveway (an Alley) and a T junction, which follow the house, the garage (move the diamond) and A; with A dragged
  away, nothing is in front of the garage and the driveway goes.

Refuses to overwrite the level; delete it first to make it again.
"""
import json
import math
import sys

try:
    import unreal
except ImportError:
    unreal = None

LEVEL = "/PCGPlugins/CSRoadNet/L_RoadTierMini"
GROUND_MATERIAL = "/PCGPlugins/CSRoadNet/M_SplineRoadDemoGround"
TEMPLATE = "/Engine/Maps/Templates/Template_Default"
TILE = 64.0

MAIN = "Medium Road"
ALLEY = "Alley"
STREET = "Small Road"
LANE = "Alley 2m"
# Half widths (m) for the checks; the network reads the real ones from the prefabs.
HALF_WIDTHS = {MAIN: 12.0, ALLEY: 4.0, STREET: 8.0, LANE: 1.0}
# A main road covers a small road this far past their half widths (SplineSettings.AlleyClearance).
CLEARANCE = 1.0

COLUMNS = (0.0, 80.0, 160.0)
ROWS = (0.0, 80.0, 160.0)
WEST = -40.0

# The lanes: (label, points in m, curved). Each end on a small road, on another lane's point (E on D's first bend, G on
# its third) or in the block (G: a dead end); D and F meet across the middle alley. Every lane keeps more than a main
# road's reach (MAIN half width, LANE's, CLEARANCE) from where the main roads lie and where RoadTierMiniCheck.py moves
# them, so none is covered along its length; their junctions keep 30 m from where the main roads cross the small roads
# (check()); and 30 m or more from the parallel streets, so plots fit between.
LANES = (
    ("D Lane bends", [(40.0, 0.0), (40.0, 35.0), (50.0, 45.0), (50.0, 57.0), (50.0, 80.0)], False),
    ("E Lane branch", [(0.0, 35.0), (40.0, 35.0)], False),
    ("F Lane curve", [(50.0, 80.0), (42.0, 100.0), (48.0, 140.0), (40.0, 160.0)], True),
    ("G Lane dead end", [(25.0, 57.0), (50.0, 57.0)], False),
)

# (label, prefab, points in m, small road, curved)
ROADS = (
    [("Alley y{:.0f}".format(y), ALLEY, [(WEST, y), (COLUMNS[-1], y)], True, False) for y in ROWS]
    + [("Small Road x{:.0f}".format(x), STREET, [(x, ROWS[0]), (x, ROWS[-1])], True, False) for x in COLUMNS]
    + [(label, LANE, points, True, curved) for label, points, curved in LANES]
    + [
        ("A Drag me", MAIN, [(0.0, 230.0), (100.0, 230.0), (200.0, 230.0)], False, False),
        # A point for each alley ending here: that T junction is drawn, and stays.
        ("B Drawn", MAIN, [(WEST, -40.0)] + [(WEST, y) for y in ROWS] + [(WEST, 190.0)], False, False),
        ("C Crossing", MAIN, [(120.0, -40.0), (120.0, 190.0)], False, False),
    ]
)

# Where RoadTierMiniCheck.py moves the main roads (its EDITS): the layout holds for these too.
MOVED = {
    "A Drag me": [(0.0, 120.0), (100.0, 120.0), (200.0, 120.0)],
    "B Drawn": [(x + 10.0, y) for x, y in ROADS[-2][2]],
    "C Crossing": [(x - 10.0, y) for x, y in ROADS[-1][2]],
}

# The house with a garage (ACSCityBuildingActor): its middle (m), 20 x 14 m (the building's default outline), the garage
# in the middle of its side facing A (+y), 33 m from A's centre line (21 m from its edge); its plot (4 m round it) leaves
# 12 m of plots along the alley south of it.
HOUSE = ("H House with garage", (60.0, 190.0))
HOUSE_GARAGE = (0.0, 700.0)

# What each main road is there for, and where its label floats (m), above the tallest house.
LABELS = (
    ("A Drag me\ninto the grid", (100.0, 255.0)),
    ("B Drawn\nalleys end on its points", (-78.0, 80.0)),
    ("C Crossing\nacross three alleys", (120.0, -62.0)),
    ("Lanes 2m\nbetween the houses", (40.0, -40.0)),
    ("H House with garage\njoined to A", (25.0, 205.0)),
)
LABEL_HEIGHT = 26.0


def segments(points):
    return list(zip(points, points[1:]))


def crossing(a, b):
    """Where segments a and b cross: (x, y, degrees between them) or None."""
    (ax, ay), (bx, by) = a
    (cx, cy), (dx, dy) = b
    ux, uy, vx, vy = bx - ax, by - ay, dx - cx, dy - cy
    cross = ux * vy - uy * vx
    if abs(cross) < 1e-9:
        return None
    s = ((cx - ax) * vy - (cy - ay) * vx) / cross
    t = ((cx - ax) * uy - (cy - ay) * ux) / cross
    if not (0.0 <= s <= 1.0 and 0.0 <= t <= 1.0):
        return None
    return ax + s * ux, ay + s * uy, math.degrees(math.asin(min(1.0, abs(cross) / (math.hypot(ux, uy) * math.hypot(vx, vy)))))


def distance(p, segment):
    (ax, ay), (bx, by) = segment
    ux, uy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((p[0] - ax) * ux + (p[1] - ay) * uy) / (ux * ux + uy * uy)))
    return math.hypot(p[0] - ax - t * ux, p[1] - ay - t * uy)


def check():
    """The layout as the junctions need it, with the main roads where they lie and where RoadTierMiniCheck.py moves them:
    a main road crossing a small road at 30 degrees or more, 30 m or more from a junction of small roads and 15 m from a
    lane's bend (else a sliver of a junction, a known way to hang the GPU); A, as it lies, clear of every small road;
    every lane clear of every main road (none covered along its length)."""
    problems = []
    small = [r for r in ROADS if r[3]]
    lanes = [r for r in small if r[1] == LANE]
    # Junctions: the grid's nodes and the lanes' ends on another road (the others are dead ends); the other lane points
    # bend.
    def on_road(p, own):
        return any(distance(p, s) < 0.5 for r in small if r[0] != own for s in segments(r[2]))
    ends = [p for r in lanes for p in (r[2][0], r[2][-1]) if on_road(p, r[0])]
    junctions = [(x, y) for x in COLUMNS for y in ROWS] + ends
    bends = [p for r in lanes for p in r[2][1:-1] if p not in ends]
    drawn_ts = [(WEST, y) for y in ROWS]
    places = [(label, prefab, points, "") for label, prefab, points, small_road, _ in ROADS if not small_road]
    places += [(label, MAIN, points, " moved") for label, points in MOVED.items()]
    for label, prefab, points, how in places:
        for small_label, small_prefab, small_points, _, _ in small:
            for a in segments(points):
                for b in segments(small_points):
                    hit = crossing(a, b)
                    if not hit:
                        continue
                    at = (hit[0], hit[1])
                    near = min(math.hypot(hit[0] - x, hit[1] - y) for x, y in junctions)
                    bend = min([math.hypot(hit[0] - x, hit[1] - y) for x, y in bends] or [1e9])
                    print("  {}{} on {} at ({:.0f}, {:.0f}): {:.0f} degrees, {:.0f} m from a junction, {:.0f} m from a bend".format(label, how, small_label, hit[0], hit[1], hit[2], near, bend))
                    if hit[2] < 30.0:
                        problems.append("{}{} on {}: {:.0f} degrees".format(label, how, small_label, hit[2]))
                    if near < 30.0 and at not in drawn_ts:
                        problems.append("{}{} on {}: {:.0f} m from a junction".format(label, how, small_label, near))
                    if bend < 15.0:
                        problems.append("{}{} on {}: {:.0f} m from a bend".format(label, how, small_label, bend))
            reach = HALF_WIDTHS[prefab] + HALF_WIDTHS[small_prefab] + CLEARANCE
            if label == "A Drag me" and not how:
                gap = min(distance(p, s) for p in small_points for s in segments(points))
                if gap <= reach:
                    problems.append("A Drag me covers {} ({:.0f} m, reach {:.0f} m)".format(small_label, gap, reach))
            if small_prefab == LANE:
                gap = min(distance(p, s) for p in small_points for s in segments(points))
                if gap <= reach:
                    problems.append("{}{} covers a point of {} ({:.0f} m, reach {:.0f} m)".format(label, how, small_label, gap, reach))
    print("too close: {}".format("; ".join(problems) or "nothing"))
    return problems


def log(text):
    unreal.log("CS2ROADTIERMINI " + text)


def build():
    never = unreal.PropertyAccessChangeNotifyMode.NEVER
    keep = unreal.AttachmentRule.KEEP_WORLD
    assets = unreal.EditorAssetLibrary
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if assets.does_asset_exist(LEVEL):
        unreal.log_error("CS2ROADTIERMINI {} exists; delete it to make it again".format(LEVEL))
        return
    if not levels.new_level_from_template(LEVEL, TEMPLATE):
        unreal.log_error("CS2ROADTIERMINI could not create {} from {}".format(LEVEL, TEMPLATE))
        return

    # The template's floor gives way to a ground 0.25 m under the roads.
    for actor in actors.get_all_level_actors():
        component = actor.get_component_by_class(unreal.StaticMeshComponent) if isinstance(actor, unreal.StaticMeshActor) else None
        mesh = component.get_editor_property("static_mesh") if component else None
        if mesh and mesh.get_name() == "SM_Template_Map_Floor":
            actors.destroy_actor(actor)
    xs = [p[0] for r in ROADS for p in r[2]]
    ys = [p[1] for r in ROADS for p in r[2]]
    lo_x, hi_x, lo_y, hi_y = min(xs) - 150.0, max(xs) + 150.0, min(ys) - 150.0, max(ys) + 150.0
    ground = actors.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector((lo_x + hi_x) * 50.0, (lo_y + hi_y) * 50.0, -25.0))
    ground.set_actor_label("Ground")
    ground.set_actor_scale3d(unreal.Vector(hi_x - lo_x, hi_y - lo_y, 1.0))
    component = ground.get_component_by_class(unreal.StaticMeshComponent)
    component.set_editor_property("static_mesh", unreal.load_asset("/Engine/BasicShapes/Plane"))
    component.set_material(0, assets.load_asset(GROUND_MATERIAL))

    network = actors.spawn_actor_from_class(unreal.load_class(None, "/Script/CSRoadNet.CSRoadNetworkActor"), unreal.Vector(0.0, 0.0, 0.0))
    network.set_actor_label("Road Tier Mini")
    network.set_editor_property("local_update", True, notify_mode=never)
    network.set_editor_property("tile_size", TILE, notify_mode=never)
    network.set_editor_property("batch_buildings", True, notify_mode=never)

    road_class = unreal.load_class(None, "/Script/CSRoadNet.CSRoadSplineActor")
    local = unreal.SplineCoordinateSpace.LOCAL
    for label, prefab, points, small, curved in ROADS:
        actor = actors.spawn_actor_from_class(road_class, unreal.Vector(0.0, 0.0, 0.0))
        actor.set_actor_label(label)
        actor.attach_to_actor(network, "", keep, keep, keep, False)
        spline = actor.get_editor_property("spline")
        spline.set_editor_property("prefab", prefab, notify_mode=never)
        if small:
            spline.set_editor_property("tier", unreal.CSRoadTier.ALLEY, notify_mode=never)
        spline.clear_spline_points(False)
        for x, y in points:
            spline.add_spline_point(unreal.Vector(x * 100.0, y * 100.0, 0.0), local, False)
        for i in range(len(points)):
            spline.set_spline_point_type(i, unreal.SplinePointType.CURVE if curved else unreal.SplinePointType.LINEAR, False)
        spline.update_spline()

    # The house with a garage: a change of its properties rebuilds it and tells the network.
    label, (x, y) = HOUSE
    house = actors.spawn_actor_from_class(unreal.load_class(None, "/Script/CSRoadNet.CSCityBuildingActor"), unreal.Vector(x * 100.0, y * 100.0, 0.0))
    house.set_actor_label(label)
    house.set_editor_property("garage_location", unreal.Vector(HOUSE_GARAGE[0] * 100.0, HOUSE_GARAGE[1] * 100.0, 0.0))
    house.set_editor_property("has_garage", True)

    # What each main road is there for, flat above the houses beside it.
    for caption, (x, y) in LABELS:
        text = actors.spawn_actor_from_class(unreal.TextRenderActor, unreal.Vector(x * 100.0, y * 100.0, LABEL_HEIGHT * 100.0), unreal.Rotator(roll=0.0, pitch=90.0, yaw=180.0))
        text.set_actor_label("Label " + caption.split("\n")[0])
        render = text.get_editor_property("text_render")
        render.set_editor_property("text", unreal.Text(caption))
        render.set_editor_property("world_size", 400.0)
        render.set_editor_property("horizontal_alignment", unreal.HorizTextAligment.EHTA_CENTER)

    network.rebuild()
    keys = ("num_splines", "num_roads", "num_tiles", "pending_tiles", "num_buildings", "num_garages", "num_joined_garages", "last_error")
    stats = {k: network.get_editor_property(k) for k in keys}
    log("build " + json.dumps(stats) + "; garage: " + house.get_editor_property("garage_status"))
    if stats["last_error"]:
        unreal.log_warning("CS2ROADTIERMINI the build failed, the level is saved all the same: " + stats["last_error"])
    if not levels.save_current_level():
        unreal.log_error("CS2ROADTIERMINI could not save " + LEVEL)
        return
    log("saved " + LEVEL)


if unreal:
    build()
elif "--check" in sys.argv:
    sys.exit(1 if check() else 0)
else:
    print(__doc__)
