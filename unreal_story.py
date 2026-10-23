"""Unreal side of story mode (run inside the editor by unreal_runner): GTA IV's first mission and
its cutscenes from the exporter's story/ folder -> /Game/GTAIV/Story.

  S_CS_<track>         the cutscene audio
  A_CS_<cut>_<n>       each character's body animation per shot (on the shared ped skeleton)
  DA_CS_<cut>          the cutscenes (UGTAIVCutscene: camera, actors, subtitles, audio)
  DA_GTAIV_Story       the mission (UGTAIVStoryData)
"""

import json
import os

import unreal

import unreal_player as up

ROOT = "/Game/GTAIV"
STORY = ROOT + "/Story"
EAL = unreal.EditorAssetLibrary
AT = unreal.AssetToolsHelpers.get_asset_tools()


def _ue_pos(x, y, z):
    return x * 100.0, -y * 100.0, z * 100.0


def _ue_root(row):
    """[x, y, z, qw, qx, qy, qz] GTA -> [x, y, z, qx, qy, qz, qw] Unreal (cm, Y mirrored)."""
    x, y, z, qw, qx, qy, qz = row
    return [x * 100.0, -y * 100.0, z * 100.0, -qx, qy, -qz, qw]


def _transform(row):
    r = _ue_root(row)
    return unreal.Transform(unreal.Vector(r[0], r[1], r[2]), unreal.Quat(r[3], r[4], r[5], r[6]).rotator(),
                            unreal.Vector(1, 1, 1))


def _import_wav(path, folder, name):
    dest = "%s/%s" % (folder, name)
    if EAL.does_asset_exist(dest):
        return unreal.load_asset(dest)
    task = unreal.AssetImportTask()
    task.filename = path
    task.destination_path = folder
    task.destination_name = name
    task.automated = True
    task.replace_existing = True
    task.save = True
    AT.import_asset_tasks([task])
    return unreal.load_asset(dest)


def _sequence(path_name, skel, preview, ref, baker, clip, fps):
    """A body animation: every bone's rotation, and the pelvis's movement too (sitting down,
    leaning, climbing the stairs)."""
    folder, name = path_name.rsplit("/", 1)
    if EAL.does_asset_exist(path_name):
        EAL.delete_asset(path_name)
    fac = unreal.AnimSequenceFactory()
    fac.set_editor_property("target_skeleton", skel)
    if preview:
        fac.set_editor_property("preview_skeletal_mesh", preview)
    seq = AT.create_asset(name, folder, unreal.AnimSequence, fac)
    frames = clip["frames"]
    locs = [baker.local_rotations(f) for f in clip["pose"]]
    pelvis = []
    par = baker.parent.get("Char_Pelvis")
    for f in clip["pose"]:
        w = baker.world(f)
        p = w["Char_Pelvis"][0] if "Char_Pelvis" in w else ref["Char_Pelvis"]["wp"]
        if par in ref:
            pp, pq = ref[par]["wp"], ref[par]["wq"]
            d = (p[0] - pp[0], p[1] - pp[1], p[2] - pp[2])
            pelvis.append(up.qrot(up.qinv(pq), d))
        else:
            pelvis.append(ref["Char_Pelvis"]["lp"])
    ctrl = seq.get_editor_property("controller")
    ctrl.open_bracket(unreal.Text("GTA IV cutscene"), False)
    ctrl.set_frame_rate(unreal.FrameRate(int(fps), 1), False)
    ctrl.set_number_of_frames(unreal.FrameNumber(max(1, frames - 1)), False)
    animated = set(clip["animated"])
    for b, r in ref.items():
        if b == "Char_Pelvis":
            pos = [unreal.Vector(*p) for p in pelvis]
        else:
            pos = [unreal.Vector(*r["lp"])] * frames
        use = b in animated and b != "Char" and not any(s in b for s in up.SKIP)
        rot = [unreal.Quat(*(fr[b] if use and b in fr else r["lq"])) for fr in locs]
        ctrl.add_bone_curve(b, False)
        ctrl.set_bone_track_keys(b, pos, rot, [unreal.Vector(1, 1, 1)] * frames, False)
    ctrl.close_bracket(False)
    EAL.save_loaded_asset(seq)
    return seq


def _model_asset(story, model):
    info = story["models"].get(model) or {}
    kind, asset = info.get("kind"), info.get("asset")
    if kind == "ped" and asset:
        return "ped", unreal.load_asset("%s/Characters/%s" % (ROOT, asset))
    if kind == "prop" and asset:
        return "prop", unreal.load_asset("%s/Meshes/%s" % (ROOT, asset))
    if kind == "vehicle":
        return "vehicle", model
    return None, None


