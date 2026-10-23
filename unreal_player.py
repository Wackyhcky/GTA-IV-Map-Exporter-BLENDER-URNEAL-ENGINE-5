"""Runs inside Unreal Editor (imported by unreal_runner): the GTA IV player kit.

    Characters/Weapons/DA_GTAIV_Pistol      the pistol (UGTAIVWeaponData): w_glock, its icon,
                                            gun@handgun clips baked for the ped skeleton, sounds
    Characters/Weapons/Animations/A_GTAIV_Pistol_*  the same clips as Animation Sequences
    Characters/Weapons/Sounds/S_GTAIV_*     pistol sounds from resident.rpf/weapons
and, in a Game Animation Sample project, the "GTA IV Player" component on
SandboxCharacter_Mover_Ragdoll (Niko + pistol), which GM_GTAIV uses as the player.

The clips come as model-space bone transforms of the exporter's (Unreal-convention) skeleton.
Unreal's import turns that space and the bones' axes; both are recovered from the skeleton's
reference pose, so the keys are written exactly in the imported skeleton's bone spaces.
"""

import json
import math
import os
import shutil

import unreal

ROOT = "/Game/GTAIV"
CHARS = ROOT + "/Characters"
WEAPONS = CHARS + "/Weapons"
SKELETON = CHARS + "/SKEL_GTAIV_Ped"
DATA = WEAPONS + "/DA_GTAIV_Pistol"
PLAYER_BP = "/Game/Blueprints/SandboxCharacter_Mover_Ragdoll"
GAME_MODE = CHARS + "/GM_GTAIV"
EAL = unreal.EditorAssetLibrary
AT = unreal.AssetToolsHelpers.get_asset_tools()
SKIP = ("Roll", "Twist", "FB_", "PointFB_")


# ------------------------------------------------------------------ small quaternion maths (x, y, z, w)
def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def qinv(q):
    return (-q[0], -q[1], -q[2], q[3])


def qrot(q, v):
    x, y, z, _ = qmul(qmul(q, (v[0], v[1], v[2], 0.0)), qinv(q))
    return (x, y, z)


def qnorm(q):
    n = math.sqrt(sum(c * c for c in q)) or 1.0
    return tuple(c / n for c in q)


def mat_to_q(m):
    """3x3 rows -> quaternion."""
    t = m[0][0] + m[1][1] + m[2][2]
    if t > 0:
        r = math.sqrt(1 + t) * 2
        return qnorm(((m[2][1] - m[1][2]) / r, (m[0][2] - m[2][0]) / r, (m[1][0] - m[0][1]) / r, 0.25 * r))
    if m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        r = math.sqrt(1 + m[0][0] - m[1][1] - m[2][2]) * 2
        return qnorm((0.25 * r, (m[0][1] + m[1][0]) / r, (m[0][2] + m[2][0]) / r, (m[2][1] - m[1][2]) / r))
    if m[1][1] > m[2][2]:
        r = math.sqrt(1 + m[1][1] - m[0][0] - m[2][2]) * 2
        return qnorm(((m[0][1] + m[1][0]) / r, 0.25 * r, (m[1][2] + m[2][1]) / r, (m[0][2] - m[2][0]) / r))
    r = math.sqrt(1 + m[2][2] - m[0][0] - m[1][1]) * 2
    return qnorm(((m[0][2] + m[2][0]) / r, (m[1][2] + m[2][1]) / r, 0.25 * r, (m[1][0] - m[0][1]) / r))


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def unit(v):
    n = math.sqrt(sum(c * c for c in v)) or 1.0
    return (v[0] / n, v[1] / n, v[2] / n)


def frame(up, side):
    """Orthonormal basis (as matrix columns) from two directions."""
    z = unit(up)
    x = unit(cross(side, z))
    y = cross(z, x)
    return [[x[0], y[0], z[0]], [x[1], y[1], z[1]], [x[2], y[2], z[2]]]


def matmul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def transpose(a):
    return [[a[j][i] for j in range(3)] for i in range(3)]


