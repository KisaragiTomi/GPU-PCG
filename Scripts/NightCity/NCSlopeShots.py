# -*- coding: utf-8 -*-
"""Shots of the small levels (MakeNCSimpleModules.py), whichever is open: L_NCSlopeStreets and L_NCSlopeRoad, the buildings level
beside the slope with a plinth, a platform and steps (GroundJoin_Plan.md 4, 11.1); L_NCFieldTest, a street under the city's field.

  UnrealEditor.exe <project> /PCGPlugins/CSRoadNet/Houses/NCSimple/L_NCSlopeStreets -ExecCmds="py <this file>"
  UnrealEditor.exe <project> /PCGPlugins/CSRoadNet/Houses/NCSimple/L_NCSlopeRoad -ExecCmds="py <this file>"
  UnrealEditor.exe <project> /PCGPlugins/CSRoadNet/Houses/NCSimple/L_NCFieldTest -ExecCmds="py <this file>"

Needs the real editor (in UnrealEditor-Cmd the captures come out black). Nothing is saved.

Output: Saved/NCSlopeShots/<level>, <view>.png and summary.json (each actor's drawn modules per slot).
"""
import json
import os

import unreal

W, H = 1600, 900
WARMUP = 48
SLOTS = ("Wall", "Window", "Door", "Corner", "Parapet", "Roof", "CornerInner", "GroundWall", "GroundWindow", "RoofCorner", "ParapetCorner", "Soffit", "Column", "Plinth", "PlinthCorner", "Platform", "Stair")
SLOTS += tuple(style + piece for style in ("Square", "Chamfer", "Round", "RoundLarge") for piece in ("Corner", "Roof", "Parapet", "Plinth"))

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
LEVEL = world.get_name()
OUT_DIR = os.path.join(unreal.Paths.project_saved_dir(), "NCSlopeShots", LEVEL)
streets = [a for a in ACTORS.get_all_level_actors() if a.get_class().get_name() == "CSCityStreetActor"]
networks = [a for a in ACTORS.get_all_level_actors() if a.get_class().get_name() == "CSRoadNetworkActor"]
for network in networks:
    # The network does not tick in a background editor.
    network.set_editor_property("min_free_video_memory", 0)
    # All of it in one go: the ground waits for the buildings.
    network.set_editor_property("max_tiles_per_batch", 0)
    network.rebuild()
# The sun from the cameras' side, more sky, so that the shaded sides can be read. Nothing is saved.
for actor in ACTORS.get_all_level_actors():
    if isinstance(actor, unreal.DirectionalLight):
        actor.set_actor_rotation(unreal.Rotator(roll=0.0, pitch=-35.0, yaw=62.0), False)
    if isinstance(actor, unreal.SkyLight):
        actor.get_component_by_class(unreal.SkyLightComponent).set_intensity(3.0)


def street_views(name, y, rise, low_x=3000.0, high_x=9000.0):
    """A street at y (cm), rising rise (cm) over 120 m: from above, along it from the sidewalk at both ends, and one building's
    front from across the street."""
    z = lambda x: rise * x / 12000.0
    side = y - 1200.0   # the sidewalk of the street's -Y side
    return [
        (name + "_top", unreal.Vector(6000.0, y - 7000.0, 5500.0), unreal.Vector(6000.0, y, 300.0), 70.0),
        (name + "_uphill", unreal.Vector(-800.0, y - 400.0, z(0.0) + 220.0), unreal.Vector(4500.0, side - 1200.0, z(4500.0) + 200.0), 60.0),
        (name + "_downhill", unreal.Vector(12800.0, y - 400.0, z(12000.0) + 300.0), unreal.Vector(7500.0, side - 1200.0, z(7500.0) + 100.0), 60.0),
        (name + "_front", unreal.Vector(low_x, y + 300.0, z(low_x) + 900.0), unreal.Vector(low_x, side - 1400.0, z(low_x) + 100.0), 80.0),
        (name + "_front2", unreal.Vector(high_x, y + 300.0, z(high_x) + 900.0), unreal.Vector(high_x, side - 1400.0, z(high_x) + 100.0), 80.0),
        (name + "_close", unreal.Vector(low_x - 1100.0, side + 250.0, z(low_x) + 180.0), unreal.Vector(low_x + 400.0, side - 900.0, z(low_x) + 150.0), 70.0),
    ]


