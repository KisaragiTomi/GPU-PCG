# -*- coding: utf-8 -*-
"""Shots of L_NCBuildingTypes (MakeNCSimpleModules.py): round and cut-off corners on each footprint and stacking, drums,
every overhang, a street of overhangs, a street-level view of an overhang on its columns, and a street on a slope.

  UnrealEditor.exe <project> /PCGPlugins/CSRoadNet/Houses/NCSimple/L_NCBuildingTypes -ExecCmds="py <this file>"

Needs the real editor (in UnrealEditor-Cmd the captures come out black). Nothing is saved.

Output: Saved/NCBuildingTypesShots, <view>.png and summary.json (each building's massing and drawn modules per slot).
"""
import json
import os

import unreal

OUT_DIR = os.path.join(unreal.Paths.project_saved_dir(), "NCBuildingTypesShots")
W, H = 1600, 900
WARMUP = 48
SLOTS = ("Wall", "Window", "Door", "Corner", "Parapet", "Roof", "CornerInner", "GroundWall", "GroundWindow", "RoofCorner", "ParapetCorner", "Soffit", "Column")
ROW, COLUMN = 6000.0, 5000.0

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
actors = [a for a in ACTORS.get_all_level_actors() if a.get_class().get_name() in ("CSCityBuildingActor", "CSCityStreetActor")]
# Rebuilt as the editor's property change does (Rebuild is not exposed to Python).
for building in actors:
    if building.get_class().get_name() == "CSCityBuildingActor":
        building.set_editor_property("height", building.get_editor_property("height"), notify_mode=unreal.PropertyAccessChangeNotifyMode.ALWAYS)
# The sun from the cameras' side (they look along +X and +Y), so that the faces towards them are lit; more sky, so that the
# undersides of the overhangs can be read. Nothing is saved.
for actor in ACTORS.get_all_level_actors():
    if isinstance(actor, unreal.DirectionalLight):
        actor.set_actor_rotation(unreal.Rotator(roll=0.0, pitch=-35.0, yaw=62.0), False)
    if isinstance(actor, unreal.SkyLight):
        actor.get_component_by_class(unreal.SkyLightComponent).set_intensity(3.0)


def row_views(name, row, height=9000.0):
    """A whole row from above at an angle, from its -Y side and its +Y side."""
    y = row * ROW
    return [
        (name + "_south", unreal.Vector(12500.0, y - 7000.0, height), unreal.Vector(12500.0, y, 800.0), 80.0),
        (name + "_north", unreal.Vector(12500.0, y + 7000.0, height), unreal.Vector(12500.0, y, 800.0), 80.0),
    ]


def close(name, row, column, dx=-2400.0, dy=-3000.0, dz=900.0, tz=900.0, fov=65.0):
    """One building close up, at about eye height or above."""
    x, y = column * COLUMN, row * ROW
    return (name, unreal.Vector(x + dx, y + dy, dz), unreal.Vector(x, y, tz), fov)


