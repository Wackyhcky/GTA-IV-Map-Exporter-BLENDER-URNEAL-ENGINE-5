"""GTA IV characters (componentpeds.img) and weapons (weapons.img).

A ped is <name>.wft (skeleton: GTA IV's 80-bone "Char_*" rig), <name>.wdd (body-part
drawables named <slot>_<variant>_<letter>, e.g. head_000_r, uppr_001_u, lowr_000_m) and
<name>.wtd (textures). Niko is separate: playerped.rpf holds player.wft and one .wdr per
body part (head_000_r.wdr, ...) with its textures in .wtd files named after them. Body-part vertices are skinned: 4 weights + 4 indices into a per-geometry
bone table (see drawable._read_geometry)."""

import os
import string

import numpy as np

from . import drawable, img, rpf, texture
from .mapdata import joaat
from .rsc import Resource

SLOTS = ("head", "uppr", "lowr", "suse", "hand", "feet", "jack", "hair", "sus2", "teef", "face")
_NAMES = {}


def component_name(h):
    """joaat hash of a body-part drawable -> its name (e.g. 'uppr_000_u')."""
    if not _NAMES:
        for sl in SLOTS:
            for i in range(64):
                for v in string.ascii_lowercase:
                    n = "%s_%03d_%s" % (sl, i, v)
                    _NAMES[joaat(n)] = n
    return _NAMES.get(h)


class Skeleton:
    """names, parents (-1 = root), local (4x4) and world (4x4) rest matrices, GTA space, metres."""

    def __init__(self):
        self.names = []
        self.parents = []
        self.local = []
        self.world = []


def read_skeleton(res, skel):
    bones = res.ptr(skel)
    count = res.u16(skel + 0x14)
    sk = Skeleton()
    if bones is None or not count:
        return sk
    addr = {bones + i * 0xE0: i for i in range(count)}
    for i in range(count):
        b = bones + i * 0xE0
        sk.names.append(res.cstr(res.ptr(b)))
        sk.parents.append(addr.get(res.ptr(b + 0x10), -1))
        m = np.eye(4)
        m[:3, :3] = drawable._quat_matrix(*res.f32(b + 0x40, 4))
        m[:3, 3] = res.f32(b + 0x20, 3)
        sk.local.append(m)
    # Make names consistent across ped types so they all share one Unreal skeleton:
    # some peds call the toes Toe0, and the Extra_## helpers hang off different parents.
    for i, n in enumerate(sk.names):
        n = n.replace("_Toe0", "_Toe")
        if n.startswith("Extra_") and sk.parents[i] >= 0:
            n += "_" + sk.names[sk.parents[i]].replace("Char_", "").replace("_", "")
        sk.names[i] = n
    for i in range(count):
        p = sk.parents[i]
        sk.world.append(sk.world[p] @ sk.local[i] if 0 <= p < i else sk.local[i])
    return sk


GROUND = 1.0  # GTA peds have their origin at hip height, about a metre above the soles
TO_UNREAL = np.array([[-1, 0, 0, 0], [0, -1, 0, 0], [0, 0, 1, GROUND], [0, 0, 0, 1]], float)


def add_ground_root(sk):
    """Put the origin at the feet and face the other way, like Unreal/GASP characters: a new
    'Root' bone on the ground becomes the parent of 'Char', and the body is turned 180 degrees
    about the vertical. Root is appended so existing skin indices stay valid."""
    r = len(sk.names)
    sk.names.append("Root")
    sk.parents.append(-1)
    sk.local.append(np.eye(4))
    for i, p in enumerate(sk.parents[:r]):
        if p < 0:
            sk.parents[i] = r
            sk.local[i] = TO_UNREAL @ sk.local[i]
    sk.world = [TO_UNREAL @ w for w in sk.world] + [np.eye(4)]


