# -*- coding: utf-8 -*-
"""Creates /PCGPlugins/CSRoadNet/L_RoadEdgeCases: road layouts where the CS2 road network may go wrong.

  UnrealEditor-Cmd.exe <project> -ExecutePythonScript=<this file> -unattended -RenderOffscreen
  python <this file> --preview <png>     (no engine: the layout as a picture)

One ACSRoadNetworkActor, built in 256 m tiles and baked to Nanite, takes the spline roads of four
groups of scenes, each scene apart from the others and labelled in the level (Docs/CS2RoadsGpu.md,
"问题场景关卡", says what each one is there for):

- A, alleys giving way to main roads (the cases of the Splines.Alleys test), including two main
  roads mirrored about an alley (the alley lies exactly as near to both), a closed alley covered
  where it starts and a grid of alleys a main road crosses on the diagonal;
- B, the spline network's thresholds and odd input: ends 0.9 / 1.1 m apart (SnapDistance 1 m),
  crossings 1.9 / 2.1 m apart in height (JunctionHeight 2 m), a 3 m segment with automatic
  tangents, a hairpin, a long straight, a 10 degree crossing, eight roads on one node, a copy of
  a spline, a ring, two roads closer than their widths, a steep ramp;
- C, tile edges: a junction, a road's middle and an overpass right on tile corners, a long curve
  over several tiles crossed on a tile border;
- R, RoadLab's own scenarios (the golden tests' RoadScenarios, from Saved/CSRoadGolden or
  CS2ROADGOLDEN), each edge a fixed spline (bAdjustSegments off), plus the other two prefabs of
  the scenarios the GPU is known to disagree on (elevated-cross, curved-viaduct, highway-merge,
  sloped-junction).

The ground is flat at height 0 (no NetworkFile), so a road's elevation is its height, as in the
scenarios. Refuses to overwrite the level; delete it first to make it again.
"""
import json
import math
import os
import sys

try:
    import unreal
except ImportError:
    unreal = None

LEVEL = "/PCGPlugins/CSRoadNet/L_RoadEdgeCases"
FOLDER = "/PCGPlugins/CSRoadNet"
GROUND_MATERIAL = FOLDER + "/M_SplineRoadDemoGround"
TEMPLATE = "/Engine/Maps/Templates/Template_Default"
TILE = 256.0
GOLDEN_BASE = ["acute-y", "asymmetric-tee", "bridge-transition", "cross", "curve", "curved-viaduct", "elevated-cross", "elevated-mixed", "five-way",
               "highway-merge", "mixed-width", "multi-crossing", "oblique-cross", "six-way", "slope", "sloped-junction", "stacked-cross", "tee"]
GOLDEN_VARIANTS = [scenario + variant for scenario in ("elevated-cross", "curved-viaduct", "highway-merge", "sloped-junction")
                   for variant in ("@Highway_Twoway_-_2_lanes", "@Medium_Road_Divided")]


class Road:
    """A spline road in the network's space (UE metres: x, y, z up). Without tangents its points are
    joined straight (linear points); smooth leaves the tangents to the spline (automatic); tangents
    (arrive, leave) per point fix them, as a converted edge's Bezier gives them."""

    def __init__(self, label, prefab, points, tangents=None, smooth=False, closed=False, alley=False, adjust=True):
        self.label = label
        self.prefab = prefab
        self.points = [tuple(float(v) for v in (p if len(p) == 3 else (p[0], p[1], 0.0))) for p in points]
        self.tangents = tangents
        self.smooth = smooth
        self.closed = closed
        self.alley = alley
        self.adjust = adjust

    def moved(self, dx, dy):
        return Road(self.label, self.prefab, [(x + dx, y + dy, z) for x, y, z in self.points], self.tangents, self.smooth, self.closed, self.alley, self.adjust)

    def automatic(self, i):
        """About the tangent the spline gives point i by itself: half the step from the point before to
        the point after, one step at an open end."""
        p, n = self.points, len(self.points)
        if self.closed:
            before, after, scale = p[(i - 1) % n], p[(i + 1) % n], 0.5
        elif i == 0:
            before, after, scale = p[0], p[1], 1.0
        elif i == n - 1:
            before, after, scale = p[n - 2], p[n - 1], 1.0
        else:
            before, after, scale = p[i - 1], p[i + 1], 0.5
        return tuple((av - bv) * scale for av, bv in zip(after, before))

    def polyline(self, steps=16):
        """The road sampled for the preview: its segments as the Bezier curves the network takes them as
        (b = a + leave / 3, c = d - arrive / 3)."""
        n = len(self.points)
        out = []
        for i in range(n if self.closed else n - 1):
            j = (i + 1) % n
            a, d = self.points[i], self.points[j]
            if self.tangents:
                leave, arrive = self.tangents[i][1], self.tangents[j][0]
            elif self.smooth:
                leave, arrive = self.automatic(i), self.automatic(j)
            else:
                leave = arrive = tuple(dv - av for av, dv in zip(a, d))
            b = tuple(av + lv / 3.0 for av, lv in zip(a, leave))
            c = tuple(dv - rv / 3.0 for dv, rv in zip(d, arrive))
            for k in range(0 if not out else 1, steps + 1):
                t = k / steps
                s = 1.0 - t
                out.append(tuple(av * s ** 3 + 3 * bv * s * s * t + 3 * cv * s * t * t + dv * t ** 3 for av, bv, cv, dv in zip(a, b, c, d)))
        return out