# ------------------------------------------------------------------ baking
def reference_pose(skel):
    pose = unreal.AnimPoseExtensions.get_reference_pose(skel)
    out = {}
    for name in unreal.AnimPoseExtensions.get_bone_names(pose):
        w = unreal.AnimPoseExtensions.get_bone_pose(pose, name, unreal.AnimPoseSpaces.WORLD)
        l = unreal.AnimPoseExtensions.get_bone_pose(pose, name, unreal.AnimPoseSpaces.LOCAL)
        out[str(name)] = {
            "wp": (w.translation.x, w.translation.y, w.translation.z),
            "wq": (w.rotation.x, w.rotation.y, w.rotation.z, w.rotation.w),
            "lp": (l.translation.x, l.translation.y, l.translation.z),
            "lq": (l.rotation.x, l.rotation.y, l.rotation.z, l.rotation.w),
        }
    return out


class Baker:
    """Exporter model space -> the imported skeleton's bone spaces."""

    def __init__(self, data, ref):
        self.names = data["bones"]
        self.ref = ref
        # The import may mirror the space (glTF is right-handed, Unreal left-handed), which no
        # rotation can express: try with and without mirroring Y and keep what matches.
        best = None
        for mirror in (False, True):
            rest = {n: self._m(data["rest"][i], mirror) for i, n in enumerate(self.names)}

            def vecs(pos):
                return (sub(pos("Char_Head"), pos("Char_Pelvis")), sub(pos("Char_L_Thigh"), pos("Char_R_Thigh")),
                        sub(pos("Char_L_Toe"), pos("Char_L_Calf")))
            e_up, e_side, e_fwd = vecs(lambda n: rest[n][:3])
            u_up, u_side, u_fwd = vecs(lambda n: ref[n]["wp"])
            c = mat_to_q(matmul(frame(u_up, u_side), transpose(frame(e_up, e_side))))
            err = sum((a - b) ** 2 for a, b in zip(unit(qrot(c, e_fwd)), unit(u_fwd)))
            if best is None or err < best[0]:
                best = (err, mirror, rest, c, e_up, u_up)
        _err, self.mirror, rest, self.c, e_up, u_up = best
        self.rest = rest
        self.scale = math.sqrt(sum(c * c for c in u_up)) / max(1e-6, math.sqrt(sum(c * c for c in e_up)))
        p = qrot(self.c, rest["Char_Pelvis"][:3])
        upel = ref["Char_Pelvis"]["wp"]
        self.offset = (upel[0] - self.scale * p[0], upel[1] - self.scale * p[1], upel[2] - self.scale * p[2])
        # each bone's own axes: imported rest = space change * exporter rest * K
        self.k = {}
        for n in self.names:
            if n in ref:
                self.k[n] = qmul(qinv(qmul(self.c, tuple(rest[n][3:]))), ref[n]["wq"])
        parents = data["skeleton"]["parents"]
        sk_names = data["skeleton"]["names"]
        self.parent = {n: (sk_names[p] if p >= 0 else None) for n, p in zip(sk_names, parents)}

    @staticmethod
    def _m(row, mirror):
        """Mirror a [px,py,pz,qx,qy,qz,qw] row across Y."""
        if not mirror:
            return list(row)
        return [row[0], -row[1], row[2], -row[3], row[4], -row[5], row[6]]

    def world(self, pose):
        """One frame ([bone] -> [px,py,pz,qx,qy,qz,qw]) -> {bone: (pos, quat)} in Unreal space."""
        out = {}
        for i, n in enumerate(self.names):
            if n not in self.k:
                continue
            row = self._m(pose[i], self.mirror)
            q = qmul(qmul(self.c, tuple(row[3:])), self.k[n])
            p = qrot(self.c, row[:3])
            out[n] = ((self.offset[0] + self.scale * p[0], self.offset[1] + self.scale * p[1],
                       self.offset[2] + self.scale * p[2]), qnorm(q))
        return out

    def local_rotations(self, pose, fold_root=False):
        w = self.world(pose)
        if fold_root and "Char" in w and "Char" in self.ref:   # the root's turn stays in its children
            w["Char"] = (w["Char"][0], self.ref["Char"]["wq"])
        out = {}
        for n, (_p, q) in w.items():
            par = self.parent.get(n)
            pq = w[par][1] if par in w else (self.ref[par]["wq"] if par in self.ref else (0, 0, 0, 1))
            out[n] = qnorm(qmul(qinv(pq), q))
        return out