def field_views():
    """The street along X, stretch by stretch: from above, and each stretch from the sidewalk on its -Y side."""
    out = [("overview", unreal.Vector(10000.0, -11000.0, 9000.0), unreal.Vector(10000.0, 0.0, 500.0), 75.0)]
    for name, x in (("overhang", 2500.0), ("round", 7500.0), ("chamfer", 12500.0), ("plain", 17500.0)):
        out.append((name + "_top", unreal.Vector(x, -5000.0, 4500.0), unreal.Vector(x, 0.0, 700.0), 70.0))
        out.append((name + "_north", unreal.Vector(x, -500.0, 300.0), unreal.Vector(x, 1400.0, 800.0), 90.0))
        out.append((name + "_south", unreal.Vector(x, 500.0, 300.0), unreal.Vector(x, -1400.0, 800.0), 90.0))
    return out


def blocks_views():
    """Four blocks 100 m across on a hill: from above, each block's free ground from above and from its street, then the field's maps."""
    out = [("overview", unreal.Vector(-6000.0, -8000.0, 12000.0), unreal.Vector(10000.0, 10000.0, 200.0), 65.0),
           ("overview_ne", unreal.Vector(27000.0, 27000.0, 9000.0), unreal.Vector(10000.0, 10000.0, 600.0), 65.0),
           ("top", unreal.Vector(10000.0, 10000.0, 26000.0), unreal.Vector(10000.0, 10001.0, 0.0), 60.0)]
    for name, x, y, z in (("planter", 5000.0, 5000.0, 300.0), ("market", 15000.0, 5000.0, 400.0), ("mixed", 5000.0, 15000.0, 700.0), ("empty", 15000.0, 15000.0, 800.0)):
        out.append((name + "_top", unreal.Vector(x, y - 3500.0, z + 4000.0), unreal.Vector(x, y, z), 70.0))
        out.append((name + "_yard", unreal.Vector(x - 2200.0, y - 2200.0, z + 500.0), unreal.Vector(x + 800.0, y + 800.0, z), 75.0))
    # A vacant plot from the street in the market, and the seam where the middle of the third block is raised.
    out.append(("market_street", unreal.Vector(15000.0, 1100.0, 330.0), unreal.Vector(15000.0, 3000.0, 400.0), 75.0))
    out.append(("mixed_raised", unreal.Vector(5000.0, 9500.0, 4500.0), unreal.Vector(5000.0, 15000.0, 900.0), 60.0))
    # Both sides of the avenue at x = 100 m, 9 m up there: where the ground meets its edge.
    out.append(("edge_east", unreal.Vector(10200.0, 15000.0, 1250.0), unreal.Vector(11400.0, 15200.0, 900.0), 60.0))
    out.append(("edge_west", unreal.Vector(9800.0, 15000.0, 1250.0), unreal.Vector(8600.0, 15200.0, 900.0), 60.0))
    out += [("map_gap", unreal.Vector(10000.0, 10000.0, 26000.0), unreal.Vector(10000.0, 10001.0, 0.0), 60.0),
            ("map_elevation", unreal.Vector(10000.0, 10000.0, 26000.0), unreal.Vector(10000.0, 10001.0, 0.0), 60.0)]
    return out


# A view named so shows the field's map of that channel.
MAP_VIEWS = {"map_gap": "GAP_STYLE", "map_elevation": "ELEVATION", "map_corner": "CORNER_STYLE", "map_overhang": "OVERHANG"}

if LEVEL == "L_NCFieldTest":
    views = field_views()
elif LEVEL == "L_NCBlocksHill":
    views = blocks_views()