def _merge(meshes):
    """Concatenate finished drawable.Mesh objects (materials appended per mesh)."""
    out = drawable.Mesh()
    base = 0
    mats = []
    for m in meshes:
        out.positions.append(m.positions)
        out.normals.append(m.normals)
        out.uvs.append(m.uvs)
        out.colors.append(m.colors)
        out.faces.append(m.faces + base)
        out.face_mats.append(m.face_mats + len(mats))
        if m.skinned:
            out.joints.append(m.joints)
            out.weights.append(m.weights)
        else:
            out.joints.append(np.zeros((len(m.positions), 4), np.int32))
            out.weights.append(np.tile(np.array([1, 0, 0, 0], np.float32), (len(m.positions), 1)))
        mats.extend(m.materials)
        out.textures.update(m.textures)
        base += len(m.positions)
    out.materials = mats
    out.skinned = True
    return out if out.finish() else None


def standard_facing(sk):
    """True when the skeleton faces the way GTA IV's peds normally do (its left thigh on +X in
    Unreal's orientation). One ped (f_m_business_01) has its skeleton turned round against its
    mesh; it would break the shared skeleton and the animation retargeting."""
    try:
        lt = sk.world[sk.names.index("Char_L_Thigh")][0, 3]
        rt = sk.world[sk.names.index("Char_R_Thigh")][0, 3]
    except ValueError:
        return False
    return lt > rt


# GTA IV turns these twist bones in code each frame; on their own they hang off the wrong bone
# (the upper-arm twists off the clavicle, the forearm twist off the upper arm), so in Unreal the
# skin weighted to them would stay behind when the arm moves. Re-attach them to the bone they follow.
TWIST_PARENTS = {
    "L_UpperArmRoll": "Char_L_UpperArm", "L_ArmRoll": "Char_L_UpperArm", "Char_L_ForeTwist": "Char_L_Forearm",
    "R_UpperArmRoll": "Char_R_UpperArm", "R_ArmRoll": "Char_R_UpperArm", "Char_R_ForeTwist": "Char_R_Forearm",
}


def fix_twist_bones(sk):
    for bone, parent in TWIST_PARENTS.items():
        if bone not in sk.names or parent not in sk.names:
            continue
        i, p = sk.names.index(bone), sk.names.index(parent)
        sk.parents[i] = p
        sk.local[i] = np.linalg.inv(sk.world[p]) @ sk.world[i]  # same rest pose, new parent


def _finish(p, meshes):
    """Merge the body parts and convert to Unreal character conventions."""
    p.mesh = _merge(meshes) if meshes else None
    if p.mesh is None:
        return None
    fix_twist_bones(p.skeleton)
    add_ground_root(p.skeleton)
    m = p.mesh
    m.positions = m.positions @ TO_UNREAL[:3, :3].T + TO_UNREAL[:3, 3]
    m.normals = m.normals @ TO_UNREAL[:3, :3].T
    return p


class Ped:
    def __init__(self, name):
        self.name = name
        self.skeleton = None
        self.mesh = None          # skinned drawable.Mesh of the default outfit
        self.parts = []           # component names used
        self.txd = name