def _bake_clip(baker, ref, clip, fps):
    """One exported clip -> (GTAIVAnimClip, per-frame local rotations). Clips with root motion fold
    the root's turn into the pelvis and keep the pelvis's movement (PelvisOffset, cm)."""
    root = clip.get("root", False)
    locals_ = [baker.local_rotations(f, fold_root=root) for f in clip["pose"]]
    bones = [b for b in clip["animated"] if b in ref and not any(s in b for s in SKIP) and b != "Char"]
    if root and "Char_Pelvis" not in bones and "Char_Pelvis" in ref:
        bones.append("Char_Pelvis")
    keys = []
    for fr in locals_:
        for b in bones:
            t = unreal.Transform()
            t.translation = unreal.Vector(*ref[b]["lp"])
            t.rotation = unreal.Quat(*fr[b])
            keys.append(t)
    c = unreal.GTAIVAnimClip(fps=fps, frames=clip["frames"], bones=bones, keys=keys)
    if root and "Char_Pelvis" in ref:
        rp = ref["Char_Pelvis"]["wp"]
        offs = []
        for f in clip["pose"]:
            p = baker.world(f)["Char_Pelvis"][0]
            offs.append(unreal.Vector(p[0] - rp[0], p[1] - rp[1], p[2] - rp[2]))
        c.set_editor_property("pelvis_offset", offs)
    return c, locals_


def build_vehicle_anims(path, log):
    """veh_anims.json -> /Game/GTAIV/Traffic/DA_GTAIV_VehicleAnims (drivers' sit / steer clips)."""
    if not os.path.isfile(path) or not hasattr(unreal, "GTAIVVehicleAnims"):
        return None
    skel = unreal.load_asset(SKELETON)
    if skel is None:
        log("LOG Drivers need the characters: export them to see people in the cars")
        return None
    data = json.load(open(path, encoding="utf-8"))
    ref = reference_pose(skel)
    baker = Baker(data, ref)
    field = {"sit_drive": "sit", "steer_l": "steer_left", "steer_r": "steer_right", "reverse": "reverse",
             "sit_pass": "sit_passenger", "get_in_ds": "get_in", "get_out_ds": "get_out", "jump_out_ds": "jump_out",
             "d_open_out_ds": "open_door", "d_close_in_ds": "close_door_inside", "d_close_out_ds": "close_door_outside",
             "jack": "jack", "jacked": "jacked"}
    sets = {}
    for name, clip in data["clips"].items():
        group, cname = name.split("/", 1)
        if cname not in field:
            continue
        c, _ = _bake_clip(baker, ref, clip, data["fps"])
        sets.setdefault(group, {})[field[cname]] = c
    for group, clips in sets.items():   # a set without a clip (the bus has no jump out) borrows the car's
        for k, c in sets.get("std", {}).items():
            clips.setdefault(k, c)
    folder = ROOT + "/Traffic"
    apath = folder + "/DA_GTAIV_VehicleAnims"
    da = unreal.load_asset(apath) if EAL.does_asset_exist(apath) else None
    if da is None:
        da = AT.create_asset("DA_GTAIV_VehicleAnims", folder, unreal.GTAIVVehicleAnims, unreal.DataAssetFactory())
    da.set_editor_property("sets", {unreal.Name(g): unreal.GTAIVVehicleAnimSet(**v) for g, v in sets.items()})
    EAL.save_loaded_asset(da, only_if_is_dirty=False)
    log("LOG Driver animations: %s" % ", ".join(sorted(sets)))
    return da