elif LEVEL == "L_NCSlopeStreets":
    views = [("overview", unreal.Vector(-5000.0, -9000.0, 11000.0), unreal.Vector(6000.0, 13000.0, 0.0), 65.0)]
    views += street_views("round", 0.0, 480.0) + street_views("pilotis", 13000.0, 480.0) + street_views("chamfer", 26000.0, 960.0)
else:
    views = [("overview", unreal.Vector(-5000.0, -6000.0, 9000.0), unreal.Vector(6000.0, 6500.0, 0.0), 65.0)]
    views += street_views("batch", 0.0, 600.0) + street_views("hung", 13000.0, 600.0)

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
    """Each street's drawn modules per slot, read back from the GPU (blocking)."""
    out = {}
    for street in streets:
        counts = {}
        for slot, component in zip(SLOTS, street.get_editor_property("facade")):
            if component:
                counts[slot] = component.debug_read_drawn_instance_count_sync()
        out[street.get_actor_label()] = {"status": street.get_editor_property("status"), "buildings": street.get_editor_property("num_buildings"), "modules": counts, "plots": list(street.get_editor_property("plots"))}
    for network in networks:
        ground = {}
        content = network.get_content()
        for component in content.get_components_by_class(unreal.CSGpuInstancedMeshComponent) if content else []:
            if component.get_name().startswith("GroundTile"):
                mesh = component.get_editor_property("base_mesh")
                key = mesh.get_name() if mesh else "?"
                ground[key] = ground.get(key, 0) + component.debug_read_drawn_instance_count_sync()
        out[network.get_actor_label()] = {"buildings": network.get_editor_property("num_buildings"), "ground_tiles": network.get_editor_property("num_ground_tiles"), "ground": ground, "error": network.get_editor_property("last_error")}
    return out


def show_map(channel):
    """The field's map over the shots: a capture draws as a game view, so hidden-in-game has to go too."""
    for actor in ACTORS.get_all_level_actors():
        if actor.get_class().get_name() != "CSCityFieldActor":
            continue
        field_map = actor.get_editor_property("map")
        field_map.set_editor_property("always_show", True)
        actor.set_editor_property("brush_channel", getattr(unreal.CSCityFieldChannel, channel), notify_mode=unreal.PropertyAccessChangeNotifyMode.ALWAYS)
        field_map.set_hidden_in_game(False)
        # Its proxy made again, with the channel and the flag it was given.
        field_map.set_visibility(False)
        field_map.set_visibility(True)


def begin(index):
    name, location, target, fov = views[index]
    if name in MAP_VIEWS:
        show_map(MAP_VIEWS[name])
    comp.set_editor_property("fov_angle", fov)
    capture.set_actor_location_and_rotation(location, unreal.MathLibrary.find_look_at_rotation(location, target), False, True)
    STATE["view"] = index
    STATE["frames"] = 0


def finish():
    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    os.makedirs(OUT_DIR, exist_ok=True)
    drawn = modules()
    with open(os.path.join(OUT_DIR, "summary.json"), "w", encoding="utf-8") as f:
        json.dump({"actors": drawn, "shots": STATE["done"]}, f, indent=2, ensure_ascii=False)
    unreal.log("NCSLOPESHOT " + json.dumps(drawn))
    ACTORS.destroy_actor(capture)
    unreal.log("NCSLOPESHOT done")
    unreal.SystemLibrary.quit_editor()


def tick(delta):
    # Anything going wrong ends it: a callback that raises every tick never quits the editor.
    try:
        step()
    except Exception as error:
        unreal.log_error("NCSLOPESHOT failed: {}".format(error))
        unreal.unregister_slate_post_tick_callback(STATE["handle"])
        unreal.SystemLibrary.quit_editor()


def step():
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
    unreal.log("NCSLOPESHOT shot " + name)
    if STATE["view"] + 1 < len(views):
        begin(STATE["view"] + 1)
    else:
        finish()


STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
