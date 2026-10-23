"""GTA IV vehicles: models (vehicles.img), definitions (vehicles.ide), paint (carcols.dat)."""

import os
import re

import numpy as np

from . import drawable, img, texture
from .rsc import Resource

# vehicles.ide types that drive on roads in traffic
ROAD_TYPES = ("car", "bike")
# never in ambient traffic (emergency/service/special), even though they're road vehicles
NOT_TRAFFIC = re.compile(r"^(police|police2|noose|nstockade|fbi|ambulance|firetruk|pstockade|polpatriot|"
                         r"airtug|ripley|annihilator|bus|cablecar|subway|tram|trash|stockade|predator|tug|forklift|"
                         r"marquis|reefer|squalo|tropic|jetmax|dinghy|maverick|polmav|tourmav|cavalcade2)$")


class VehicleDef:
    __slots__ = ("name", "txd", "kind", "handling", "game_name", "frequency", "wheel_front", "wheel_rear",
                 "flags", "colors", "anim_group")

    def __init__(self, f):
        self.name = f[0].lower()
        self.txd = f[1].lower()
        self.kind = f[2].lower()
        self.handling = f[3].upper()
        self.game_name = f[4]
        self.frequency = _num(f[7], 0)
        self.wheel_front = _num(f[9], 0.3)
        self.wheel_rear = _num(f[10], 0.3)
        self.flags = f[15].lower() if len(f) > 15 else ""
        self.colors = []   # [(rgb1, rgb2, rgb3)] possible paint combos
        self.anim_group = f[5].lower() if len(f) > 5 else "veh@std"   # in-car animations, e.g. veh@low

    @property
    def in_traffic(self):
        return (self.kind in ROAD_TYPES and self.frequency > 0 and not NOT_TRAFFIC.match(self.name))


def _num(s, default):
    try:
        return float(s)
    except (TypeError, ValueError):
        return default


def _data_file(game_dir, rel):
    """Prefer GTAIV/update/... (patches, Fusion Fix) over the original."""
    for base in (os.path.join(game_dir, "update"), game_dir):
        p = os.path.join(base, rel)
        if os.path.isfile(p):
            return p
    return None


def _sections(path):
    section = None
    for line in open(path, encoding="latin-1"):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        if "," not in line:
            tok = line.lower()
            section = None if tok == "end" else tok
            continue
        yield section, [x.strip() for x in line.split(",")]


def load_vehicle_defs(game_dir):
    """{name: VehicleDef} with paint combos, and {txd: parent txd}."""
    defs, txd_parent = {}, {}
    ide = _data_file(game_dir, os.path.join("common", "data", "vehicles.ide"))
    if ide is None:
        return defs, txd_parent
    for section, f in _sections(ide):
        if section == "cars" and len(f) >= 11:
            v = VehicleDef(f)
            defs[v.name] = v
        elif section == "txdp" and len(f) >= 2:
            txd_parent[f[0].lower()] = f[1].lower()
    cols = _data_file(game_dir, os.path.join("common", "data", "carcols.dat"))
    if cols is not None:
        palette = []
        for section, f in _sections(cols):
            if section == "col" and len(f) >= 3:
                palette.append(tuple(int(_num(c, 0)) for c in f[:3]))
            elif section in ("car", "car3", "car4") and f and f[0].lower() in defs:
                per = {"car": 2, "car3": 3, "car4": 4}[section]
                idx = [int(_num(c, -1)) for c in f[1:] if c != ""]
                combos = []
                # "car3" = 2 body colours + a specular colour, "car4" = 3 body colours + specular;
                # paint3 materials use the 3rd body colour, or the primary when there isn't one
                for k in range(0, len(idx) - per + 1, per):
                    c = [palette[i] if 0 <= i < len(palette) else (128, 128, 128) for i in idx[k:k + per]]
                    body = c[:3] if per == 4 else [c[0], c[1], c[0]]
                    combos.append(tuple(body))
                defs[f[0].lower()].colors = combos
    return defs, txd_parent