def build_clips(anims_path, log):
    skel = unreal.load_asset(SKELETON)
    if skel is None:
        log("LOG No ped skeleton: export the characters first")
        return {}, {}
    data = json.load(open(anims_path, encoding="utf-8"))
    ref = reference_pose(skel)
    baker = Baker(data, ref)
    niko = unreal.load_asset(CHARS + "/SK_PED_Niko")
    folder = WEAPONS + "/Animations"
    clips, seqs = {}, {}
    for name, clip in data["clips"].items():
        frames = clip["frames"]
        locals_ = [baker.local_rotations(f) for f in clip["pose"]]
        bones = [b for b in clip["animated"] if b in ref and not any(s in b for s in SKIP) and b != "Char"]
        keys = []
        for fr in locals_:
            for b in bones:
                q = fr[b]
                lp = ref[b]["lp"]
                keys.append(unreal.Transform(unreal.Vector(*lp), unreal.Quat(*q).rotator(), unreal.Vector(1, 1, 1)))
        # Transform(rotator) loses nothing for these; keep quats exact via set_editor_property below
        for i, t in enumerate(keys):
            fr, b = divmod(i, len(bones))
            q = locals_[fr][bones[b]]
            t.rotation = unreal.Quat(*q)
        clips[name] = unreal.GTAIVAnimClip(fps=data["fps"], frames=frames, bones=bones, keys=keys)
        seqs[name] = make_sequence(folder, "A_GTAIV_Pistol_" + name, skel, niko, ref, locals_, frames, data["fps"])
    log("LOG Pistol animations: %s (space %s)" % (", ".join(sorted(clips)), "mirrored" if baker.mirror else "direct"))
    global LAST_BAKER
    LAST_BAKER = baker
    return clips, seqs


LAST_BAKER = None


def grip_transform(kit, baker, hand="Char_R_Hand"):
    """The weapon relative to the hand bone so that its gun_grip bone sits in the hand, like
    GTA IV attaches it. The weapon mesh is in the exporter's GTA space mirrored across Y (cm);
    the hand bone's axes differ from GTA's by the baker's per-bone rotation."""
    g = kit.get("grip")
    if not g or baker is None or hand not in baker.k:
        return unreal.Transform()
    q = qnorm(tuple(g[3:]))
    qi = qinv(q)
    t = qrot(qi, (-g[0], -g[1], -g[2]))          # inverse of the grip bone, GTA model space
    if baker.mirror:
        qi = (-qi[0], qi[1], -qi[2], qi[3])
        t = (t[0], -t[1], t[2])
    t = (t[0] * 100.0, t[1] * 100.0, t[2] * 100.0)
    kinv = qinv(baker.k[hand])
    rot = qnorm(qmul(kinv, qi))
    pos = qrot(kinv, t)
    out = unreal.Transform()
    out.translation = unreal.Vector(*pos)
    out.rotation = unreal.Quat(*rot)
    return out


def make_sequence(folder, asset_name, skel, preview, ref, locals_, frames, fps):
    path = "%s/%s" % (folder, asset_name)
    if EAL.does_asset_exist(path):
        EAL.delete_asset(path)
    fac = unreal.AnimSequenceFactory()
    fac.set_editor_property("target_skeleton", skel)
    if preview:
        fac.set_editor_property("preview_skeletal_mesh", preview)
    seq = AT.create_asset(asset_name, folder, unreal.AnimSequence, fac)
    ctrl = seq.get_editor_property("controller")
    ctrl.open_bracket(unreal.Text("GTA IV animation"), False)
    ctrl.set_frame_rate(unreal.FrameRate(int(fps), 1), False)
    ctrl.set_number_of_frames(unreal.FrameNumber(max(1, frames - 1)), False)
    for b, r in ref.items():
        pos = [unreal.Vector(*r["lp"])] * frames
        rot = [unreal.Quat(*(fr[b] if b in fr and not any(s in b for s in SKIP) and b != "Char" else r["lq"]))
               for fr in locals_]
        scl = [unreal.Vector(1, 1, 1)] * frames
        ctrl.add_bone_curve(b, False)
        ctrl.set_bone_track_keys(b, pos, rot, scl, False)
    ctrl.close_bracket(False)
    EAL.save_loaded_asset(seq)
    return seq


