"""GTA IV cutscenes (pc/anim/cuts.img, cutsprops.img; audio in pc/audio/sfx/cutscenes.rpf).

<name>.cut is text: one or more [CUTSCENE_HEADER] blocks, each with the scene's origin
([OFFSET] x y z in metres; [ORIENT] is a heading the clips are already turned by), its [MODELS] (index, model, animation
clip, face clip), the [ANIM] dictionaries (<name>_<section>.wad, one per section: a block with
several plays them back to back, clip names ending in the section number), the [CAMERA] clip,
[AUDIO] and [TEXT] (subtitles: start ms, duration ms, GXT key in [MISSION_TEXT_NAME]'s table).

Clip tracks (anims.Wad layout): kinds 0/1 bone translation/rotation, 5/6 the ped's position and
heading in the scene (its mover), 7/8 the camera's position/rotation and 136 its vertical field
of view. Props and vehicles move with their root bone's 0/1 tracks. Positions are metres in the
scene: world = OFFSET + local.

Written for each cutscene: cutscene_<name>.json (shots with per-frame camera and actor
placements, subtitles) and the actors' body animations (clip_<name>_<n>.json, the anims.json
format the importer bakes onto the ped skeleton), plus <name>.wav.
"""

import json
import os
import re
import struct

import numpy as np

from . import anims, peds
from .audio import Rpf3Archive, decode_stream, write_wav
from .gxt import Text
from .img import ImgArchive
from .mapdata import joaat

FPS = 30.0
CUTS = os.path.join("pc", "anim", "cuts.img")
CUTSPROPS = os.path.join("pc", "anim", "cutsprops.img")
_TOKEN = re.compile(r"~[a-zA-Z0-9_]+~")


def _tag(block, name):
    m = re.search(r"\[%s\](.*?)\[/%s\]" % (name, name), block, re.S)
    return m.group(1).strip() if m else ""


def _section(block):
    models = []
    for line in _tag(block, "MODELS").splitlines():
        f = line.split()
        if len(f) >= 3 and f[0].isdigit():
            models.append({"index": int(f[0]), "model": f[1].lower(), "clip": f[2].lower(),
                           "face": f[3].lower() if len(f) > 3 and not f[3].isdigit() else None})
    vehicles = {}
    for line in _tag(block, "VEHICLE_DETAILS").splitlines():
        f = line.split()
        if f and f[0].isdigit():
            vehicles[int(f[0])] = [int(v) for v in f[1:] if v.lstrip("-").isdigit()]
    return {"offset": [float(v) for v in _tag(block, "OFFSET").split()[:3]] or [0.0, 0.0, 0.0],
            "orient": float((_tag(block, "ORIENT").split() or ["0"])[0]), "models": models,
            "anim": (_tag(block, "ANIM").split() or [None])[0],
            "camera": (_tag(block, "CAMERA").split() or ["camera_0"])[0].lower(),
            "audio": (_tag(block, "AUDIO").split() or [None])[0], "vehicles": vehicles, "remove": _tag(block, "REMOVE"),
            "lights": [l.strip().lower() for l in _tag(block, "LIGHTS").splitlines() if l.strip()]}


def parse_cut(text):
    """-> [header block]: {text [(start ms, duration ms, key)], text_table, sections [{offset,
    orient, models [{model, clip, face}], anim (wad), camera, audio, vehicles, remove}]}.
    Subtitle times count from the start of their header block."""
    table = _tag(text, "MISSION_TEXT_NAME")
    out = []
    for block in re.split(r"(?=\[CUTSCENE_HEADER\])", text):
        if "[SECTION_START]" not in block:
            continue
        subs = []
        for line in _tag(block, "TEXT").splitlines():
            f = line.split()
            if len(f) >= 3 and f[0].isdigit():
                subs.append((int(f[0]), int(f[1]), f[2]))
        sections = [_section(sb) for sb in re.findall(r"\[SECTION_START\](.*?)\[SECTION_END\]", block, re.S)]
        out.append({"text": subs, "text_table": table, "sections": [x for x in sections if x["anim"]]})
    return out


