"""Story mode: GTA IV's first mission, "The Cousins Bellic" (roman1.sco), with its cutscenes.

The opening (intro.cut: the Platypus, then Roman meeting Niko at the Broker docks), the drive to
Roman's place in Hove Beach in his car, and rom1_b.cut in his apartment. Exported for the
GTAIVTraffic plugin's story player: the cutscenes (cutscenes.py), the characters, props and
vehicles they use, Roman's car (drivable), the mission's text and the places it happens.
"""

import json
import os

import numpy as np

from .cutscenes import CUTSPROPS, CutsceneExporter
from .img import ImgArchive
from .mapdata import ModelDef

MISSION = "roman1"
CUTSCENES = ("intro", "rom1_b")
ROMAN_CAR = "rom"
ROMAN_PED = "ig_roman"
# Broker, from the docks where the Platypus comes in to Hove Beach (GTA metres): the districts
# with map pieces in here come along with the export
AREA_BOX = (640.0, -620.0, 1000.0, -120.0)
# ROMAN1's lines (the GXT keys are hashed: matched by their English text)
TEXT = {
    "get_in": "Get into ~b~Roman's car.",
    "drive": "Drive to ~y~Roman's place.",
    "radar": "The yellow icon on the radar shows the location of your next objective.",
    "route": "While in a vehicle the colored line on the radar",
    "dead": "Roman is dead.",
    "trashed": "Roman's car has been trashed.",
    "abandoned": "You have abandoned Roman.",
    "stuck": "The vehicle is stuck.",
}


def mission_areas(gd, areas):
    """The map districts the mission needs (the docks, Hove Beach and the streets between)."""
    x0, y0, x1, y1 = AREA_BOX
    out = []
    for area, insts in areas.items():
        if area.endswith("_interiors") or area in ("interiors",) or area.startswith("lodcull"):
            continue
        n = sum(1 for i in insts if x0 <= i.pos[0] <= x1 and y0 <= i.pos[1] <= y1)
        if n >= 40:
            out.append(area)
    # the Platypus, the ship Niko arrives on (the opening's first scenes are aboard)
    if "shipintro" in areas:
        out.append("shipintro")
    return sorted(out)


def _mission_text(game_dir):
    try:
        from .gxt import load_gxt
        tables = load_gxt(open(os.path.join(game_dir, "common", "text", "american.gxt"), "rb").read())
    except Exception:
        return {}
    t = tables.get("ROMAN1", {})
    out = {}
    for k, want in TEXT.items():
        hit = next((v for v in t.values() if want.lower() in v.lower()), None)
        if hit:
            out[k] = hit
    main = tables.get("MAIN", {})
    from .mapdata import joaat
    for key, name in (("m_pass", "passed"), ("m_fail", "failed")):
        v = main.get(joaat(key))
        if v:
            out[name] = v
    out.setdefault("passed", "MISSION PASSED")
    out.setdefault("failed", "MISSION FAILED")
    return out