def import_sounds(sounds, log):
    folder = WEAPONS + "/Sounds"
    tasks = []
    for name, wav in sounds.items():
        t = unreal.AssetImportTask()
        t.filename = wav
        t.destination_path = folder
        t.destination_name = "S_GTAIV_" + name
        t.automated = True
        t.replace_existing = True
        t.save = False
        tasks.append(t)
    if tasks:
        AT.import_asset_tasks(tasks)
    out = {}
    for name in sounds:
        s = unreal.load_asset("%s/S_GTAIV_%s" % (folder, name))
        if s:
            out[name] = s
    if out:
        EAL.save_directory(folder, only_if_is_dirty=False)
    log("LOG Imported %d pistol sounds" % len(out))
    return out


BLOOD_HLSL = r"""
float2 uv = UV * Atlas.xy + Atlas.zw;
float4 c = Texture2DSample(Tex, TexSampler, uv);
return float4(c.rgb * float3(3.2, 1.2, 1.2), saturate(c.a * 1.35 * Opacity));
"""


def _import_textures(files, folder, prefix):
    tasks = []
    for name, path in files.items():
        t = unreal.AssetImportTask()
        t.filename = path
        t.destination_path = folder
        t.destination_name = prefix + name
        t.automated = True
        t.replace_existing = True
        t.save = False
        tasks.append(t)
    if tasks:
        AT.import_asset_tasks(tasks)
    out = {}
    for name in files:
        tex = unreal.load_asset("%s/%s%s" % (folder, prefix, name))
        if tex:
            if name.endswith("_normal"):
                tex.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_NORMALMAP)
                tex.set_editor_property("srgb", False)
            out[name] = tex
    return out


