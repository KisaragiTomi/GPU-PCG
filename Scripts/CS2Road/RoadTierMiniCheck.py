# -*- coding: utf-8 -*-
"""Checks L_RoadTierMini (MakeRoadTierMini.py): each main road moved over the small roads and back, and the house with a
garage moved and its garage turned away and back.

  Open /PCGPlugins/CSRoadNet/L_RoadTierMini in the editor, then run this file (Python console: py <this file>).

Waits until the network settles (no pending tile, every tile baked, the road and building counts unchanged for 30
ticks) and shoots it from above. Then per edit: moves a main road as a drag would (UpdateSpline tells the network,
which updates the tiles near it), or the house or its garage (a property change tells it), waits, shoots, checks the
garage is joined or not as the edit has it, moves it back, waits, and checks the road and building counts are as they
were. Nothing is saved and the editor stays open.

Output: Saved/CS2RoadTierMini, <shot>.png and summary.txt. Logs CS2ROADTIERMINI VERDICT OK or FAILED at the end.
"""
import json
import os
import traceback

import unreal

OUT = os.path.join(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir()), "CS2RoadTierMini")
W, H = 1600, 900
# Lumen keeps history across frames: one capture per frame until it settles.
WARMUP = 40
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
level_actors = ACTORS.get_all_level_actors()
network = next(a for a in level_actors if a.get_class().get_name() == "CSRoadNetworkActor")
ROADS = {a.get_actor_label(): a for a in level_actors if a.get_class().get_name() == "CSRoadSplineActor"}
HOUSE = next(a for a in level_actors if a.get_class().get_name() == "CSCityBuildingActor" and a.get_actor_label() == "H House with garage")
LOCAL = unreal.SplineCoordinateSpace.LOCAL
ALWAYS = unreal.PropertyAccessChangeNotifyMode.ALWAYS
os.makedirs(OUT, exist_ok=True)
log = []
failed = []

# (name, what, road, new points in m or None, offset in m, garages joined while moved). What: a main road, the house
# (moved by the offset) or its garage (turned to the side facing away from A). The offsets stay short of covering the
# parallel small road (a main road reaches 21 m from a small road's centre line): that is the other case, a road covered
# along its length. A dragged in leaves nothing in front of the garage. Turned away from A, the garage faces the last
# alley 23 m off and no main road: it joins the alley, which leads on to B and C (GARAGE_VIA is what its status says).
EDITS = [
    ("a_in", "road", "A Drag me", [(0.0, 120.0), (100.0, 120.0), (200.0, 120.0)], None, 0),
    ("b_east", "road", "B Drawn", None, (10.0, 0.0), 1),
    ("c_west", "road", "C Crossing", None, (-10.0, 0.0), 1),
    ("house_east", "house", None, None, (25.0, 0.0), 1),
    ("garage_back", "garage", None, None, None, 1),
]
GARAGE_VIA = {"garage_back": "(no main road within 50 m)"}


def stats():
    keys = ("num_roads", "num_tiles", "pending_tiles", "num_nanite_tiles", "num_buildings", "num_garages", "num_joined_garages", "last_error")
    return {k: network.get_editor_property(k) for k in keys}


def house_place():
    # A copy: the struct get_editor_property returns follows the property as it changes.
    garage = HOUSE.get_editor_property("garage_location")
    return HOUSE.get_actor_location(), unreal.Vector(garage.x, garage.y, garage.z)


def set_house(place):
    # Moved by a script, the house hears of it through a property change (a drag in the editor calls PostEditMove).
    HOUSE.set_actor_location(place[0], False, False)
    HOUSE.set_editor_property("garage_location", place[1], notify_mode=ALWAYS)


def points(road):
    spline = ROADS[road].get_editor_property("spline")
    return [spline.get_location_at_spline_point(i, LOCAL) for i in range(spline.get_number_of_spline_points())]


def set_points(road, values):
    spline = ROADS[road].get_editor_property("spline")
    for i, p in enumerate(values):
        spline.set_location_at_spline_point(i, p, LOCAL, False)
    # UCSRoadSplineComponent::UpdateSpline tells the network, as a drag in the editor does: the local update path.
    spline.update_spline()


rt = unreal.RenderingLibrary.create_render_target2d(world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8, unreal.LinearColor(0, 0, 0, 1), False)
capture = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, unreal.Vector(0, 0, 100000), unreal.Rotator(0, 0, 0), transient=True)
comp = capture.get_component_by_class(unreal.SceneCaptureComponent2D)
comp.set_editor_property("texture_target", rt)
comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
comp.set_editor_property("fov_angle", 50.0)
comp.set_editor_property("capture_every_frame", False)
comp.set_editor_property("capture_on_movement", False)
comp.set_editor_property("always_persist_rendering_state", True)
# The whole level from above: up is +X, right is +Y.
TOP = (unreal.Vector(8000.0, 9500.0, 46000.0), unreal.Rotator(roll=0.0, pitch=-89.9, yaw=0.0))
STATE = {"handle": None, "phase": "settle", "next": "base", "stable": 0, "last": None, "ticks": 0, "frames": 0, "shot": None,
         "edit": -1, "saved": None, "base": None}