class Scene:
    def __init__(self, code, group, title, centre, roads):
        self.code = code
        self.title = title
        # What the level's label and the preview show: code and group, then the title.
        self.caption = "{} {}\n{}".format(code, group, title)
        self.centre = centre
        self.roads = [road.moved(centre[0], centre[1]) for road in roads]

    def bounds(self):
        xs = [p[0] for road in self.roads for p in road.polyline()]
        ys = [p[1] for road in self.roads for p in road.polyline()]
        return min(xs), min(ys), max(xs), max(ys)


def main_road(label, points, prefab="Medium Road", **kw):
    return Road(label, prefab, points, **kw)


def street(label, points, prefab="Small Road", **kw):
    return Road(label, prefab, points, **kw)


def alley(label, points, **kw):
    return Road(label, "Alley", points, alley=True, **kw)


def alley_scenes():
    """Row A at y = 0: the mirrored pair must be mirrored about y = 0 to the bit, so the alley is
    exactly as near to both."""
    cases = [
        ("A1", "crossed by a main road", [alley("alley", [(-100, 0), (100, 0)]), main_road("main", [(0, 100), (0, -100)])]),
        ("A2", "crossed on a slant", [alley("alley", [(-100, 0), (100, 0)]), main_road("main", [(-60, 100), (60, -100)])]),
        ("A3", "main road along it", [alley("alley", [(-100, 0), (100, 0)]), main_road("main", [(-40, -3), (40, -3)])]),
        ("A4", "main road 10 m over it", [alley("alley", [(-100, 0), (100, 0)]), main_road("main", [(0, 100, 10), (0, -100, 10)])]),
        ("A5", "mirrored main roads (tie)", [alley("alley", [(100, 0), (0, 0)]), main_road("main north", [(0, 0), (100, -100)]),
                                                           main_road("main south", [(0, 0), (100, 100)])]),
        ("A6", "closed, covered at its start", [alley("ring", [(-40, -40), (40, -40), (40, 40), (-40, 40)], closed=True),
                                                        main_road("main", [(-40, -120), (-40, 120)])]),
        ("A7", "grid crossed diagonally", [alley("alley y{}".format(y), [(-90, y), (90, y)]) for y in (-60, 0, 60)]
         + [alley("alley x{}".format(x), [(x, -90), (x, 90)]) for x in (-60, 0, 60)] + [main_road("main", [(-110, 110), (110, -110)])]),
    ]
    return [Scene(code, "alley", title, (300.0 * i, 0.0), roads) for i, (code, title, roads) in enumerate(cases)]