def build_blood(files, log):
    """GTA IV's blood decals -> textures, M_GTAIV_BloodDecal (Tex, Atlas, Opacity) and M_GTAIV_BloodDrop."""
    folder = WEAPONS + "/Blood"
    tex = _import_textures(files, folder, "T_GTAIV_")
    if "blood_01" not in tex:
        return {}
    MEL = unreal.MaterialEditingLibrary
    path = folder + "/M_GTAIV_BloodDecal"
    if EAL.does_asset_exist(path):
        EAL.delete_asset(path)
    mat = AT.create_asset("M_GTAIV_BloodDecal", folder, unreal.Material, unreal.MaterialFactoryNew())
    mat.set_editor_property("material_domain", unreal.MaterialDomain.MD_DEFERRED_DECAL)
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    custom = MEL.create_material_expression(mat, unreal.MaterialExpressionCustom, -300, 0)
    custom.set_editor_property("code", BLOOD_HLSL)
    custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT4)
    inputs = []
    for n in ("UV", "Atlas", "Opacity", "Tex"):
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", n)
        inputs.append(ci)
    custom.set_editor_property("inputs", inputs)
    uv = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureCoordinate, -700, -200)
    MEL.connect_material_expressions(uv, "", custom, "UV")
    atlas = MEL.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -700, -50)
    atlas.set_editor_property("parameter_name", "Atlas")
    atlas.set_editor_property("default_value", unreal.LinearColor(1, 1, 0, 0))
    MEL.connect_material_expressions(atlas, "RGBA", custom, "Atlas")
    op = MEL.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -700, 100)
    op.set_editor_property("parameter_name", "Opacity")
    op.set_editor_property("default_value", 1.0)
    MEL.connect_material_expressions(op, "", custom, "Opacity")
    tobj = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureObjectParameter, -700, 220)
    tobj.set_editor_property("parameter_name", "Tex")
    tobj.set_editor_property("texture", tex["blood_01"])
    MEL.connect_material_expressions(tobj, "", custom, "Tex")
    rgb = MEL.create_material_expression(mat, unreal.MaterialExpressionComponentMask, -100, -50)
    rgb.set_editor_properties({"r": True, "g": True, "b": True, "a": False})
    alpha = MEL.create_material_expression(mat, unreal.MaterialExpressionComponentMask, -100, 80)
    alpha.set_editor_properties({"r": False, "g": False, "b": False, "a": True})
    MEL.connect_material_expressions(custom, "", rgb, "")
    MEL.connect_material_expressions(custom, "", alpha, "")
    MEL.connect_material_property(rgb, "", unreal.MaterialProperty.MP_BASE_COLOR)
    MEL.connect_material_property(alpha, "", unreal.MaterialProperty.MP_OPACITY)
    rough = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -100, 200)
    rough.set_editor_property("r", 0.45)
    MEL.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    MEL.recompile_material(mat)
    EAL.save_loaded_asset(mat)

    path = folder + "/M_GTAIV_BloodDrop"
    if EAL.does_asset_exist(path):
        EAL.delete_asset(path)
    drop = AT.create_asset("M_GTAIV_BloodDrop", folder, unreal.Material, unreal.MaterialFactoryNew())
    col = MEL.create_material_expression(drop, unreal.MaterialExpressionConstant3Vector, -300, 0)
    col.set_editor_property("constant", unreal.LinearColor(0.09, 0.0, 0.0, 1.0))
    MEL.connect_material_property(col, "", unreal.MaterialProperty.MP_BASE_COLOR)
    r2 = MEL.create_material_expression(drop, unreal.MaterialExpressionConstant, -300, 150)
    r2.set_editor_property("r", 0.08)
    MEL.connect_material_property(r2, "", unreal.MaterialProperty.MP_ROUGHNESS)
    drop.set_editor_property("used_with_instanced_static_meshes", True)
    MEL.recompile_material(drop)
    EAL.save_loaded_asset(drop)
    EAL.save_directory(folder, only_if_is_dirty=False)
    log("LOG Blood: GTA IV decals (%s)" % ", ".join(sorted(tex)))
    return {"blood_decal": mat, "blood_splat": tex["blood_01"],
            "blood_pools": [tex[n] for n in ("blood_pool_a", "blood_pool_b") if n in tex],
            "blood_drop": drop, "blood_drop_mesh": unreal.load_asset("/Engine/BasicShapes/Sphere")}


VOICES = CHARS + "/Voices"
VOICE_DATA = VOICES + "/DA_GTAIV_Voices"