class VehicleModel:
    """body: drawable.Mesh in model space (Y forward, Z up, metres)
    wheels: {mesh name: Mesh} in wheel-local space
    wheel_slots: [(bone name, (x, y, z), mesh name, right_side)]"""

    def __init__(self):
        self.body = None
        self.wheels = {}
        self.wheel_slots = []
        self.seats = {}    # seat bone (seat_dside_f = driver) -> (x, y, z)
        self.bones = {}    # bone name -> (index, (x, y, z))


WHEEL_BONES = ("wheel_lf", "wheel_rf", "wheel_lm1", "wheel_rm1", "wheel_lm2", "wheel_rm2", "wheel_lr", "wheel_rr")


def load_vehicle_model(data):
    """Read a vehicle .wft: body from the main fragment drawable, wheel meshes from the
    wheel children (stored once; the game reuses them for every wheel)."""
    res = Resource(data)
    main = res.ptr(0xB4)
    if main is None or res.ptr(main + 0x08) is None:
        return None
    out = VehicleModel()
    body = drawable.Mesh()
    drawable._read_materials(res, main, body)
    drawable._read_models(res, main, body)
    if not body.finish():
        return None
    out.body = body
    skel = res.ptr(main + 0x0C)
    if skel is None:
        return out
    bones = res.ptr(skel)
    count = res.u16(skel + 0x14)
    names = [res.cstr(res.ptr(bones + i * 0xE0)).lower() for i in range(count)]
    world = drawable._bone_world_matrices(res, skel)
    for i, name in enumerate(names):
        if i < len(world):
            out.bones[name] = (i, tuple(float(c) for c in world[i][:3, 3]))
        if name.startswith("seat_") and i < len(world):
            out.seats[name] = tuple(float(c) for c in world[i][:3, 3])
    seen = set()
    for off in range(0x90, len(res.sys) - 4, 4):
        d = res.ptr(off)
        if d is None or d in seen or d == main or d + 0x50 > len(res.sys):
            continue
        child = off - 0x90
        if res.u32(child) == 0 or res.ptr(d + 0x0C) != skel:
            continue
        if all(res.ptr(d + 0x40 + 4 * k) is None for k in range(4)):
            continue
        seen.add(d)
        bone = res.u16(child + 0x0E)
        name = names[bone] if bone < count else ""
        if not name.startswith("wheel_"):
            continue
        m = drawable.Mesh()
        drawable._read_materials(res, main, m)
        try:
            drawable._read_models(res, d, m)
        except Exception:
            continue
        if m.finish():
            out.wheels[name] = m
    if not out.wheels:
        return out
    front = "wheel_lf" if "wheel_lf" in out.wheels else next(iter(out.wheels))
    rear = "wheel_lr" if "wheel_lr" in out.wheels else front
    for i, name in enumerate(names):
        if name in WHEEL_BONES and i < len(world):
            pos = tuple(float(c) for c in world[i][:3, 3])
            mesh = front if name in ("wheel_lf", "wheel_rf") else rear
            out.wheel_slots.append((name, pos, mesh, name.startswith("wheel_r")))
    return out


def _subset(mesh, face_mask):
    """A new finished drawable.Mesh with only the masked triangles (materials kept as they are)."""
    m = drawable.Mesh()
    faces = mesh.faces[face_mask]
    used = np.unique(faces)
    remap = np.full(len(mesh.positions), -1, np.int64)
    remap[used] = np.arange(len(used))
    m.positions = mesh.positions[used]
    m.normals = mesh.normals[used]
    m.uvs = mesh.uvs[used]
    m.colors = mesh.colors[used] if len(mesh.colors) == len(mesh.positions) else mesh.colors
    m.faces = remap[faces]
    m.face_mats = mesh.face_mats[face_mask]
    m.materials = mesh.materials
    m.textures = mesh.textures
    m.joints, m.weights, m.skinned = [], [], False
    return m


DOOR_BONES = {"door_lf": ("door_dside_f", "window_lf")}