def threshold_scenes():
    """Rows B at y = -400 and -800: what the spline network splits, merges, snaps and joins."""
    star = [street("arm {}".format(k), [(0, 0), (100 * math.cos(math.radians(45 * k)), 100 * math.sin(math.radians(45 * k)))]) for k in range(8)]
    shallow = 140.0 * math.tan(math.radians(10.0))
    ring = [(60 * math.cos(math.radians(a)), 60 * math.sin(math.radians(a))) for a in (0, 90, 180, 270)]
    cases = [
        ("B1", "ends 0.9 / 1.1 m apart", [street("west 0.9", [(-100, -30), (-0.45, -30)]), street("east 0.9", [(0.45, -30), (100, -30)]),
                                              street("west 1.1", [(-100, 30), (-0.55, 30)]), street("east 1.1", [(0.55, 30), (100, 30)])]),
        ("B2", "crossing 1.9 / 2.1 m higher", [street("raised 1.9", [(-140, -40, 1.9), (-10, -40, 1.9)]), street("ground 1.9", [(-75, -100), (-75, 20)]),
                                                             street("raised 2.1", [(10, 40, 2.1), (140, 40, 2.1)]), street("ground 2.1", [(75, -20), (75, 100)])]),
        ("B3", "ends 0.5 / 1.5 m short", [street("through", [(-100, 0), (100, 0)]), street("short 0.5", [(-40, -100), (-40, -0.5)]),
                                                        street("short 1.5", [(40, -100), (40, -1.5)])]),
        ("B4", "3 m segment, auto tangents", [street("short segment", [(-100, 0), (-97, 0), (30, 0), (100, -40)], smooth=True)]),
        ("B5", "hairpin", [street("hairpin", [(-100, 20), (40, 20), (70, 0), (40, -20), (-100, -20)], smooth=True)]),
        ("B6", "280 m straight", [street("straight", [(-140, 0), (140, 0)]), street("cross", [(0, -60), (0, 60)])]),
        ("B7", "crossing at 10 deg", [street("level", [(-140, 0), (140, 0)]), street("slanted", [(-140, shallow), (140, -shallow)])]),
        ("B8", "8 roads on one node", star),
        ("B9", "a spline and its copy", [street("original", [(-100, 0), (100, 0)]), street("copy", [(-100, 0), (100, 0)]), street("cross", [(0, -80), (0, 80)])]),
        ("B10", "ring and a link", [street("ring", ring, smooth=True, closed=True), street("link", [(-150, 0), (-60, 0)])]),
        ("B11", "parallel, 12 m apart", [main_road("north", [(-120, -6), (120, -6)]), main_road("south", [(-120, 6), (120, 6)])]),
        ("B12", "ramps at 37 %", [street("west", [(-140, 0), (-60, 0)]), street("up", [(-60, 0, 0), (-20, 0, 15)]), street("top", [(-20, 0, 15), (20, 0, 15)]),
                                  street("down", [(20, 0, 15), (60, 0, 0)]), street("east", [(60, 0), (140, 0)])]),
    ]
    return [Scene(code, "spline", title, (300.0 * (i % 6), -400.0 * (1 + i // 6)), roads) for i, (code, title, roads) in enumerate(cases)]


def tile_scenes():
    """Block C east of the rest, on the tile corners of a network at the origin (TILE metres; the tile
    row grows with -y)."""
    corner = -2 * TILE
    cases = [
        ("C1", "junction on a tile corner", (11 * TILE, corner),
         [street("arm east", [(0, 0), (100, 0)]), street("arm west", [(0, 0), (-100, 0)]), street("arm south", [(0, 0), (0, 100)]), street("arm north", [(0, 0), (0, -100)])]),
        ("C2", "middle on a tile corner", (13 * TILE, corner),
         [street("diagonal", [(-100, -100), (100, 100)]), street("side a", [(-50, -50), (-92, -8)]), street("side b", [(50, 50), (92, 8)])]),
        ("C3", "curve over tiles, border road", (13 * TILE, -3.5 * TILE),
         [street("curve", [(-700, 0), (-350, -60), (0, 60), (350, -60), (500, 0)], smooth=True), street("border", [(0, -150), (0, 150)])]),
        ("C4", "overpass on a tile corner", (15 * TILE, corner),
         [street("ground x", [(-100, 0), (100, 0)]), street("ground y", [(0, -100), (0, 100)]), main_road("overpass", [(-180, 180, 12), (180, -180, 12)])]),
    ]
    return [Scene(code, "tiles", title, centre, roads) for code, title, centre, roads in cases]


def golden_dir():
    if os.environ.get("CS2ROADGOLDEN"):
        return os.environ["CS2ROADGOLDEN"]
    if unreal:
        return os.path.join(unreal.Paths.project_saved_dir(), "CSRoadGolden")
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "..", "Saved", "CSRoadGolden")


def golden_scenes(missing):
    """Rows R at y = 400 and on: each edge of a golden scenario as a fixed spline, Unity (x, y, z)
    metres to UE (x, -z, y)."""
    scenes = []
    for i, scenario in enumerate(GOLDEN_BASE + GOLDEN_VARIANTS):
        path = os.path.join(golden_dir(), scenario + ".json")
        if not os.path.exists(path):
            missing.append(scenario)
            continue
        with open(path, encoding="utf-8") as f:
            edges = json.load(f)["edges"]
        roads = []
        for k, edge in enumerate(edges):
            c = edge["curve"]
            a, b, cc, d = [(c[j], -c[j + 2], c[j + 1]) for j in (0, 3, 6, 9)]
            leave = tuple(3.0 * (bv - av) for av, bv in zip(a, b))
            arrive = tuple(3.0 * (dv - cv) for cv, dv in zip(cc, d))
            roads.append(Road("e{} {}".format(k, edge["prefab"]), edge["prefab"], [a, d], tangents=[(leave, leave), (arrive, arrive)], adjust=False))
        name, _, variant = scenario.partition("@")
        title = name + ("\n" + variant.replace("_-_", " ").replace("_", " ") if variant else "")
        scenes.append(Scene("R{}".format(i + 1), "RoadLab", title, (450.0 * (i % 6), 400.0 * (1 + i // 6)), roads))
    return scenes


def all_scenes(missing):
    return alley_scenes() + threshold_scenes() + tile_scenes() + golden_scenes(missing)


def preview(path):
    """The layout from above, without the engine: roads (elevated ones blue, alleys orange), scene
    labels and the tile grid."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    missing = []
    scenes = all_scenes(missing)
    boxes = [scene.bounds() for scene in scenes]
    lo_x = min(b[0] for b in boxes) - 100
    lo_y = min(b[1] for b in boxes) - 100
    hi_x = max(b[2] for b in boxes) + 100
    hi_y = max(b[3] for b in boxes) + 100
    figure, axes = plt.subplots(figsize=(20, 20 * (hi_y - lo_y) / (hi_x - lo_x)), dpi=110)
    for k in range(math.floor(lo_x / TILE), math.ceil(hi_x / TILE) + 1):
        axes.axvline(k * TILE, color="#dddddd", linewidth=0.6, zorder=0)
    for k in range(math.floor(lo_y / TILE), math.ceil(hi_y / TILE) + 1):
        axes.axhline(k * TILE, color="#dddddd", linewidth=0.6, zorder=0)
    roads = 0
    for scene in scenes:
        for road in scene.roads:
            line = road.polyline()
            height = max(abs(p[2]) for p in line)
            colour = "#d97706" if road.alley else ("#2563eb" if height > 2.0 else "#222222")
            axes.plot([p[0] for p in line], [p[1] for p in line], color=colour, linewidth=1.2 if road.alley else 1.6, zorder=2)
            axes.plot([p[0] for p in road.points], [p[1] for p in road.points], "o", color=colour, markersize=1.8, zorder=3)
            roads += 1
        x0, y0, x1, y1 = scene.bounds()
        axes.text(scene.centre[0], y0 - 14, scene.caption, ha="center", va="bottom", fontsize=6.5, linespacing=1.1, zorder=4)
    # Overlapping scenes would build into each other.
    overlaps = []
    for i in range(len(scenes)):
        for j in range(i + 1, len(scenes)):
            a, b = boxes[i], boxes[j]
            if a[0] < b[2] + 40 and b[0] < a[2] + 40 and a[1] < b[3] + 40 and b[1] < a[3] + 40:
                overlaps.append((scenes[i].code, scenes[j].code))
    axes.set_xlim(lo_x, hi_x)
    axes.set_ylim(hi_y, lo_y)
    axes.set_aspect("equal")
    axes.set_title("L_RoadEdgeCases: {} scenes, {} spline roads (grey lines: {} m tiles; blue: above 2 m; orange: alleys)".format(len(scenes), roads, int(TILE)), fontsize=10)
    figure.tight_layout()
    figure.savefig(path)
    print("preview {}: {} scenes, {} roads, x {:.0f}..{:.0f} m, y {:.0f}..{:.0f} m".format(path, len(scenes), roads, lo_x, hi_x, lo_y, hi_y))
    print("missing golden scenarios: {}".format(", ".join(missing) or "none"))
    print("overlapping scenes: {}".format(", ".join("{}/{}".format(a, b) for a, b in overlaps) or "none"))


def log(text):
    unreal.log("CS2EDGECASES " + text)


def build():
    never = unreal.PropertyAccessChangeNotifyMode.NEVER
    keep = unreal.AttachmentRule.KEEP_WORLD
    assets = unreal.EditorAssetLibrary
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if assets.does_asset_exist(LEVEL):
        unreal.log_error("CS2EDGECASES {} exists; delete it to make it again".format(LEVEL))
        return
    missing = []
    scenes = all_scenes(missing)
    if missing:
        unreal.log_warning("CS2EDGECASES no golden data for {} (run Scripts/CS2Road/GoldenDump); left out".format(", ".join(missing)))
    if not levels.new_level_from_template(LEVEL, TEMPLATE):
        unreal.log_error("CS2EDGECASES could not create {} from {}".format(LEVEL, TEMPLATE))
        return

    # The template's floor gives way to a ground 0.25 m under the roads, over every scene.
    for actor in actors.get_all_level_actors():
        component = actor.get_component_by_class(unreal.StaticMeshComponent) if isinstance(actor, unreal.StaticMeshActor) else None
        mesh = component.get_editor_property("static_mesh") if component else None
        if mesh and mesh.get_name() == "SM_Template_Map_Floor":
            actors.destroy_actor(actor)
    boxes = [scene.bounds() for scene in scenes]
    lo_x, lo_y = min(b[0] for b in boxes) - 300.0, min(b[1] for b in boxes) - 300.0
    hi_x, hi_y = max(b[2] for b in boxes) + 300.0, max(b[3] for b in boxes) + 300.0
    ground = actors.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector((lo_x + hi_x) * 50.0, (lo_y + hi_y) * 50.0, -25.0))
    ground.set_actor_label("Ground")
    ground.set_actor_scale3d(unreal.Vector(hi_x - lo_x, hi_y - lo_y, 1.0))
    component = ground.get_component_by_class(unreal.StaticMeshComponent)
    component.set_editor_property("static_mesh", unreal.load_asset("/Engine/BasicShapes/Plane"))
    component.set_material(0, assets.load_asset(GROUND_MATERIAL))

    network = actors.spawn_actor_from_class(unreal.load_class(None, "/Script/CSRoadNet.CSRoadNetworkActor"), unreal.Vector(0.0, 0.0, 0.0))
    network.set_actor_label("CS2 Edge Cases")
    network.set_editor_property("local_update", True, notify_mode=never)
    road_class = unreal.load_class(None, "/Script/CSRoadNet.CSRoadSplineActor")
    local = unreal.SplineCoordinateSpace.LOCAL

    def centimetres(v):
        return unreal.Vector(v[0] * 100.0, v[1] * 100.0, v[2] * 100.0)

    for scene in scenes:
        for road in scene.roads:
            actor = actors.spawn_actor_from_class(road_class, unreal.Vector(0.0, 0.0, 0.0))
            actor.set_actor_label("{} {}".format(scene.code, road.label))
            actor.attach_to_actor(network, "", keep, keep, keep, False)
            spline = actor.get_editor_property("spline")
            spline.set_editor_property("prefab", road.prefab, notify_mode=never)
            spline.set_editor_property("adjust_segments", road.adjust, notify_mode=never)
            if road.alley:
                spline.set_editor_property("tier", unreal.CSRoadTier.ALLEY, notify_mode=never)
            spline.clear_spline_points(False)
            for point in road.points:
                spline.add_spline_point(centimetres(point), local, False)
            for i in range(len(road.points)):
                if road.tangents:
                    spline.set_tangents_at_spline_point(i, centimetres(road.tangents[i][0]), centimetres(road.tangents[i][1]), local, False)
                elif not road.smooth:
                    spline.set_spline_point_type(i, unreal.SplinePointType.LINEAR, False)
            spline.set_closed_loop(road.closed, False)
            spline.update_spline()
        # The scene's code and title, flat on the ground in front of it.
        try:
            x0, y0, x1, y1 = scene.bounds()
            text = actors.spawn_actor_from_class(unreal.TextRenderActor, unreal.Vector(scene.centre[0] * 100.0, (y0 - 25.0) * 100.0, 10.0), unreal.Rotator(roll=0.0, pitch=90.0, yaw=180.0))
            text.set_actor_label("Label {}".format(scene.code))
            render = text.get_editor_property("text_render")
            render.set_editor_property("text", unreal.Text(scene.caption))
            render.set_editor_property("world_size", 900.0)
            render.set_editor_property("horizontal_alignment", unreal.HorizTextAligment.EHTA_CENTER)
        except Exception as error:
            unreal.log_warning("CS2EDGECASES label of {}: {}".format(scene.code, error))

    network.rebuild()
    stats = {k: network.get_editor_property(k) for k in ("num_splines", "num_roads", "num_tiles", "pending_tiles", "num_triangles", "num_marking_instances", "last_error")}
    stats["missing_atlases"] = [str(a) for a in network.get_editor_property("missing_atlases")]
    stats["missing_marking_surfaces"] = [str(a) for a in network.get_editor_property("missing_marking_surfaces")]
    log("{} scenes; build {}".format(len(scenes), json.dumps(stats)))
    if not levels.save_current_level():
        unreal.log_error("CS2EDGECASES could not save " + LEVEL)
        return
    log("saved " + LEVEL)


if unreal:
    build()
elif "--preview" in sys.argv:
    preview(sys.argv[sys.argv.index("--preview") + 1])
else:
    print(__doc__)