def build_voices(root_dir, voices, log):
    """Ped voice lines (speech.rpf) -> SoundWaves under Characters/Voices and DA_GTAIV_Voices."""
    if not voices or not voices.get("voices"):
        return None
    tasks, wanted = [], []
    def add(rel, folder):
        name = "S_" + os.path.splitext(os.path.basename(rel))[0]
        t = unreal.AssetImportTask()
        t.filename = os.path.join(root_dir, rel)
        t.destination_path = folder
        t.destination_name = name
        t.automated = True
        t.replace_existing = True
        t.save = False
        tasks.append(t)
        return "%s/%s" % (folder, name)
    plan = {}
    for model, v in voices["voices"].items():
        folder = "%s/%s" % (VOICES, model)
        plan[model] = {k: [add(r, folder) for r in v.get(k, [])] for k in ("panic", "scream", "fight")}
    pain_plan = {}
    for sex, sets in voices.get("pain", {}).items():
        pain_plan[sex] = {}
        for k, rels in sets.items():
            paths = []
            for r in rels:
                bank = os.path.basename(os.path.dirname(r)).lstrip("_")
                paths.append(add(r, "%s/_pain/%s" % (VOICES, bank)))
            pain_plan[sex][k] = paths
    for i in range(0, len(tasks), 200):
        AT.import_asset_tasks(tasks[i:i + 200])
    load = lambda paths: [s for s in (unreal.load_asset(p) for p in paths) if s]
    if EAL.does_asset_exist(VOICE_DATA):
        da = unreal.load_asset(VOICE_DATA)
    else:
        f = unreal.DataAssetFactory()
        f.set_editor_property("data_asset_class", unreal.GTAIVVoiceData)
        da = AT.create_asset("DA_GTAIV_Voices", VOICES, unreal.GTAIVVoiceData, f)
    vmap = unreal.Map(unreal.Name, unreal.GTAIVVoice)
    for model, lines in plan.items():
        v = unreal.GTAIVVoice()
        v.set_editor_property("female", bool(voices["voices"][model].get("female")))
        v.set_editor_property("panic", load(lines["panic"]))
        v.set_editor_property("scream", load(lines["scream"]))
        v.set_editor_property("fight", load(lines["fight"]))
        vmap[model] = v
    def pain_set(sex):
        ps = unreal.GTAIVPainSet()
        p = pain_plan.get(sex, {})
        for k in ("pain_high", "pain_low", "death_high", "death_low"):
            ps.set_editor_property(k, load(p.get(k, [])))
        return ps
    da.set_editor_properties({"voices": vmap, "pain_male": pain_set("male"), "pain_female": pain_set("female")})
    EAL.save_loaded_asset(da, only_if_is_dirty=False)
    EAL.save_directory(VOICES, only_if_is_dirty=True)
    log("LOG Voices: %d ped voices (%d lines), pain and death cries" % (len(plan), len(tasks)))
    return da


def muzzle_of(mesh):
    """Barrel end: the far end of the model's longest axis from its origin (the grip)."""
    if mesh is None:
        return unreal.Vector(15, 0, 3)
    b = mesh.get_bounding_box()
    lo, hi = b.min, b.max
    ext = [(hi.x - lo.x, 0), (hi.y - lo.y, 1), (hi.z - lo.z, 2)]
    axis = max(ext)[1]
    lo_l, hi_l = [lo.x, lo.y, lo.z], [hi.x, hi.y, hi.z]
    c = [(lo_l[i] + hi_l[i]) / 2 for i in range(3)]
    c[axis] = hi_l[axis] if abs(hi_l[axis]) >= abs(lo_l[axis]) else lo_l[axis]
    return unreal.Vector(*c)


def build_data(clips, seqs, sounds, weapon_mesh, icon, log, kit=None):
    if EAL.does_asset_exist(DATA):
        da = unreal.load_asset(DATA)
    else:
        f = unreal.DataAssetFactory()
        f.set_editor_property("data_asset_class", unreal.GTAIVWeaponData)
        da = AT.create_asset("DA_GTAIV_Pistol", WEAPONS, unreal.GTAIVWeaponData, f)
    mesh = unreal.load_asset(weapon_mesh) if weapon_mesh and EAL.does_asset_exist(weapon_mesh) else None
    tex = unreal.load_asset(icon) if icon and EAL.does_asset_exist(icon) else None
    clip_map = unreal.Map(unreal.Name, unreal.GTAIVAnimClip)
    for k, v in clips.items():
        clip_map[k] = v
    seq_map = unreal.Map(unreal.Name, unreal.AnimSequence)
    for k, v in seqs.items():
        seq_map[k] = v
    impacts = [sounds[k] for k in sorted(sounds) if k.startswith("pistol_impact")]
    da.set_editor_properties({
        "mesh": mesh, "icon": tex, "muzzle": muzzle_of(mesh), "clips": clip_map, "sequences": seq_map,
        "shot_sound": sounds.get("pistol_shot"), "shot_low_sound": sounds.get("pistol_lows"),
        "echo_sound": sounds.get("pistol_echo"), "dry_sound": sounds.get("pistol_dry"),
        "clip_in_sound": sounds.get("handgun_clip_in"), "cock_sound": sounds.get("handgun_cock"),
        "impact_sounds": impacts,
    })
    if kit and kit.get("blood_assets"):
        da.set_editor_properties(kit["blood_assets"])
    if kit and kit.get("grip"):
        da.set_editor_property("grip", grip_transform(kit, LAST_BAKER))
    if kit and kit.get("muzzle"):
        m = kit["muzzle"]
        mirror = LAST_BAKER.mirror if LAST_BAKER else True
        da.set_editor_property("muzzle", unreal.Vector(m[0] * 100, (-m[1] if mirror else m[1]) * 100, m[2] * 100))
    EAL.save_loaded_asset(da, only_if_is_dirty=False)
    log("LOG Pistol: %s, %d clips, %d sounds" % ("w_glock" if mesh else "no model", len(clips), len(sounds)))
    return da


