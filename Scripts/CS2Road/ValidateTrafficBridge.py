"""Run in a separate editor on Template_Default; leaves the map unsaved.

UnrealEditor.exe <project> /Engine/Maps/Templates/Template_Default
  -ExecCmds="py <this file>" -unattended -RenderOffscreen
  "-ini:EditorSettings:[/Script/UnrealEd.EditorPerformanceSettings]:bThrottleCPUWhenNotForeground=False"

Writes Saved/CodexTrafficBridge/editor-validation.json and four rendered views.
Requires the updated editor DLLs. A missing/failed report is NOT a passing test.
"""
import json
import os
import time
import traceback

import unreal

OUT = os.path.join(unreal.Paths.project_saved_dir(), "CodexTrafficBridge")
os.makedirs(OUT, exist_ok=True)
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
NEVER = unreal.PropertyAccessChangeNotifyMode.NEVER
KEEP = unreal.AttachmentRule.KEEP_WORLD
REPORT = {"status": "running", "checks": []}
STATE = {"handle": None, "phase": 0, "started": time.monotonic()}
CREATED = []


def check(condition, message):
    if not condition:
        raise AssertionError(message)
    REPORT["checks"].append(message)


def finish(error=None):
    if STATE["handle"] is not None:
        unreal.unregister_slate_post_tick_callback(STATE["handle"])
    REPORT["status"] = "failed" if error else "passed"
    if error:
        REPORT["error"] = str(error)
        unreal.log_error("CS2TRAFFICBRIDGE " + str(error))
    with open(os.path.join(OUT, "editor-validation.json"), "w", encoding="utf-8") as stream:
        json.dump(REPORT, stream, indent=2, ensure_ascii=False)
    for actor in reversed(CREATED):
        ACTORS.destroy_actor(actor)
    unreal.log("CS2TRAFFICBRIDGE " + REPORT["status"])
    unreal.SystemLibrary.quit_editor()


def set_property(obj, name, value):
    obj.set_editor_property(name, value, notify_mode=NEVER)


def setup():
    network = ACTORS.spawn_actor_from_class(unreal.load_class(None, "/Script/CSRoadNet.CSRoadNetworkActor"), unreal.Vector(12000, -7000, 200))
    CREATED.append(network)
    STATE["network"] = network
    set_property(network, "source", unreal.CSRoadNetworkSource.SPLINES)
    set_property(network, "build_markings", False)
    set_property(network, "show_traffic", False)
    set_property(network, "trace_bridge_ground", False)
    set_property(network, "traffic_vehicle_count", 32)
    set_property(network, "traffic_in_editor", False)
    ground = ACTORS.spawn_actor_from_class(unreal.StaticMeshActor, network.get_actor_location() + unreal.Vector(0, 0, -100))
    CREATED.append(ground)
    ground.static_mesh_component.set_static_mesh(unreal.load_asset("/Engine/BasicShapes/Cube"))
    ground.set_actor_scale3d(unreal.Vector(500, 500, 1.5))
    road_class = unreal.load_class(None, "/Script/CSRoadNet.CSRoadSplineActor")
    for label, prefab, points in [
        ("QA Main", "Medium Road", [(-16000, 0, 0), (16000, 0, 0)]),
        ("QA Cross", "Small Road", [(0, -14000, 0), (0, 14000, 0)]),
        ("QA Viaduct", "Highway Twoway - 2 lanes", [(-8000, -16000, 1200), (-8000, 0, 1400), (-6000, 16000, 1800)]),
    ]:
        actor = ACTORS.spawn_actor_from_class(road_class, network.get_actor_location())
        CREATED.append(actor)
        actor.set_actor_label(label)
        actor.attach_to_actor(network, "", KEEP, KEEP, KEEP, False)
        spline = actor.get_editor_property("spline")
        set_property(spline, "prefab", prefab)
        spline.clear_spline_points(False)
        for point in points:
            spline.add_spline_point(unreal.Vector(*point), unreal.SplineCoordinateSpace.LOCAL, False)
        spline.update_spline()
    network.rebuild()
    check(not network.get_editor_property("last_error"), "road build succeeds without markings")
    check(network.get_editor_property("num_traffic_lanes") > 0, "lane graph exists while traffic and markings are hidden")
    check(network.get_editor_property("num_bridge_piers") > 0, "elevated road generates piers")
    piers = network.get_editor_property("bridge_piers")
    check(piers.get_instance_count() == network.get_editor_property("num_bridge_piers") * 3, "each pier has column, cap and footing")
    network.set_editor_property("show_traffic", True)
    bodies = network.get_editor_property("traffic_bodies")
    STATE["bodies"] = bodies
    check(0 < bodies.get_instance_count() <= 32, "show switch creates a bounded car population")
    initial = bodies.get_instance_transform(0, False).translation
    network.reset_traffic()
    check(initial == bodies.get_instance_transform(0, False).translation, "reset with same seed reproduces positions")
    STATE["initial"] = initial
    set_property(network, "traffic_in_editor", True)
    STATE["started"] = time.monotonic()