def export_story(ex, batches, batch_meshes, gltf_batch_cls, ue_name, vehicle_defs):
    """ex: the UnrealExporter (its game data, material helpers and output folder).
    Returns the story manifest, or None."""
    out_dir = os.path.join(ex.out_dir, "story")
    ce = CutsceneExporter(ex.game_dir, ex.gd.key, out_dir, log=ex.log)
    manifests = {}
    try:
        for name in CUTSCENES:
            try:
                manifests[name] = ce.export(name, vehicle_models=set(vehicle_defs))
            except Exception as e:
                ex.log("Couldn't export cutscene %s: %s" % (name, e))
    finally:
        ce.close()
    if "intro" not in manifests:
        return None
    models = dict(ce.models)

    # the cutscenes' props: cutsprops.img (searched like the map's archives)
    props_img = ImgArchive(os.path.join(ex.game_dir, CUTSPROPS), ex.gd.key)
    ex.gd.archives.append(("cutsprops", props_img))
    for n in props_img.names():
        ex.gd.index.setdefault(n, props_img)
    props = {}
    b = gltf_batch_cls()
    for name, kind in sorted(models.items()):
        if kind != "prop":
            continue
        try:
            mesh = ex.gd.load_model(ModelDef(name, name, 0.0, 0, None, "cutsprops"), {})
        except Exception as e:
            ex.log("Couldn't read cutscene prop %s: %s" % (name, e))
            mesh = None
        if mesh is None:
            continue
        sm = "SM_CS_" + ue_name(name)
        b.add_model(sm, mesh, [ex._material_name(m, mesh, name) for m in mesh.materials])
        props[name] = sm
    if b.nodes:
        fn = "story_props.gltf"
        b.write(os.path.join(ex.out_dir, "meshes", fn))
        batches.append(fn)
        batch_meshes[fn] = [n["name"] for n in b.nodes if "mesh" in n]

    # the characters (cutscene versions, Roman in game, extras) on the shared ped skeleton
    from .peds import PedArchive, standard_facing
    pa = PedArchive(ex.game_dir, ex.gd.key, cutscenes=True)
    peds, ped_assets = [], {}
    try:
        wanted = sorted({n for n, k in models.items() if k == "ped" and n != "player"} | {ROMAN_PED})
        for name in wanted:
            try:
                p = pa.ped(name)
            except Exception as e:
                ex.log("Couldn't read %s: %s" % (name, e))
                p = None
            if p is None or not standard_facing(p.skeleton):
                continue
            prefix = "PED_%s_" % name
            slots = [ex._char_material(prefix, m, p.mesh, [pa.textures(name)]) for m in p.mesh.materials]
            mesh_name = "SK_PED_" + ue_name(name)
            gb = gltf_batch_cls()
            gb.add_skinned_model(mesh_name, p.mesh, slots, p.skeleton)
            fn = "ped_%s.gltf" % ue_name(name)
            gb.write(os.path.join(ex.out_dir, "meshes", fn))
            peds.append({"name": name, "mesh": mesh_name, "gltf": fn, "parts": p.parts, "slots": slots,
                         "height": float(p.mesh.positions[:, 2].max() - p.mesh.positions[:, 2].min())})
            ped_assets[name] = mesh_name
        # Niko (the player): in the cutscenes and as the one who plays the mission
        try:
            p = pa.player()
        except Exception:
            p = None
        if p is not None and standard_facing(p.skeleton):
            slots = [ex._char_material("PED_niko_", m, p.mesh, [pa.player_textures(p)]) for m in p.mesh.materials]
            gb = gltf_batch_cls()
            gb.add_skinned_model("SK_PED_Niko", p.mesh, slots, p.skeleton)
            gb.write(os.path.join(ex.out_dir, "meshes", "ped_niko.gltf"))
            peds.append({"name": "niko", "mesh": "SK_PED_Niko", "gltf": "ped_niko.gltf", "parts": p.parts,
                         "slots": slots, "player": True})
        ped_assets["player"] = "SK_PED_Niko"
    finally:
        pa.close()

    vehicles = sorted({n for n, k in models.items() if k == "vehicle"} | {ROMAN_CAR})

    # where the mission starts: everyone where the opening leaves them
    last = manifests["intro"]["shots"][-1]
    start = {}
    for a in last["actors"]:
        key = {"player": "player", "cs_roman_d": "roman", ROMAN_CAR: "car"}.get(a["model"])
        if key and key not in start:
            start[key] = a["root"][-1]
    door = None
    if "rom1_b" in manifests:
        for a in manifests["rom1_b"]["shots"][0]["actors"]:
            if a["model"] == "player":
                door = a["root"][0]
    story = {
        "mission": MISSION,
        "cutscenes": {"intro": "cutscene_intro.json", "end": "cutscene_rom1_b.json" if "rom1_b" in manifests else None},
        "start": start,
        "door": door,               # Roman's front door (where the last cutscene starts)
        "car": ROMAN_CAR,
        "roman": ROMAN_PED,
        "text": _mission_text(ex.game_dir),
        "models": {n: {"kind": k, "asset": props.get(n) or ped_assets.get(n) or ("SM_VEH_" + ue_name(n) if k == "vehicle" else None)}
                   for n, k in models.items()},
        "peds": peds,
        "vehicles": vehicles,
    }
    with open(os.path.join(out_dir, "story.json"), "w", encoding="utf-8") as f:
        json.dump(story, f, indent=1)
    ex.log("Story: %s with %d cutscenes, %d characters, %d props, %d vehicles"
           % (MISSION, len(manifests), len(peds), len(props), len(vehicles)))
    return story