def add_player_component(log):
    """The GTA IV Player component on the GASP ragdoll character (once; the original asset is
    backed up first), and GM_GTAIV spawns that character as the player."""
    if not EAL.does_asset_exist(PLAYER_BP):
        log("LOG %s not found: add the GTA IV Player component to your character yourself" % PLAYER_BP)
        return
    bp = unreal.load_asset(PLAYER_BP)
    sds = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    handles = sds.k2_gather_subobject_data_for_blueprint(bp)
    for h in handles:
        obj = unreal.SubobjectDataBlueprintFunctionLibrary.get_object(unreal.SubobjectDataBlueprintFunctionLibrary.get_data(h))
        if obj and obj.get_class().get_name() == "GTAIVPlayerComponent":
            break
    else:
        content = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_content_dir())
        src = os.path.join(content, "Blueprints", "SandboxCharacter_Mover_Ragdoll.uasset")
        bak = os.path.join(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir()),
                           "GTAIVExporter_backup", "SandboxCharacter_Mover_Ragdoll.uasset")
        if os.path.isfile(src) and not os.path.isfile(bak):
            os.makedirs(os.path.dirname(bak), exist_ok=True)
            shutil.copy2(src, bak)
        params = unreal.AddNewSubobjectParams(parent_handle=handles[0], new_class=unreal.GTAIVPlayerComponent,
                                              blueprint_context=bp)
        handle, fail = sds.add_new_subobject(params)
        if not fail.is_empty():
            log("LOG Couldn't add the GTA IV Player component: %s" % fail)
            return
        sds.rename_subobject(handle, unreal.Text("GTAIV_Player"))
        unreal.BlueprintEditorLibrary.compile_blueprint(bp)
        EAL.save_loaded_asset(bp, only_if_is_dirty=False)
        log("LOG Added the GTA IV Player component to SandboxCharacter_Mover_Ragdoll (backup in Saved/GTAIVExporter_backup)")
    if EAL.does_asset_exist(GAME_MODE):
        gm = unreal.load_asset(GAME_MODE)
        cdo = unreal.get_default_object(gm.generated_class())
        cdo.set_editor_property("default_pawn_class", bp.generated_class())
        EAL.save_loaded_asset(gm, only_if_is_dirty=False)
        log("LOG GM_GTAIV's player is now SandboxCharacter_Mover_Ragdoll")


def setup(export_dir, kit, log):
    """kit (from the exporter): {"anims": rel path, "sounds": {name: rel path}, "weapon": mesh name,
    "icon": texture name}"""
    clips, seqs = build_clips(os.path.join(export_dir, kit["anims"]), log)
    sounds = import_sounds({k: os.path.join(export_dir, v) for k, v in kit.get("sounds", {}).items()}, log)
    build_voices(export_dir, kit.get("voices"), log)
    kit = dict(kit)
    kit["blood_assets"] = build_blood({k: os.path.join(export_dir, v) for k, v in kit.get("blood", {}).items()}, log)
    build_data(clips, seqs, sounds, "%s/Meshes/%s" % (ROOT, kit["weapon"]) if kit.get("weapon") else None,
               "%s/Textures/%s" % (ROOT, kit["icon"]) if kit.get("icon") else None, log, kit)
    add_player_component(log)