def clip_tracks(wad, name):
    """Every track of a clip (anims.Wad.clip keeps only bone tracks):
    -> (frames, {(kind, bone): array [frames, n]}) or None."""
    p = wad.clips.get(joaat(name.lower()))
    if p is None:
        return None
    s = wad.s
    frames, block = wad._u16(p + 8), wad._u16(p + 0xA)
    nseq = max(1, wad._u16(p + 0x18))
    trk, nt = wad._p(wad._u32(p + 0x20)), wad._u16(p + 0x24)
    out = {}
    for t in range(nt):
        td = wad._p(wad._u32(trk + 4 * t))
        kind, bone = s[td], wad._u16(td + 2)
        lst = wad._p(wad._u32(td + 8))
        parts = []
        for q in range(nseq):
            d = wad._p(wad._u32(lst + 4 * q))
            fmt = s[d + 1]
            n = frames if nseq == 1 else min(block, frames - q * (block - 1))
            chans = [wad._p(wad._u32(d + 4 + 4 * c)) for c in range(wad._u32(d + 0x14))]
            try:
                if fmt in (0, 1, 2):
                    k = {0: 3, 1: 4, 2: 1}[fmt]
                    v = np.tile(np.frombuffer(s, "<f4", k, wad._p(wad._u32(chans[0] + 8))).astype(float), (n, 1))
                elif fmt in (16, 17, 18):
                    v = np.stack([wad._channel(c, n) for c in chans], 1)
                else:
                    v = None
            except Exception:
                v = None
            parts.append(v)
        if any(v is None for v in parts):
            continue
        v = parts[0] if nseq == 1 else np.concatenate([parts[0]] + [x[1:] for x in parts[1:]])
        if len(v) < frames:   # a track a frame or two short: hold its last value
            v = np.concatenate([v, np.repeat(v[-1:], frames - len(v), 0)])
        out[(kind, bone)] = v[:frames]
    return frames, out


def stereo(pcm):
    """Cutscene streams are surround (3 channels: L R C, 5: L R C Ls Rs, the centre carrying the
    dialogue); Unreal imports 1, 2, 4, 6 or 8. Fold to stereo the usual way (-3 dB centre and rears)."""
    n = pcm.shape[1]
    if n <= 2:
        return pcm
    x = pcm.astype(np.float64)
    left, right = x[:, 0].copy(), x[:, 1].copy()
    if n >= 3:
        left += 0.707 * x[:, 2]
        right += 0.707 * x[:, 2]
    if n >= 5:
        left += 0.707 * x[:, 3]
        right += 0.707 * x[:, 4]
    out = np.stack([left, right], 1)
    peak = np.abs(out).max()
    if peak > 32000:
        out *= 32000.0 / peak
    return out.astype(np.int16)


def _qmat(q):
    return anims._quat_mat(np.asarray(q, float))


def _rz(deg):
    a = np.radians(deg)
    return np.array([[np.cos(a), -np.sin(a), 0], [np.sin(a), np.cos(a), 0], [0, 0, 1]])


def _row(m):
    """4x4 GTA-space placement -> [x, y, z, qw, qx, qy, qz] (the importer's placement rows)."""
    x, y, z, w = anims._mat_quat(m[:3, :3])
    return [round(float(v), 4) for v in m[:3, 3]] + [round(w, 6), round(x, 6), round(y, 6), round(z, 6)]


# a ped's mesh is exported turned 180 degrees and lifted a metre (peds.TO_UNREAL): undo it so the
# mesh lands where the game's ped (origin at the hip) stands
_PED_FROM_MESH = np.linalg.inv(peds.TO_UNREAL)