def import_cutscene(story_dir, story, name, rel, skel, ref, say):
    m = json.load(open(os.path.join(story_dir, rel), encoding="utf-8"))
    sound = None
    if m.get("audio"):
        wav = os.path.join(story_dir, m["audio"])
        if os.path.isfile(wav):
            sound = _import_wav(wav, STORY + "/Audio", "S_CS_" + os.path.splitext(m["audio"])[0])
    shots = []
    n_anim = 0
    bakers = {}
    for si, shot in enumerate(m["shots"]):
        cam = []
        for c in shot["camera"]:
            cam += [*_ue_pos(c[0], c[1], c[2]), c[3], -c[4], c[5], c[6], -c[7], c[8], c[9]]
        actors = []
        for a in shot["actors"]:
            kind, asset = _model_asset(story, a["model"])
            if kind is None or (kind != "vehicle" and asset is None):
                continue
            root = []
            for row in a["root"]:
                root += _ue_root(row)
            act = unreal.GTAIVCutActor()
            act.set_editor_property("model", a["model"])
            act.set_editor_property("root", root)
            if kind == "ped":
                act.set_editor_property("skeletal_mesh", asset)
                if a.get("clip") and skel is not None:
                    data = json.load(open(os.path.join(story_dir, a["clip"]), encoding="utf-8"))
                    key = len(data["bones"])
                    if key not in bakers:
                        bakers[key] = up.Baker(data, ref)
                    seq = _sequence("%s/Animations/A_CS_%s_%d" % (STORY, name, n_anim), skel, asset, ref, bakers[key],
                                    data["clips"]["body"], data["fps"])
                    n_anim += 1
                    act.set_editor_property("anim", seq)
            elif kind == "prop":
                act.set_editor_property("static_mesh", asset)
            else:
                act.set_editor_property("vehicle", asset)
            actors.append(act)
        lights = []
        for L in shot.get("lights", []):
            tr = L["track"]
            ue = []
            for k in range(0, len(tr), 6):
                ue += [tr[k] * 100.0, -tr[k + 1] * 100.0, tr[k + 2] * 100.0, tr[k + 3], -tr[k + 4], tr[k + 5]]
            cl = unreal.GTAIVCutLight()
            cl.set_editor_property("name", L["name"])
            cl.set_editor_property("spot", L["type"] in ("spt", "spot", "key"))
            cl.set_editor_property("rim", L["type"] == "rim")
            cl.set_editor_property("colour", unreal.LinearColor(*L["colour"], 1.0))
            cl.set_editor_property("track", ue)
            lights.append(cl)
        s = unreal.GTAIVCutShot()
        s.set_editor_property("lights", lights)
        s.set_editor_property("frames", shot["frames"])
        s.set_editor_property("start", shot["start"])
        s.set_editor_property("camera", cam)
        s.set_editor_property("actors", actors)
        shots.append(s)
        say("LOG  cutscene %s: shot %d / %d" % (name, si + 1, len(m["shots"])))
    subs = []
    for start, dur, text in m["subtitles"]:
        st = unreal.GTAIVSubtitle()
        st.set_editor_property("start", start)
        st.set_editor_property("duration", dur)
        st.set_editor_property("text", text)
        subs.append(st)
    path = "%s/DA_CS_%s" % (STORY, name)
    da = unreal.load_asset(path) if EAL.does_asset_exist(path) else None
    if da is None:
        da = AT.create_asset("DA_CS_" + name, STORY, unreal.GTAIVCutscene, unreal.DataAssetFactory())
    da.set_editor_property("fps", m["fps"])
    da.set_editor_property("duration", m["duration"])
    da.set_editor_property("shots", shots)
    da.set_editor_property("subtitles", subs)
    da.set_editor_property("audio", sound)
    # GTA IV plays the opening at dusk and Roman's apartment at night
    da.set_editor_property("hour", {"intro": 20.6, "rom1_b": 21.5}.get(name, -1.0))
    EAL.save_loaded_asset(da, only_if_is_dirty=False)
    say("LOG Cutscene %s: %d shots, %.0fs, %d animations" % (name, len(shots), m["duration"], n_anim))
    return da


def import_story(export_dir, rel, say):
    if not hasattr(unreal, "GTAIVStoryData"):
        say("LOG Story mode needs the updated GTAIVTraffic plugin")
        return None
    path = os.path.join(export_dir, rel)
    story_dir = os.path.dirname(path)
    story = json.load(open(path, encoding="utf-8"))
    skel = unreal.load_asset(up.SKELETON) if EAL.does_asset_exist(up.SKELETON) else None
    ref = up.reference_pose(skel) if skel else None
    cuts = {}
    for key, rel_cut in (story.get("cutscenes") or {}).items():
        if rel_cut:
            cuts[key] = import_cutscene(story_dir, story, rel_cut[len("cutscene_"):-len(".json")], rel_cut, skel, ref, say)
    da_path = STORY + "/DA_GTAIV_Story"
    da = unreal.load_asset(da_path) if EAL.does_asset_exist(da_path) else None
    if da is None:
        da = AT.create_asset("DA_GTAIV_Story", STORY, unreal.GTAIVStoryData, unreal.DataAssetFactory())
    da.set_editor_property("intro", cuts.get("intro"))
    da.set_editor_property("end", cuts.get("end"))
    st = story.get("start") or {}
    for key, prop in (("player", "player_start"), ("roman", "roman_start"), ("car", "car_start")):
        if key in st:
            da.set_editor_property(prop, _transform(st[key]))
    if story.get("door"):
        d = story["door"]
        da.set_editor_property("door", unreal.Vector(*_ue_pos(d[0], d[1], d[2])))
    da.set_editor_property("car_name", story.get("car", "rom"))
    roman = "%s/Characters/SK_PED_%s" % (ROOT, story.get("roman", "ig_roman"))
    if EAL.does_asset_exist(roman):
        da.set_editor_property("roman_mesh", unreal.load_asset(roman))
    da.set_editor_property("text", {k: v for k, v in (story.get("text") or {}).items()})
    EAL.save_loaded_asset(da, only_if_is_dirty=False)
    say("LOG Story: The Cousins Bellic is ready (Start New Game plays it)")
    return da