views = [("overview", unreal.Vector(-11000.0, -12000.0, 19000.0), unreal.Vector(12500.0, 18000.0, 0.0), 65.0)]
views += row_views("round", 0) + row_views("round_large", 1) + row_views("chamfer", 2) + row_views("flat", 3) + row_views("round_overhang", 4)
views += [
    close("round_box_top", 0, 0, dx=-1500.0, dy=-1500.0, dz=3000.0, tz=1200.0, fov=70.0),
    close("round_tower", 0, 5, dx=-4000.0, dy=-4000.0, dz=3000.0, tz=2200.0, fov=70.0),
    close("large_drum", 1, 2, dx=-2500.0, dy=-2500.0, dz=1800.0, tz=2000.0, fov=80.0),
    close("chamfer_box_top", 2, 0, dx=-1500.0, dy=-1500.0, dz=3000.0, tz=1200.0, fov=70.0),
    # Under the overhangs: from the street, then looking up at the soffit.
    close("flat_front_eye", 3, 0, dx=-2600.0, dy=-3200.0, dz=170.0, tz=700.0, fov=70.0),
    close("flat_front_under", 3, 0, dx=-1200.0, dy=-1700.0, dz=160.0, tz=500.0, fov=80.0),
    close("flat_sides_eye", 3, 1, dx=-3200.0, dy=-3000.0, dz=170.0, tz=700.0, fov=70.0),
    close("flat_all_eye", 3, 3, dx=-3200.0, dy=-3200.0, dz=170.0, tz=700.0, fov=70.0),
    close("round_overhang_eye", 4, 2, dx=-2400.0, dy=-2600.0, dz=170.0, tz=700.0, fov=70.0),
    # The big blocks: as the street sees them, from the sidewalk, and under the soffit.
    close("plaza_flat_street", 5, 0, dx=-1500.0, dy=-5200.0, dz=170.0, tz=1000.0, fov=75.0),
    close("plaza_flat_corner", 5, 0, dx=-5200.0, dy=-3900.0, dz=170.0, tz=900.0, fov=70.0),
    close("plaza_flat_under", 5, 0, dx=-1800.0, dy=-2150.0, dz=170.0, tz=700.0, fov=90.0),
    ("street_eye", unreal.Vector(-1500.0, 36000.0, 300.0), unreal.Vector(7000.0, 36000.0, 900.0), 75.0),
    ("street_top", unreal.Vector(7500.0, 36000.0, 20000.0), unreal.Vector(7500.0, 36001.0, 0.0), 60.0),
    # The street on a slope, from down the hill along it, and side on: the buildings step up, none of them leans.
    ("slope_along", unreal.Vector(-3000.0, 42000.0, 500.0), unreal.Vector(7500.0, 42000.0, 1000.0), 70.0),
    ("slope_side", unreal.Vector(7500.0, 42000.0 - 9000.0, 900.0), unreal.Vector(7500.0, 42000.0, 700.0), 80.0),
]

rt = unreal.RenderingLibrary.create_render_target2d(world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8, unreal.LinearColor(0, 0, 0, 1), False)
capture = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, unreal.Vector(0, 0, 0))
comp = capture.get_component_by_class(unreal.SceneCaptureComponent2D)
comp.set_editor_property("texture_target", rt)
comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
comp.set_editor_property("capture_every_frame", False)
comp.set_editor_property("capture_on_movement", False)
# Without a view state the level's post-process exposure does not apply and Lumen never runs.
comp.set_editor_property("always_persist_rendering_state", True)

STATE = {"ticks": 0, "view": -1, "frames": 0, "handle": None, "done": []}


def modules():
    """Each building's drawn modules per slot, read back from the GPU (blocking)."""
    out = {}
    for building in actors:
        counts = {}
        for slot, component in zip(SLOTS, building.get_editor_property("facade")):
            if component:
                counts[slot] = component.debug_read_drawn_instance_count_sync()
        out[building.get_actor_label()] = counts
    return out


def begin(index):
    name, location, target, fov = views[index]
    comp.set_editor_property("fov_angle", fov)
    capture.set_actor_location_and_rotation(location, unreal.MathLibrary.find_look_at_rotation(location, target), False, True)
    STATE["view"] = index
    STATE["frames"] = 0


def finish():
    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    os.makedirs(OUT_DIR, exist_ok=True)
    drawn = modules()
    with open(os.path.join(OUT_DIR, "summary.json"), "w", encoding="utf-8") as f:
        json.dump({"modules": drawn, "shots": STATE["done"]}, f, indent=2, ensure_ascii=False)
    empty = [label for label, counts in drawn.items() if sum(counts.values()) == 0]
    unreal.log("NCTYPESSHOT {} buildings, {} drew nothing: {}".format(len(drawn), len(empty), empty))
    ACTORS.destroy_actor(capture)
    unreal.log("NCTYPESSHOT done")
    unreal.SystemLibrary.quit_editor()


def tick(delta):
    STATE["ticks"] += 1
    # Let streaming, the proxies, the facade writes and the first Lumen frames settle before the first shot.
    if STATE["ticks"] < 60:
        return
    if STATE["view"] < 0:
        begin(0)
    comp.capture_scene()
    STATE["frames"] += 1
    if STATE["frames"] < WARMUP:
        return
    name = views[STATE["view"]][0]
    unreal.RenderingLibrary.export_render_target(world, rt, OUT_DIR, name + ".png")
    STATE["done"].append(name)
    unreal.log("NCTYPESSHOT shot " + name)
    if STATE["view"] + 1 < len(views):
        begin(STATE["view"] + 1)
    else:
        finish()


STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