class CutsceneExporter:
    def __init__(self, game_dir, key, out_dir, log=print):
        self.game_dir, self.key, self.out_dir, self.log = game_dir, key, out_dir, log
        self.cuts = ImgArchive(os.path.join(game_dir, CUTS), key)
        self._wads = {}
        self._sk = None
        self._text = None
        self.models = {}    # model name -> "ped" / "prop" / "vehicle" (what the cutscenes use)
        os.makedirs(out_dir, exist_ok=True)

    def text(self):
        if self._text is None:
            path = os.path.join(self.game_dir, "common", "text", "american.gxt")
            self._text = Text(open(path, "rb").read()) if os.path.isfile(path) else None
        return self._text

    def _model_rig(self, model):
        """The skeleton a character's cutscene clips were made for. The story characters' cutscene
        rigs (cs_*) hang the spine off the pelvis, the game's peds (and Niko) off the root: the
        tracks are relative to those parents, so baking a cs_ clip on the game's rig bends the
        body over and twists the chest against the hips."""
        if not hasattr(self, "_rigs"):
            self._rigs = {}
        if model in self._rigs:
            return self._rigs[model]
        sk = self.skeleton()
        rig = sk
        if model.startswith("cs_"):
            try:
                from .peds import PedArchive, read_skeleton
                from .rsc import Resource
                pa = PedArchive(self.game_dir, self.key, cutscenes=True)
                try:
                    raw = pa.read(model + ".wft")
                finally:
                    pa.close()
                spine_on_pelvis = False
                if raw is not None:
                    res = Resource(raw)
                    main = res.ptr(0xB4)
                    own = read_skeleton(res, res.ptr(main + 0x0C))
                    if "Char_Spine" in own.names:
                        par = own.parents[own.names.index("Char_Spine")]
                        spine_on_pelvis = par >= 0 and own.names[par] == "Char_Pelvis"
                if spine_on_pelvis:
                    import copy
                    rig = copy.deepcopy(sk)
                    i, pv = rig.names.index("Char_Spine"), rig.names.index("Char_Pelvis")
                    rig.parents[i] = pv
                    rig.local[i] = np.linalg.inv(rig.world[pv]) @ rig.world[i]
                    rig.ids = sk.ids
            except Exception as e:
                self.log("Cutscene rig of %s: %s" % (model, e))
        self._rigs[model] = rig
        return rig

    def _clip_rig(self, model, frames, tracks):
        """The rig a clip was made for: the one under which the chest lines up with the hips.
        (Background characters in cutscenes use either: a clip on the wrong rig turns the chest
        against the hips, whatever the character is doing.)"""
        import copy
        game = self.skeleton()
        cut = self._model_rig(model)
        if cut is game:
            cut = copy.deepcopy(game)
            i, pv = cut.names.index("Char_Spine"), cut.names.index("Char_Pelvis")
            cut.parents[i] = pv
            cut.local[i] = np.linalg.inv(cut.world[pv]) @ cut.world[i]
            cut.ids = game.ids
        bones = {k: v for k, v in tracks.items() if k[0] in (0, 1) and k[1] in game.ids and k[1] != 0}
        sample = sorted({0, frames // 4, frames // 2, 3 * frames // 4, frames - 1})
        sub = {k: v[sample] for k, v in bones.items()}
        best, rig = None, game
        for cand in (game, cut):
            m = anims.bake(cand, len(sample), sub, False)
            n = cand.names
            tw = []
            for fr in m:
                hips = fr[n.index("Char_L_Thigh")][:3, 3] - fr[n.index("Char_R_Thigh")][:3, 3]
                chest = fr[n.index("Char_L_UpperArm")][:3, 3] - fr[n.index("Char_R_UpperArm")][:3, 3]
                c = np.dot(hips, chest) / max(1e-9, np.linalg.norm(hips) * np.linalg.norm(chest))
                tw.append(np.degrees(np.arccos(np.clip(c, -1, 1))))
            score = float(np.median(tw))
            if best is None or score < best:
                best, rig = score, cand
        return rig

    def skeleton(self):
        if self._sk is None:
            self._sk = anims.reference_skeleton(self.game_dir, self.key)
        return self._sk

    def wad(self, name):
        if name not in self._wads:
            raw = self.cuts.read(name + ".wad")
            self._wads[name] = anims.Wad(raw) if raw else None
        return self._wads[name]

    def subtitle(self, key, table):
        t = self.text()
        s = t.get(key, table) if t else None
        if not s:
            return None
        s = s.replace("~n~", "\n")
        return _TOKEN.sub("", s).strip()

    @staticmethod
    def kind_of(model, vehicle_models):
        if model == "player" or model.startswith(("cs_", "m_", "f_", "ig_")):
            return "ped"
        if model in vehicle_models:
            return "vehicle"
        return "prop"

    def _clip_json(self, sk, frames, tracks):
        """A ped's body animation in the anims.json clip format (no root motion: the shot
        places the ped with its mover track)."""
        bone_tracks = {k: v for k, v in tracks.items() if k[0] in (0, 1) and k[1] in sk.ids and k[1] != 0}
        mats = anims.bake(sk, frames, bone_tracks, False)
        names = list(sk.names)
        animated = sorted({names[sk.ids[b]] for (_k, b) in bone_tracks})
        return {"frames": frames, "seconds": frames / FPS, "root": False, "animated": animated,
                "pose": [[[*(np.round(m[:3, 3], 4).tolist()), *[round(c, 5) for c in anims._mat_quat(m[:3, :3])]]
                          for m in fr] for fr in mats]}

    def _clip_header(self):
        import copy
        sk = self.skeleton()
        ours = copy.deepcopy(sk)
        peds.fix_twist_bones(ours)
        peds.add_ground_root(ours)
        rest = [peds.TO_UNREAL @ w for w in sk.world]
        return {"fps": FPS, "bones": list(sk.names), "skeleton": {"names": ours.names, "parents": ours.parents},
                "rest": [[*(r[:3, 3].tolist()), *anims._mat_quat(r[:3, :3])] for r in rest]}

    def export(self, name, vehicle_models=(), audio=None):
        """Writes cutscene_<name>.json (+ clip files and <name>.wav). Returns its manifest."""
        raw = self.cuts.read(name + ".cut")
        if raw is None:
            raise RuntimeError("no cutscene %s" % name)
        blocks = parse_cut(raw.decode("latin-1"))
        sk = self.skeleton()
        header = self._clip_header()
        shots, subs, t0 = [], [], 0.0
        n_clip = 0
        for b in blocks:
            block_start = t0
            for sec in b["sections"]:
                # the clips are already in world directions: only OFFSET moves them (ORIENT is the
                # scene's heading for the game, not a turn of the data: applying it floats the
                # Platypus scenes off the ship and parks Roman's car 10 m off the road)
                origin = np.eye(4)
                origin[:3, 3] = sec["offset"]
                wad = self.wad(sec["anim"])
                if wad is None:
                    self.log("Cutscene %s: no %s.wad" % (name, sec["anim"]))
                    continue
                cam = clip_tracks(wad, sec["camera"])
                if cam is None:
                    self.log("Cutscene %s: no camera in %s" % (name, sec["anim"]))
                    continue
                frames, ct = cam
                pos, rot = ct.get((7, 0)), ct.get((8, 0))
                fov = ct.get((136, 0))
                camera = []
                for f in range(frames):
                    m = np.eye(4)
                    m[:3, :3] = _qmat(rot[min(f, len(rot) - 1)]) if rot is not None else np.eye(3)
                    m[:3, 3] = pos[min(f, len(pos) - 1)] if pos is not None else 0.0
                    w = origin @ m
                    fwd, up = w[:3, 1], w[:3, 2]   # GTA cameras look down their local Y, Z up
                    camera.append([round(float(v), 4) for v in (*w[:3, 3], *fwd, *up)]
                                  + [round(float(fov[min(f, len(fov) - 1)][0]) if fov is not None else 45.0, 3)])
                actors = []
                for mdl in sec["models"]:
                    got = clip_tracks(wad, mdl["clip"])
                    if got is None:
                        continue
                    nf, tr = got
                    kind = self.kind_of(mdl["model"], vehicle_models)
                    self.models.setdefault(mdl["model"], kind)
                    rows = []
                    for f in range(frames):
                        g = min(f, nf - 1)
                        m = np.eye(4)
                        if kind == "ped":
                            p, q = tr.get((5, 0)), tr.get((6, 0))
                        else:
                            p, q = tr.get((0, 0)), tr.get((1, 0))
                        if q is not None:
                            m[:3, :3] = _qmat(q[min(g, len(q) - 1)])
                        if p is not None:
                            m[:3, 3] = p[min(g, len(p) - 1)]
                        w = origin @ m
                        if kind == "ped":
                            w = w @ _PED_FROM_MESH
                        rows.append(_row(w))
                    actor = {"model": mdl["model"], "kind": kind, "root": rows}
                    if kind == "vehicle" and mdl["index"] in sec["vehicles"]:
                        actor["colours"] = sec["vehicles"][mdl["index"]]
                    if kind == "ped":
                        fn = "clip_%s_%d.json" % (name, n_clip)
                        n_clip += 1
                        data = dict(header)
                        data["clips"] = {"body": self._clip_json(self._clip_rig(mdl["model"], nf, tr), nf, tr)}
                        with open(os.path.join(self.out_dir, fn), "w", encoding="utf-8") as fh:
                            json.dump(data, fh, separators=(",", ":"))
                        actor["clip"] = fn
                    actors.append(actor)
                # the scene's own lights (spt_ spots, fil_ fills, rim_ rims): where they are and where
                # they point each frame, and their colour
                lights = []
                for ln in sec.get("lights", []):
                    got = clip_tracks(wad, ln)
                    if got is None:
                        continue
                    nf, tr = got
                    p, d, c = tr.get((0, 0)), tr.get((143, 0)), tr.get((139, 0))
                    if p is None:
                        continue
                    track = []
                    for f in range(frames):
                        g = min(f, len(p) - 1)
                        wp = origin @ np.array([*p[g], 1.0])
                        wd = origin[:3, :3] @ (d[min(f, len(d) - 1)] if d is not None else np.array([0.0, 0.0, -1.0]))
                        track += [round(float(v), 4) for v in (*wp[:3], *wd)]
                    lights.append({"name": ln, "type": ln.split("_")[0], "track": track,
                                   "colour": [round(float(v), 3) for v in (c[0] if c is not None else (1, 1, 1))]})
                shots.append({"frames": frames, "start": round(t0, 3), "camera": camera, "actors": actors,
                              "origin": _row(origin), "lights": lights})
                t0 += frames / FPS
            for start, dur, key in b["text"]:
                s = self.subtitle(key, b["text_table"])
                if s:
                    subs.append([round(block_start + start / 1000.0, 3), round(dur / 1000.0, 3), s])
        manifest = {"name": name, "fps": FPS, "duration": round(t0, 3), "shots": shots, "subtitles": subs,
                    "audio": None}
        track = audio or next((sec["audio"] for b in blocks for sec in b["sections"] if sec["audio"]), None)
        if track:
            wav = self._audio(track)
            if wav:
                manifest["audio"] = wav
        path = os.path.join(self.out_dir, "cutscene_%s.json" % name)
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(manifest, fh, separators=(",", ":"))
        self.log("Cutscene %s: %d shots, %.0fs, %d actors, %d subtitles%s" % (
            name, len(shots), t0, len({a["model"] for s in shots for a in s["actors"]}), len(subs),
            ", audio" if manifest["audio"] else ""))
        return manifest

    def _audio(self, track):
        path = os.path.join(self.game_dir, "pc", "audio", "sfx", "cutscenes.rpf")
        if not os.path.isfile(path):
            return None
        rpf = Rpf3Archive(path, self.key)
        try:
            h = joaat(track.lower())
            if h not in rpf.entries:
                self.log("Cutscene audio %s not found" % track)
                return None
            rate, pcm = decode_stream(rpf.read(h))
        finally:
            if hasattr(rpf, "close"):
                rpf.close()
        fn = "%s.wav" % track.lower()
        write_wav(os.path.join(self.out_dir, fn), rate, stereo(pcm))
        return fn

    def close(self):
        if hasattr(self.cuts, "close"):
            self.cuts.close()