def split_parts(vm, parts=DOOR_BONES):
    """Cuts parts (e.g. the driver's door with its window) out of the body by the bone each vertex
    follows (its strongest weight). -> {part: (mesh in the part bone's space, bone position)};
    vm.body loses those triangles."""
    body = vm.body
    if body is None or not body.skinned or not len(body.joints):
        return {}
    dom = body.joints[np.arange(len(body.joints)), body.weights.argmax(1)]
    out = {}
    for part, bone_names in parts.items():
        ids = [vm.bones[b][0] for b in bone_names if b in vm.bones]
        if not ids or bone_names[0] not in vm.bones:
            continue
        on = np.isin(dom, ids)
        mask = on[body.faces].all(axis=1)
        if mask.sum() < 8:
            continue
        piece = _subset(body, mask)
        hinge = np.array(vm.bones[bone_names[0]][1], np.float32)
        piece.positions = (piece.positions - hinge).astype(np.float32)
        out[part] = (piece, tuple(float(c) for c in hinge))
        body = _subset(body, ~mask)
    if out:
        vm.body = body
    return out


class VehicleArchive:
    def __init__(self, game_dir, key):
        self.archives = []
        for rel in (os.path.join("pc", "models", "cdimages", "vehicles.img"),):
            p = _data_file(game_dir, rel)
            if p:
                self.archives.append(img.ImgArchive(p, key))
        self._txd_cache = {}

    def names(self):
        return sorted({n[:-4] for a in self.archives for n in a.names() if n.endswith(".wft")})

    def read(self, name):
        for a in self.archives:
            d = a.read(name)
            if d is not None:
                return d
        return None

    def model(self, name):
        d = self.read(name + ".wft")
        return load_vehicle_model(d) if d is not None else None

    def textures(self, txd):
        if txd not in self._txd_cache:
            d = self.read(txd + ".wtd")
            try:
                self._txd_cache[txd] = texture.load_wtd(d) if d is not None else {}
            except Exception:
                self._txd_cache[txd] = {}
        return self._txd_cache[txd]

    def close(self):
        for a in self.archives:
            a.close()


def paint_slot(shader):
    """gta_vehicle_paint1/2/3 -> 1/2/3 (which carcols colour tints this material), else 0."""
    m = re.search(r"vehicle_paint(\d)", shader.lower())
    return int(m.group(1)) if m else 0


# handling.dat columns (after the name), see the file's header
HANDLING_FIELDS = ("mass", "drag_mult", "submerged", "com_x", "com_y", "com_z",
                   "drive_bias", "gears", "drive_force", "drive_inertia", "max_speed", "brake_force",
                   "brake_bias", "handbrake_force", "steering_lock",
                   "traction_max", "traction_min", "traction_lateral", "traction_spring_delta", "traction_bias",
                   "susp_force", "susp_comp_damp", "susp_rebound_damp", "susp_upper", "susp_lower", "susp_raise",
                   "susp_bias", "dmg_collision", "dmg_weapon", "dmg_deformation", "dmg_engine",
                   "seat_offset", "value", "model_flags", "handling_flags", "anim_group")


def load_handling(game_dir):
    """{HANDLING_ID: {field: value}} from common/data/handling.dat (cars; boats, bikes and
    aircraft have their own sections and are skipped)."""
    path = _data_file(game_dir, os.path.join("common", "data", "handling.dat"))
    out = {}
    if path is None:
        return out
    for line in open(path, encoding="latin-1"):
        if not line or line[0] in ";#!$%^&*" or not line[0].isalnum():
            continue
        tok = line.split()
        if len(tok) < len(HANDLING_FIELDS) + 1:
            continue
        row = {}
        for k, v in zip(HANDLING_FIELDS, tok[1:]):
            if k in ("model_flags", "handling_flags"):
                try:
                    row[k] = int(v, 16)
                except ValueError:
                    row[k] = 0
            else:
                row[k] = _num(v, 0.0)
        out[tok[0].upper()] = row
    return out