class PedArchive:
    def __init__(self, game_dir, key, cutscenes=False):
        self.game_dir, self.key = game_dir, key
        self.archives = []
        rels = [os.path.join("pc", "models", "cdimages", "componentpeds.img"),
                os.path.join("pc", "models", "cdimages", "weapons.img"),
                os.path.join("pc", "models", "cdimages", "pedprops.img")]
        if cutscenes:   # the cutscene versions of the story characters (cs_roman_d, ...)
            rels.append(os.path.join("pc", "anim", "cutsprops.img"))
        for rel in rels:
            for base in (os.path.join(game_dir, "update"), game_dir):
                p = os.path.join(base, rel)
                if os.path.isfile(p):
                    self.archives.append(img.ImgArchive(p, key))
                    break
        self._tex = {}

    def read(self, name):
        for a in self.archives:
            d = a.read(name)
            if d is not None:
                return d
        return None

    def ped_names(self):
        return sorted({n[:-4] for a in self.archives for n in a.names() if n.endswith(".wdd")
                       and any(x.endswith(n[:-4] + ".wft") for x in a.names())})

    def weapon_names(self):
        out = set()
        for a in self.archives:
            names = a.names()
            if any(n.endswith(".wdd") for n in names):
                continue  # ped/prop archives
            out |= {n[:-4] for n in names if n.endswith((".wdr", ".wft"))}
        return sorted(out)

    def textures(self, txd):
        if txd not in self._tex:
            d = self.read(txd + ".wtd")
            try:
                self._tex[txd] = texture.load_wtd(d) if d is not None else {}
            except Exception:
                self._tex[txd] = {}
        return self._tex[txd]

    def ped(self, name, variant=0):
        """The ped's skeleton and its outfit: for each body-part slot, the given variant number
        (or the lowest one the ped has)."""
        wft, wdd = self.read(name + ".wft"), self.read(name + ".wdd")
        if wft is None or wdd is None:
            return None
        p = Ped(name)
        wres = Resource(wft)
        main = wres.ptr(0xB4)
        skel = wres.ptr(main + 0x0C) if main is not None else None
        if skel is None:
            return None
        p.skeleton = read_skeleton(wres, skel)
        res = Resource(wdd)
        hp, count = res.collection(0x10)
        ptrs = res.ptr_array(0x18)
        comps = {}
        for i, d in enumerate(ptrs):
            if d is None or hp is None:
                continue
            n = component_name(res.u32(hp + 4 * i))
            if n:
                comps[n] = d
        chosen = []
        for sl in SLOTS:
            have = sorted(n for n in comps if n.startswith(sl + "_"))
            if not have:
                continue
            want = [n for n in have if int(n.split("_")[1]) == variant] or have
            pick = want[0]
            if self._stub(res, comps[pick]):
                # cutscene characters keep empty 4-vertex placeholders as variant 0 (the cutscene
                # picks their real clothes): take the first real one
                pick = next((n for n in have if not self._stub(res, comps[n])), pick)
            chosen.append(pick)
        meshes = []
        for n in chosen:
            try:
                m = drawable.read_drawable(res, comps[n])
            except Exception:
                m = None
            if m is not None:
                meshes.append(m)
                p.parts.append(n)
        # (still in GTA's orientation here: the game's peds have their left thigh on -X)
        if meshes and standard_facing(p.skeleton):
            self._standardize(p, meshes)
        return _finish(p, meshes)

    _standard_names = None
    _standard_parents = {}

    def _standardize(self, p, meshes):
        """Cutscene characters (cs_*): modelled facing the other way, on a 90-bone rig with a
        full face. Turn them round and fold the bones the game's own peds don't have into their
        nearest kept parent (the face into the head), so they share the peds' skeleton. Their
        animations already match it (they're baked onto that skeleton)."""
        if PedArchive._standard_names is None:
            ref = self.ped("m_y_business_01")
            PedArchive._standard_names = set(ref.skeleton.names) if ref else set()
            PedArchive._standard_parents = ({n: (ref.skeleton.names[q] if q >= 0 else None)
                                             for n, q in zip(ref.skeleton.names, ref.skeleton.parents)} if ref else {})
        keep_names = PedArchive._standard_names
        std_parent = PedArchive._standard_parents
        if not keep_names:
            return
        sk = p.skeleton
        turn = np.diag([-1.0, -1.0, 1.0, 1.0])
        for m in meshes:
            m.positions = [x @ turn[:3, :3].T for x in m.positions] if isinstance(m.positions, list) else m.positions @ turn[:3, :3].T
            m.normals = [x @ turn[:3, :3].T for x in m.normals] if isinstance(m.normals, list) else m.normals @ turn[:3, :3].T
        world = [turn @ w for w in sk.world]
        n = len(sk.names)
        keep = [i for i in range(n) if sk.names[i] in keep_names]
        new_index = {old: k for k, old in enumerate(keep)}

        def kept(i):
            while i >= 0 and i not in new_index:
                i = sk.parents[i]
            return new_index.get(i, 0)
        remap = np.array([kept(i) for i in range(n)], np.int32)
        names, parents, local, wnew = [], [], [], []
        by_name = {sk.names[i]: i for i in keep}
        for old in keep:
            par = sk.parents[old]
            while par >= 0 and par not in new_index:
                par = sk.parents[par]
            # the same parent as the game's peds (the cutscene rig hangs the spine off the pelvis)
            want = std_parent.get(sk.names[old])
            if want in by_name and by_name[want] != old:
                par = by_name[want]
            names.append(sk.names[old])
            parents.append(new_index[par] if par >= 0 else -1)
            wnew.append(world[old])
            local.append(np.linalg.inv(world[par]) @ world[old] if par >= 0 else world[old])
        sk.names, sk.parents, sk.local, sk.world = names, parents, local, wnew
        for m in meshes:
            if isinstance(m.joints, list):
                m.joints = [remap[j] for j in m.joints]
            elif len(m.joints):
                m.joints = remap[m.joints]

    @staticmethod
    def _stub(res, d):
        try:
            m = drawable.read_drawable(res, d)
        except Exception:
            return False
        return m is not None and len(m.positions) < 20

    PLAYER_SLOTS = ("head", "uppr", "lowr", "hand", "feet", "hair", "teef")  # no braces/accessories

    def _player_archive(self):
        if not hasattr(self, "_rpf"):
            self._rpf = None
            for base in (os.path.join(self.game_dir, "update"), self.game_dir):
                path = os.path.join(base, "pc", "models", "cdimages", "playerped.rpf")
                if os.path.isfile(path):
                    self._rpf = rpf.RpfArchive(path, self.key)
                    break
        return self._rpf

    def player(self, variant=0):
        """Niko Bellic in his default outfit (from playerped.rpf), on the same skeleton as
        the other peds. Its textures: player_textures(ped)."""
        a = self._player_archive()
        wft = a.read("player.wft") if a else None
        if wft is None:
            return None
        p = Ped("player")
        wres = Resource(wft)
        main = wres.ptr(0xB4)
        skel = wres.ptr(main + 0x0C) if main is not None else None
        if skel is None:
            return None
        p.skeleton = read_skeleton(wres, skel)
        files = [n for n in a.names() if n.endswith(".wdr")]
        meshes = []
        for sl in self.PLAYER_SLOTS:
            have = sorted(n for n in files if n.startswith(sl + "_"))
            if not have:
                continue
            want = [n for n in have if int(n.split("_")[1]) == variant] or have
            try:
                m = drawable.load_wdr(a.read(want[0]))
            except Exception:
                m = None
            if m is not None:
                meshes.append(m)
                p.parts.append(want[0][:-4])
        return _finish(p, meshes)

    def player_textures(self, ped):
        """Textures used by Niko's mesh, each from the .wtd of the same name."""
        a = self._player_archive()
        out = {}
        names = {n for m in ped.mesh.materials for n in (m.diffuse, m.bump) if n}
        for n in names:
            d = a.read(n.lower() + ".wtd") if a else None
            if d is None:
                continue
            try:
                out.update(texture.load_wtd(d))
            except Exception:
                pass
        return out

    def weapon_bones(self, name):
        """The weapon model's helper bones (gun_grip, gun_muzzle, ...) as 4x4 matrices in the
        model's mesh space (relative to its root bone)."""
        d = self.read(name + ".wdr")
        if d is None:
            return {}
        res = Resource(d)
        skel = res.ptr(0x0C)
        if skel is None:
            return {}
        sk = read_skeleton(res, skel)
        if not sk.world:
            return {}
        inv_root = np.linalg.inv(sk.world[0])
        return {n: inv_root @ w for n, w in zip(sk.names, sk.world)}

    def weapon(self, name):
        d = self.read(name + ".wdr")
        if d is not None:
            return drawable.load_wdr(d)
        d = self.read(name + ".wft")
        return drawable.load_wft(d) if d is not None else None

    def close(self):
        for a in self.archives:
            a.close()
        if getattr(self, "_rpf", None):
            self._rpf.close()
