"""GTA IV animations: anim.img's .wad dictionaries of crAnimation clips, e.g. gun@handgun.wad.

wad (RSC): +0x10 hash array, +0x14 u16 count, +0x18 crAnimation pointer array (hash = joaat(name))
crAnimation: +0x08 u16 frames, +0x0A u16 frames per sequence block, +0x0C f32 seconds,
    +0x14 sequence list, +0x18 u16 sequence count, +0x20 track list, +0x24 u16 track count
track: u8 type (0 bone position, 1 bone rotation, 5/6 root motion, 128+ events), u8 ?,
    u16 bone id (skeleton+0x20 maps ids to bones), u32 ?, +0x08 per-sequence data list
sequence data: u8 type, u8 format (0 static vec3, 1 static quat, 16 three channels,
    17 four channels xyzw), +0x04 channel pointers, +0x14 u32 channel count
channel: u32 vtable, u8 ?, u8 kind:
    1 raw floats (+8 pointer, +0xC u16 count), 4 constant (+8 f32),
    6 quantized (+8 pointer, +0xC bits, +0x10 count, +0x14 scale, +0x18 offset; values packed
      LSB first), 9 constant quaternion / 13 constant vector3 (+8 pointer to floats)
Rotations and positions are the bone's local transform in GTA's skeleton space.
"""

import json
import os
import struct

import numpy as np

from . import peds
from .img import ImgArchive
from .mapdata import joaat
from .rsc import Resource

FPS = 30.0

# the pistol set: (dictionary, clip, exported name)
PISTOL = (
    ("gun@handgun.wad", "fire", "fire"),
    ("gun@handgun.wad", "reload", "reload"),
    ("gun@handgun.wad", "unholster", "unholster"),
    ("gun@handgun.wad", "holster", "holster"),
    ("gun@handgun.wad", "fire_crouch", "fire_crouch"),
    ("gun@handgun.wad", "reload_crouch", "reload_crouch"),
)


# GTA IV's in-car animations (veh@<group>.wad): sitting, steering, looking back to reverse
VEHICLE_GROUPS = ("std", "low", "truck", "van", "bus")
VEHICLE_CLIPS = ("sit_drive", "steer_l", "steer_r", "reverse", "sit_pass")
# getting in and out: they keep the root bone's turn and bob (the game slides the ped between the
# door and the seat itself)
VEHICLE_ROOT_CLIPS = ("get_in_ds", "get_out_ds", "jump_out_ds", "d_open_out_ds", "d_close_in_ds", "d_close_out_ds")
# pulling a driver out: the jacker's clip has no name in the game's files (its hash), the victim's
# is jack_driver_ds of the same set
JACK_SETS = {"van": "veh@van_jack_unarmed.wad"}
JACKER_CLIP = 0xA767E715


def vehicle_clips(groups=VEHICLE_GROUPS):
    """(dictionary, clip, exported name "<group>/<clip>"[, root motion]) for the in-car animations."""
    out = [("veh@%s.wad" % g, c, "%s/%s" % (g, c)) for g in groups for c in VEHICLE_CLIPS]
    out += [("veh@%s.wad" % g, c, "%s/%s" % (g, c), True) for g in groups for c in VEHICLE_ROOT_CLIPS]
    for g in groups:
        jack = JACK_SETS.get(g, "veh@std_jack_unarmed.wad")
        out += [(jack, JACKER_CLIP, "%s/jack" % g, True), (jack, "jack_driver_ds", "%s/jacked" % g, True)]
    return tuple(out)