def finish(error=""):
    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    ACTORS.destroy_actor(capture)
    if error:
        failed.append(error)
    verdict = "FAILED: " + "; ".join(failed) if failed else "OK"
    with open(os.path.join(OUT, "summary.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(log + ["verdict " + verdict]) + "\n")
    unreal.log("CS2ROADTIERMINI VERDICT " + verdict)


def settled():
    s = stats()
    key = (s["num_roads"], s["num_buildings"], s["pending_tiles"])
    STATE["stable"] = STATE["stable"] + 1 if key == STATE["last"] else 0
    STATE["last"] = key
    return s["num_tiles"] > 0 and s["pending_tiles"] == 0 and s["num_nanite_tiles"] >= s["num_tiles"] and STATE["stable"] >= 30


def shoot(name, then):
    STATE["shot"] = name
    STATE["frames"] = 0
    STATE["phase"] = "shoot"
    STATE["next"] = then
    capture.set_actor_location_and_rotation(TOP[0], TOP[1], False, True)


def settle(then):
    STATE["phase"] = "settle"
    STATE["next"] = then
    STATE["stable"] = 0
    STATE["ticks"] = 0


def step():
    nxt = STATE["next"]
    if nxt == "base":
        STATE["base"] = stats()
        log.append("base {}; garage: {}".format(json.dumps(STATE["base"]), HOUSE.get_editor_property("garage_status")))
        if STATE["base"]["num_joined_garages"] != 1:
            failed.append("base: {} garages joined for 1 ({})".format(STATE["base"]["num_joined_garages"], HOUSE.get_editor_property("garage_status")))
        shoot("base_top", "edit")
    elif nxt == "edit":
        STATE["edit"] += 1
        if STATE["edit"] >= len(EDITS):
            finish()
            return
        name, what, road, new, offset, joined = EDITS[STATE["edit"]]
        if what == "road":
            STATE["saved"] = points(road)
            if new:
                values = [unreal.Vector(x * 100.0, y * 100.0, 0.0) for x, y in new]
            else:
                values = [p + unreal.Vector(offset[0] * 100.0, offset[1] * 100.0, 0.0) for p in STATE["saved"]]
            set_points(road, values)
        else:
            STATE["saved"] = house_place()
            location, garage = STATE["saved"]
            if what == "house":
                set_house((location + unreal.Vector(offset[0] * 100.0, offset[1] * 100.0, 0.0), garage))
            else:
                set_house((location, unreal.Vector(garage.x, -garage.y, garage.z)))
        settle("moved")
    elif nxt == "moved":
        name, what, _, _, _, joined = EDITS[STATE["edit"]]
        moved = stats()
        status = HOUSE.get_editor_property("garage_status")
        log.append("{} moved {}; garage: {}".format(name, json.dumps(moved), status))
        # Moving the house along A changes no count when the plots it frees and takes even out: its garage stays joined.
        same = (moved["num_roads"], moved["num_buildings"]) == (STATE["base"]["num_roads"], STATE["base"]["num_buildings"])
        if moved["last_error"] or (same and what != "house"):
            failed.append("{}: the move changed nothing ({})".format(name, moved["last_error"] or "same counts"))
        if moved["num_joined_garages"] != joined:
            failed.append("{}: {} garages joined for {} ({})".format(name, moved["num_joined_garages"], joined, status))
        if name in GARAGE_VIA and GARAGE_VIA[name] not in status:
            failed.append("{}: the garage joins no small road ({})".format(name, status))
        shoot(name + "_top", "undo")
    elif nxt == "undo":
        if EDITS[STATE["edit"]][1] == "road":
            set_points(EDITS[STATE["edit"]][2], STATE["saved"])
        else:
            set_house(STATE["saved"])
        settle("back")
    elif nxt == "back":
        name = EDITS[STATE["edit"]][0]
        s = stats()
        same = s["num_roads"] == STATE["base"]["num_roads"] and s["num_buildings"] == STATE["base"]["num_buildings"] and not s["last_error"]
        same = same and s["num_joined_garages"] == STATE["base"]["num_joined_garages"]
        log.append("{} back {} -> {}".format(name, json.dumps(s), "SAME" if same else "DIFFERENT"))
        if not same:
            failed.append("{} back: {} roads and {} buildings for {} and {}".format(name, s["num_roads"], s["num_buildings"], STATE["base"]["num_roads"], STATE["base"]["num_buildings"]))
        shoot(name + "_back_top", "edit")


def tick(delta):
    try:
        if STATE["phase"] == "settle":
            STATE["ticks"] += 1
            if STATE["ticks"] > 6000:
                finish("did not settle: {}".format(json.dumps(stats())))
                return
            if settled():
                step()
            return
        comp.capture_scene()
        STATE["frames"] += 1
        if STATE["frames"] < WARMUP:
            return
        unreal.RenderingLibrary.export_render_target(world, rt, OUT, STATE["shot"] + ".png")
        log.append("shot " + STATE["shot"])
        STATE["phase"] = "step"
        step()
    except Exception:
        finish(traceback.format_exc())


STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
unreal.log("CS2ROADTIERMINI start " + json.dumps(stats()))