def tick(delta):
    try:
        network, bodies = STATE["network"], STATE["bodies"]
        if STATE["phase"] == 3:
            capture_view()
            return
        elapsed = time.monotonic() - STATE["started"]
        if elapsed < 3:
            return
        if STATE["phase"] == 0:
            check(STATE["initial"] != bodies.get_instance_transform(0, False).translation, "cars advance in editor viewport ticks")
            STATE["paused"] = bodies.get_instance_transform(0, False).translation
            STATE["road_triangles"] = network.get_editor_property("num_triangles")
            network.set_editor_property("show_traffic", False)
            check(not bodies.is_visible(), "show switch hides cars immediately")
            STATE["phase"] = 1
        elif STATE["phase"] == 1:
            check(STATE["paused"] == bodies.get_instance_transform(0, False).translation, "hidden traffic remains paused")
            check(STATE["road_triangles"] == network.get_editor_property("num_triangles"), "traffic toggle preserves road geometry")
            network.set_editor_property("show_traffic", True)
            STATE["phase"] = 2
        else:
            check(STATE["paused"] != bodies.get_instance_transform(0, False).translation, "show switch resumes traffic")
            set_property(network, "build_bridge_piers", False)
            network.rebuild()
            check(network.get_editor_property("bridge_piers").get_instance_count() == 0, "bridge switch clears old instances")
            set_property(network, "build_bridge_piers", True)
            set_property(network, "build_markings", True)
            network.rebuild()
            check(not network.get_editor_property("last_error"), "rebuild with markings succeeds")
            check(network.get_editor_property("num_traffic_vehicles") > 0, "traffic recovers after road rebuild")
            set_property(network, "trace_bridge_ground", True)
            network.rebuild()
            piers = network.get_editor_property("bridge_piers")
            check(piers.get_instance_count() > 0, "ground traces retain elevated supports")
            for index in range(2, piers.get_instance_count(), 3):
                # The 1.5 m ground cube is centered 1 m below the actor: top = -0.25 m.
                # Footing center sits 0.1 m below that surface, in actor-local space.
                check(abs(piers.get_instance_transform(index, False).translation.z + 35) < 0.5, "footing samples the visible static ground")
            triangle_count = network.get_editor_property("num_triangles")
            network.set_editor_property("traffic_vehicle_mesh", unreal.load_asset("/Engine/BasicShapes/Cube"))
            check(network.get_editor_property("traffic_cabins").get_instance_count() == 0, "custom vehicle mesh replaces the default cabin proxy")
            network.set_editor_property("traffic_vehicle_mesh", None)
            check(network.get_editor_property("traffic_cabins").get_instance_count() == bodies.get_instance_count(), "clearing custom mesh restores default vehicle proxy")
            check(network.get_editor_property("num_triangles") == triangle_count, "vehicle mesh changes preserve road geometry")
            REPORT["stats"] = {key: network.get_editor_property(key) for key in (
                "num_roads", "num_traffic_lanes", "num_traffic_vehicles", "num_bridge_piers", "skipped_bridge_piers", "unsupported_traffic_roundabouts")}
            set_property(network, "traffic_in_editor", False)
            world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
            capture = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, unreal.Vector(-9000, -26000, 17000))
            CREATED.append(capture)
            component = capture.get_component_by_class(unreal.SceneCaptureComponent2D)
            rt = unreal.RenderingLibrary.create_render_target2d(world, 1600, 900, unreal.TextureRenderTargetFormat.RTF_RGBA8)
            component.set_editor_property("texture_target", rt)
            component.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
            component.set_editor_property("capture_every_frame", False)
            component.set_editor_property("capture_on_movement", False)
            component.set_editor_property("always_persist_rendering_state", True)
            STATE.update(phase=3, capture=capture, component=component, rt=rt, view=0, frames=0)
            REPORT["shots"] = []
            begin_view()
            return
        STATE["started"] = time.monotonic()
    except Exception:
        finish(traceback.format_exc())


VIEWS = [
    ("overview", (-21000, -19000, 16800), (0, 0, 0), True),
    ("bridge_piers", (-13000, -5000, 3200), (-8000, 0, 700), True),
    ("traffic_on", (-4500, -4500, 4200), (0, 0, 0), True),
    ("traffic_off", (-4500, -4500, 4200), (0, 0, 0), False),
]


def begin_view():
    name, position, target, traffic = VIEWS[STATE["view"]]
    network = STATE["network"]
    network.set_editor_property("show_traffic", traffic)
    origin = network.get_actor_location()
    position, target = origin + unreal.Vector(*position), origin + unreal.Vector(*target)
    STATE["capture"].set_actor_location_and_rotation(position, unreal.MathLibrary.find_look_at_rotation(position, target), False, True)
    STATE["frames"] = 0


def capture_view():
    STATE["component"].capture_scene()
    STATE["frames"] += 1
    if STATE["frames"] < 48:
        return
    name = VIEWS[STATE["view"]][0]
    unreal.RenderingLibrary.export_render_target(unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world(), STATE["rt"], OUT, name + ".png")
    REPORT["shots"].append(name + ".png")
    STATE["view"] += 1
    if STATE["view"] == len(VIEWS):
        finish()
    else:
        begin_view()


try:
    setup()
    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
except Exception:
    finish(traceback.format_exc())