class Wad:
    def __init__(self, data):
        self.s = Resource(data).sys
        n = self._u16(0x14)
        hp, vp = self._p(self._u32(0x10)), self._p(self._u32(0x18))
        self.clips = {self._u32(hp + 4 * i): self._p(self._u32(vp + 4 * i)) for i in range(n)}

    @staticmethod
    def _p(v):
        return v - 0x50000000 if v >> 28 == 5 else None

    def _u32(self, o):
        return struct.unpack_from("<I", self.s, o)[0]

    def _u16(self, o):
        return struct.unpack_from("<H", self.s, o)[0]

    def _f32(self, o):
        return struct.unpack_from("<f", self.s, o)[0]

    def _channel(self, ch, frames):
        s, kind = self.s, self.s[ch + 5]
        if kind == 4:
            return np.full(frames, self._f32(ch + 8))
        if kind == 1:
            ptr, n = self._p(self._u32(ch + 8)), self._u16(ch + 0xC)
            v = np.frombuffer(s, "<f4", n, ptr).astype(float)
        elif kind == 6:
            ptr, bits, n = self._p(self._u32(ch + 8)), self._u32(ch + 0xC), self._u32(ch + 0x10)
            scale, offset = self._f32(ch + 0x14), self._f32(ch + 0x18)
            raw = np.frombuffer(s, np.uint8, min((bits * n + 7) // 8, len(s) - ptr), ptr)
            bitarr = np.unpackbits(raw, bitorder="little")
            idx = np.arange(n)[:, None] * bits + np.arange(bits)[None, :]
            v = (bitarr[idx].astype(np.int64) << np.arange(bits)).sum(1) * scale + offset
        else:
            raise ValueError("animation channel kind %d" % kind)
        return v if len(v) == frames else np.resize(v, frames)

    def clip(self, name):
        """-> (frames, seconds, {(track type, bone id): array [frames, 3 or 4]})"""
        p = self.clips.get(name if isinstance(name, int) else joaat(name))
        if p is None:
            return None
        s = self.s
        frames, block, seconds = self._u16(p + 8), self._u16(p + 0xA), self._f32(p + 0xC)
        nseq = max(1, self._u16(p + 0x18))
        trk, nt = self._p(self._u32(p + 0x20)), self._u16(p + 0x24)
        tracks = {}
        for t in range(nt):
            td = self._p(self._u32(trk + 4 * t))
            kind, bone = s[td], self._u16(td + 2)
            if kind not in (0, 1):
                continue
            lst = self._p(self._u32(td + 8))
            parts = []
            for q in range(nseq):
                d = self._p(self._u32(lst + 4 * q))
                fmt = s[d + 1]
                n = frames if nseq == 1 else min(block, frames - q * (block - 1))
                chans = [self._p(self._u32(d + 4 + 4 * c)) for c in range(self._u32(d + 0x14))]
                if fmt in (0, 1):
                    k = 3 if fmt == 0 else 4
                    v = np.tile(np.frombuffer(s, "<f4", k, self._p(self._u32(chans[0] + 8))).astype(float), (n, 1))
                elif fmt in (16, 17):
                    v = np.stack([self._channel(c, n) for c in chans], 1)
                else:
                    v = None
                parts.append(v)
            if any(v is None for v in parts):
                continue
            v = parts[0] if nseq == 1 else np.concatenate([parts[0]] + [x[1:] for x in parts[1:]])
            tracks[(kind, bone)] = v[:frames]
        return frames, seconds, tracks


def _quat_mat(q):
    x, y, z, w = q / np.linalg.norm(q)
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def _mat_quat(m):
    t = np.trace(m)
    if t > 0:
        r = np.sqrt(1 + t) * 2
        q = [(m[2, 1] - m[1, 2]) / r, (m[0, 2] - m[2, 0]) / r, (m[1, 0] - m[0, 1]) / r, 0.25 * r]
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        r = np.sqrt(1 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        q = [0.25 * r, (m[0, 1] + m[1, 0]) / r, (m[0, 2] + m[2, 0]) / r, (m[2, 1] - m[1, 2]) / r]
    elif m[1, 1] > m[2, 2]:
        r = np.sqrt(1 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        q = [(m[0, 1] + m[1, 0]) / r, 0.25 * r, (m[1, 2] + m[2, 1]) / r, (m[0, 2] - m[2, 0]) / r]
    else:
        r = np.sqrt(1 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
        q = [(m[0, 2] + m[2, 0]) / r, (m[1, 2] + m[2, 1]) / r, 0.25 * r, (m[1, 0] - m[0, 1]) / r]
    return [float(v) for v in q]


def reference_skeleton(game_dir, key, ped="m_y_business_01"):
    """GTA skeleton of the reference ped with bone ids, before our Unreal fix-ups."""
    pa = peds.PedArchive(game_dir, key)
    res = Resource(pa.read(ped + ".wft"))
    main = res.ptr(0xB4)
    skel = res.ptr(main + 0x0C)
    sk = peds.read_skeleton(res, skel)
    mp = res.ptr(skel + 0x20)
    sk.ids = {res.u16(mp + 4 * i): res.u16(mp + 4 * i + 2) for i in range(len(sk.names))}
    return sk


def bake(sk, frames, tracks, root_motion=False):
    """Model-space bone matrices per frame in the exported (Unreal-convention) skeleton:
    animated bones follow their GTA parent like in the game, the twist bones that GTA turns in
    code (no tracks) follow the parent our skeleton gives them; Char keeps its rest transform
    (root motion is not applied) and everything is turned/lifted like the exported meshes."""
    n = len(sk.names)
    index = {sk.names[i]: i for i in range(n)}
    by_bone = {}
    for (kind, bid), v in tracks.items():
        j = sk.ids.get(bid)
        if j is not None:
            by_bone.setdefault(j, {})[kind] = v
    fixed_parent = {index[b]: index[p] for b, p in peds.TWIST_PARENTS.items() if b in index and p in index}
    rest_world = sk.world
    out = np.zeros((frames, n, 4, 4))
    for f in range(frames):
        done = {}

        def world(i):
            if i in done:
                return done[i]
            tr = by_bone.get(i, {})
            if i in fixed_parent:  # driven by code in GTA; ours hang off the bone they twist with
                p = fixed_parent[i]
                m = world(p) @ (np.linalg.inv(rest_world[p]) @ rest_world[i])
            elif sk.parents[i] < 0:
                m = sk.local[i].copy()
                if root_motion:   # the root's own turn and bob (getting in and out of cars)
                    if 1 in tr:
                        m[:3, :3] = _quat_mat(tr[1][f])
                    if 0 in tr:
                        m[:3, 3] = tr[0][f]
            else:
                local = sk.local[i].copy()
                if 1 in tr:
                    local[:3, :3] = _quat_mat(tr[1][f])
                if 0 in tr:
                    local[:3, 3] = tr[0][f]
                m = world(sk.parents[i]) @ local
            done[i] = m
            return m
        for i in range(n):
            out[f, i] = peds.TO_UNREAL @ world(i)
    return out


def export_clips(game_dir, key, out_dir, clips=PISTOL, log=print, file_name="anims.json"):
    """Writes anims.json: the exported skeleton's rest pose and each clip's bone transforms per
    frame (model space, metres, Unreal-convention like the exported peds). Returns its path."""
    archive = ImgArchive(os.path.join(game_dir, "pc", "anim", "anim.img"), key)
    sk = reference_skeleton(game_dir, key)
    rest = [peds.TO_UNREAL @ w for w in sk.world]
    names = list(sk.names)
    wads = {}
    import copy
    ours = copy.deepcopy(sk)
    peds.fix_twist_bones(ours)
    peds.add_ground_root(ours)
    data = {"fps": FPS, "bones": names, "skeleton": {"names": ours.names, "parents": ours.parents},
            "rest": [[*(r[:3, 3].tolist()), *_mat_quat(r[:3, :3])] for r in rest], "clips": {}}
    for entry in clips:
        wad, clip, out_name = entry[:3]
        root_motion = len(entry) > 3 and entry[3]
        if wad not in wads:
            raw = archive.read(wad)
            wads[wad] = Wad(raw) if raw else None
        w = wads[wad]
        got = w.clip(clip) if w else None
        if got is None and wad in JACK_SETS.values():   # no van set: the standard one
            fallback = "veh@std_jack_unarmed.wad"
            if fallback not in wads:
                raw = archive.read(fallback)
                wads[fallback] = Wad(raw) if raw else None
            got = wads[fallback].clip(clip) if wads[fallback] else None
        if got is None:
            log("Animation %s/%s not found" % (wad, clip))
            continue
        frames, seconds, tracks = got
        mats = bake(sk, frames, tracks, root_motion)
        animated = sorted({names[sk.ids[b]] for (_k, b) in tracks if b in sk.ids})
        data["clips"][out_name] = {
            "source": "%s/%s" % (wad[:-4], clip), "frames": frames, "seconds": seconds, "root": bool(root_motion),
            "animated": animated,
            "pose": [[[*(m[:3, 3].tolist()), *_mat_quat(m[:3, :3])] for m in fr] for fr in mats],
        }
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, file_name)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f)
    log("Animations: %s" % ", ".join("%s (%.1fs)" % (k, v["seconds"]) for k, v in data["clips"].items()))
    return path
