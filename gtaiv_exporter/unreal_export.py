"""Exports the GTA IV map for Unreal Engine (no Blender needed).

Writes into <out_dir>:
    meshes/batch_NNN.gltf + .bin   many models per file, one mesh node each
    textures/<name>.png    (Unreal can't import compressed DDS)
    gta4_unreal.json               materials, models and placements
`unreal_runner.py` then imports all of it inside the editor.
"""

import base64
import fnmatch
import json
import os
import re
import struct
import time

import numpy as np

from .mapdata import SURFACE_PRIORITY, GameData, emissive_kind, load_distant_lights, model_size, surface_kind, placed_lights, select_detail, split_interiors

ALPHA_SHADERS = ("alpha", "cutout", "decal", "glass", "window", "billboard", "trees", "grass", "wire")
BATCH_MODELS = 150


def ue_name(s):
    """Unreal asset names: letters, digits and underscores only."""
    return re.sub(r"[^A-Za-z0-9_]", "_", s)


def _quat(r):
    """WPL quaternion -> rotation quaternion (w, x, y, z) in GTA space."""
    x, y, z, w = r
    return (w, -x, -y, -z)


# GTA (right-handed, Z up) -> glTF (right-handed, Y up): (x, y, z) -> (x, z, -y)
_C4 = np.array([[1, 0, 0, 0], [0, 0, 1, 0], [0, -1, 0, 0], [0, 0, 0, 1]], np.float64)


def _quat_from_matrix(m):
    """3x3 rotation -> (x, y, z, w)."""
    t = m[0, 0] + m[1, 1] + m[2, 2]
    if t > 0:
        s = np.sqrt(t + 1.0) * 2
        q = ((m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s, 0.25 * s)
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = np.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        q = (0.25 * s, (m[0, 1] + m[1, 0]) / s, (m[0, 2] + m[2, 0]) / s, (m[2, 1] - m[1, 2]) / s)
    elif m[1, 1] > m[2, 2]:
        s = np.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        q = ((m[0, 1] + m[1, 0]) / s, 0.25 * s, (m[1, 2] + m[2, 1]) / s, (m[0, 2] - m[2, 0]) / s)
    else:
        s = np.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
        q = ((m[0, 2] + m[2, 0]) / s, (m[1, 2] + m[2, 1]) / s, 0.25 * s, (m[1, 0] - m[0, 1]) / s)
    q = np.array(q)
    return (q / np.linalg.norm(q)).tolist()


class _GltfBatch:
    def __init__(self):
        self.roots = []
        self.skins = []
        self.bin = bytearray()
        self.accessors = []
        self.views = []
        self.meshes = []
        self.nodes = []
        self.materials = []
        self._mat_index = {}

    def _add(self, arr, target, comp, typ, minmax=False):
        data = np.ascontiguousarray(arr).tobytes()
        while len(self.bin) % 4:
            self.bin.append(0)
        view = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)}
        if target is not None:
            view["target"] = target
        self.views.append(view)
        self.bin += data
        acc = {"bufferView": len(self.views) - 1, "componentType": comp, "count": len(arr), "type": typ}
        if minmax:
            acc["min"] = arr.min(axis=0).tolist()
            acc["max"] = arr.max(axis=0).tolist()
        self.accessors.append(acc)
        return len(self.accessors) - 1

    def material(self, name):
        if name not in self._mat_index:
            self._mat_index[name] = len(self.materials)
            self.materials.append({"name": name, "pbrMetallicRoughness": {"metallicFactor": 0.0}})
        return self._mat_index[name]

    def add_model(self, name, mesh, slot_names):
        # GTA (right-handed, Z up) -> glTF (right-handed, Y up): (x, y, z) -> (x, z, -y)
        p = mesh.positions
        pos = np.stack([p[:, 0], p[:, 2], -p[:, 1]], axis=1).astype(np.float32)
        n = mesh.normals
        nrm = np.stack([n[:, 0], n[:, 2], -n[:, 1]], axis=1).astype(np.float32)
        ln = np.linalg.norm(nrm, axis=1)
        bad = ln < 1e-6
        nrm[bad] = (0.0, 1.0, 0.0)
        ln[bad] = 1.0
        nrm /= ln[:, None]
        uv = mesh.uvs.astype(np.float32)  # GTA UVs are already top-left origin like glTF
        a_pos = self._add(pos, 34962, 5126, "VEC3", minmax=True)
        a_nrm = self._add(nrm, 34962, 5126, "VEC3")
        a_uv = self._add(uv, 34962, 5126, "VEC2")
        prims = []
        for mi in np.unique(mesh.face_mats):
            idx = mesh.faces[mesh.face_mats == mi].astype(np.uint32).ravel()
            a_idx = self._add(idx, 34963, 5125, "SCALAR")
            prims.append({"attributes": {"POSITION": a_pos, "NORMAL": a_nrm, "TEXCOORD_0": a_uv},
                          "indices": a_idx, "material": self.material(slot_names[mi]), "mode": 4})
        self.meshes.append({"name": name, "primitives": prims})
        self.nodes.append({"name": name, "mesh": len(self.meshes) - 1})
        self.roots.append(len(self.nodes) - 1)

    def add_skinned_model(self, name, mesh, slot_names, skeleton):
        """A skinned mesh plus its joint hierarchy (one skin per call)."""
        p = mesh.positions
        pos = np.stack([p[:, 0], p[:, 2], -p[:, 1]], axis=1).astype(np.float32)
        n = mesh.normals
        nrm = np.stack([n[:, 0], n[:, 2], -n[:, 1]], axis=1).astype(np.float32)
        ln = np.linalg.norm(nrm, axis=1)
        nrm[ln < 1e-6] = (0.0, 1.0, 0.0)
        nrm /= np.maximum(np.linalg.norm(nrm, axis=1), 1e-6)[:, None]
        uv = mesh.uvs.astype(np.float32)
        joints = np.clip(mesh.joints, 0, len(skeleton.names) - 1).astype(np.uint16)
        weights = mesh.weights.astype(np.float32)
        a_pos = self._add(pos, 34962, 5126, "VEC3", minmax=True)
        a_nrm = self._add(nrm, 34962, 5126, "VEC3")
        a_uv = self._add(uv, 34962, 5126, "VEC2")
        a_j = self._add(joints, 34962, 5123, "VEC4")
        a_w = self._add(weights, 34962, 5126, "VEC4")
        prims = []
        for mi in np.unique(mesh.face_mats):
            idx = mesh.faces[mesh.face_mats == mi].astype(np.uint32).ravel()
            prims.append({"attributes": {"POSITION": a_pos, "NORMAL": a_nrm, "TEXCOORD_0": a_uv,
                                         "JOINTS_0": a_j, "WEIGHTS_0": a_w},
                          "indices": self._add(idx, 34963, 5125, "SCALAR"),
                          "material": self.material(slot_names[mi]), "mode": 4})
        # joints: rest pose converted to glTF space
        first = len(self.nodes)
        ibm = []
        for i, bname in enumerate(skeleton.names):
            lg = _C4 @ skeleton.local[i] @ _C4.T
            wg = _C4 @ skeleton.world[i] @ _C4.T
            self.nodes.append({"name": bname, "translation": lg[:3, 3].tolist(),
                               "rotation": _quat_from_matrix(lg[:3, :3])})
            ibm.append(np.linalg.inv(wg).T.ravel())  # glTF matrices are column-major
        for i, parent in enumerate(skeleton.parents):
            if parent >= 0:
                self.nodes[first + parent].setdefault("children", []).append(first + i)
            else:
                self.roots.append(first + i)
        a_ibm = self._add(np.array(ibm, np.float32), None, 5126, "MAT4")
        self.skins.append({"name": name + "_skin", "joints": list(range(first, first + len(skeleton.names))),
                           "inverseBindMatrices": a_ibm,
                           "skeleton": first + next(i for i, p in enumerate(skeleton.parents) if p < 0)})
        self.meshes.append({"name": name, "primitives": prims})
        self.nodes.append({"name": name, "mesh": len(self.meshes) - 1, "skin": len(self.skins) - 1})
        self.roots.append(len(self.nodes) - 1)

    def write(self, path):
        bin_name = os.path.splitext(os.path.basename(path))[0] + ".bin"
        with open(os.path.join(os.path.dirname(path), bin_name), "wb") as f:
            f.write(self.bin)
        doc = {
            "asset": {"version": "2.0", "generator": "GTA IV Exporter"},
            "scene": 0,
            "scenes": [{"nodes": self.roots}],
            "nodes": self.nodes,
            **({"skins": self.skins} if self.skins else {}),
            "meshes": self.meshes,
            "materials": self.materials,
            "accessors": self.accessors,
            "bufferViews": self.views,
            "buffers": [{"uri": bin_name, "byteLength": len(self.bin)}],
        }
        with open(path, "w", encoding="utf-8") as f:
            json.dump(doc, f, separators=(",", ":"))


REFERENCE_PED = "m_y_business_01"
VEHICLE_ANIM_GROUPS = ("std", "low", "truck", "van", "bus")
PLAYER_WEAPON = "w_glock"  # the pistol the player gets (GTA IV Player component)
BLOOD_TEXTURES = ("blood_01", "blood_01_normal", "blood_pool_a", "blood_pool_b")  # pc/textures/fxprojtex.wtd


def export_blood(game_dir, out_dir, rel_to, log=print):
    """GTA IV's blood decals as PNGs; returns {name: path relative to rel_to}."""
    from . import texture
    out = {}
    for root in (os.path.join(game_dir, "update"), game_dir):
        path = os.path.join(root, "pc", "textures", "fxprojtex.wtd")
        if os.path.isfile(path):
            break
    else:
        return out
    try:
        with open(path, "rb") as f:
            texs = texture.load_wtd(f.read())
        os.makedirs(out_dir, exist_ok=True)
        for n in BLOOD_TEXTURES:
            if n in texs:
                p = os.path.join(out_dir, n + ".png")
                with open(p, "wb") as f:
                    f.write(texs[n].to_png())
                out[n] = os.path.relpath(p, rel_to)
    except Exception as e:
        log("Couldn't read the blood textures: %s" % e)
    return out


class UnrealExporter:
    def __init__(self, game_dir, out_dir, areas="*", lods=False, textures=True, key_cache=None, log=print,
                 detail=None, interiors=True, lights=True, traffic=False, characters=False, ped_filter=None,
                 characters_only=False, weapon_filter=None, distances=False, story=False):
        self.game_dir = game_dir
        # GTA IV's first mission with its cutscenes (and the districts it's played in)
        self.story = story
        # GTA IV's draw distances: objects fade at their IDE distance, LOD/SLOD stand-ins take over
        self.distances = distances
        self.characters_only = characters_only  # just the peds, Niko and weapons: no map
        self.out_dir = out_dir
        self.area_patterns = [a.strip().lower() for a in areas.replace(";", ",").split(",") if a.strip()] or ["*"]
        self.detail = detail or ("LOW" if lods else "FULL")
        self.textures = textures
        self.interiors = interiors
        self.lights = lights
        self.traffic = traffic
        self.characters = characters
        self.ped_filter = ped_filter  # None = every ped, else a list of names ("player" = Niko)
        self.weapon_filter = weapon_filter  # None = every weapon/item, else a list of names
        # vehicles (Vehicles tab) arrive in the same list as "veh:<name>"
        self.vehicle_filter = [w[4:] for w in (weapon_filter or []) if w.startswith("veh:")]
        if weapon_filter is not None:
            self.weapon_filter = [w for w in weapon_filter if not w.startswith("veh:")]
        self.key_cache = key_cache or os.path.join(out_dir, "gta4_key.bin")
        self.log = log
        self._txd_cache = {}
        self._wdd_cache = {}
        self.materials = {}   # material asset name -> {diffuse, bump, masked}
        self.tex_names = {}   # GTA texture name -> written file stem (or None)

    def _find_texture(self, name, mesh, txd):
        tex = mesh.textures.get(name)
        if tex is not None:
            return tex
        for t in self.gd.txd_chain(txd):
            d = self._txd_cache.get(t)
            if d is None:
                d = self._txd_cache[t] = self.gd.load_txd(t)
            tex = d.get(name)
            if tex is not None:
                return tex
        return None

    def _texture(self, name, mesh, txd):
        if not name or not self.textures:
            return None
        if name in self.tex_names:
            return self.tex_names[name]
        stem = None
        tex = self._find_texture(name, mesh, txd)
        if tex is not None and tex.data:
            stem = "T_" + ue_name(name)
            path = os.path.join(self.out_dir, "textures", stem + ".png")
            if not os.path.isfile(path):
                try:
                    png = tex.to_png()
                except Exception as e:
                    self.log("Couldn't decode texture %s: %s" % (name, e))
                    self.tex_names[name] = None
                    return None
                with open(path, "wb") as f:
                    f.write(png)
        self.tex_names[name] = stem
        return stem

    def _material_name(self, m, mesh, txd):
        masked = any(s in m.shader.lower() for s in ALPHA_SHADERS)
        diffuse = self._texture(m.diffuse, mesh, txd)
        bump = self._texture(m.bump, mesh, txd)
        name = "MI_" + (ue_name(m.diffuse) if m.diffuse else "untextured")
        if masked:
            name += "_M"
        if bump:
            name += "_N"
        if name not in self.materials:
            self.materials[name] = {"diffuse": diffuse, "bump": bump, "masked": masked, "kind": "opaque"}
        kind = surface_kind(m.shader)
        if SURFACE_PRIORITY[kind] > SURFACE_PRIORITY[self.materials[name]["kind"]]:
            self.materials[name]["kind"] = kind
        glow = emissive_kind(m.shader)
        if glow is not None and diffuse:
            old = self.materials[name].get("emissive")
            # a texture used both ways: "always glows" wins over "night only"
            if old is None or (old[0] and not glow[0]) or (old[0] == glow[0] and glow[1] > old[1]):
                self.materials[name]["emissive"] = [glow[0], glow[1]]
        return name

    SUBWAY_AREA = "manhatsw"  # GTA IV keeps the whole subway (tunnels, stations) in one area

    def _add_subway(self, gd, areas, selected, margin=60.0):
        """The subway under the exported districts: its tunnels and stations belong to their own
        area, so a district on its own would come without the tracks underneath it."""
        if self.SUBWAY_AREA in selected or self.SUBWAY_AREA not in areas:
            return
        xy = [i.pos for rows in selected.values() for i, _ in rows]
        x0, x1 = min(p[0] for p in xy) - margin, max(p[0] for p in xy) + margin
        y0, y1 = min(p[1] for p in xy) - margin, max(p[1] for p in xy) + margin
        under = [i for i in areas[self.SUBWAY_AREA] if x0 <= i.pos[0] <= x1 and y0 <= i.pos[1] <= y1]
        if not under:
            return
        keep = select_detail(gd, under, self.SUBWAY_AREA, self.detail, with_lods=self.distances)
        if keep:
            selected[self.SUBWAY_AREA] = keep
            self.log("Subway under the exported districts: %d placements" % len(keep))

    def _draw_distances(self, kept_by_area):
        """{id(instance): (start, end)} in metres: shown from `start` (0 = up close) to `end`
        (0 = always). Full-detail objects end at their IDE draw distance; an LOD starts where
        the furthest of its children ends and ends at its own distance, and so do SLODs."""
        everything = [i for rows in kept_by_area.values() for i in rows]
        kept = {id(i) for i in everything}
        end = {}
        for i in everything:
            d = float(self.gd.models[i.hash].draw_dist or 0.0)
            end[id(i)] = max(d, 40.0) if d > 0 else 0.0  # tiny props popping right in front of you look broken
        start = {}
        for i in everything:
            p = i.parent
            if p is not None and id(p) in kept and end[id(i)] > 0:
                start[id(p)] = max(start.get(id(p), 0.0), end[id(i)])
        out = {}
        for i in everything:
            s0, e0 = start.get(id(i), 0.0), end[id(i)]
            if e0 and s0 >= e0:
                e0 = 0.0  # a stand-in whose children reach further than it does: keep it to the horizon
            out[id(i)] = (round(s0, 1), round(e0, 1))
        n_lod = sum(1 for i in everything if i.is_lod)
        self.log("Draw distances: %d objects fade out, %d LOD/SLOD stand-ins take over far away"
                 % (sum(1 for v in out.values() if v[1]), n_lod))
        return out

    def _distant_traffic(self, placements, spacing=40.0, margin=150.0):
        """GTA IV's distant traffic lights for roads inside the exported districts:
        one row per car (segment start, segment vector, red?, phase along the segment)."""
        xy = [(r[1], r[2]) for rows in placements.values() for r in rows]
        if not xy:
            return []
        x0, x1 = min(p[0] for p in xy) - margin, max(p[0] for p in xy) + margin
        y0, y1 = min(p[1] for p in xy) - margin, max(p[1] for p in xy) + margin
        rows = []
        for pts, red in load_distant_lights(self.game_dir):
            for a, b in zip(pts, pts[1:]):
                if not (x0 <= a[0] <= x1 and y0 <= a[1] <= y1):
                    continue
                d = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
                length = (d[0] ** 2 + d[1] ** 2 + d[2] ** 2) ** 0.5
                if length < 1.0:
                    continue
                n = max(1, int(round(length / spacing)))
                for j in range(n):
                    rows.append([a[0], a[1], a[2] + 1.0, d[0], d[1], d[2], int(red), round(j / n, 4)])
        self.log("Distant traffic: %d moving lights" % len(rows))
        return rows

    # ------------------------------------------------------------ traffic
    def _veh_texture(self, name, mesh, chain, va):
        if not name or not self.textures:
            return None
        key = "veh:" + name
        if key in self.tex_names:
            return self.tex_names[key]
        tex = mesh.textures.get(name)
        for t in chain:
            if tex is not None:
                break
            tex = va.textures(t).get(name)
        stem = None
        if tex is not None and tex.data:
            stem = "T_VEH_" + ue_name(name)
            path = os.path.join(self.out_dir, "textures", stem + ".png")
            if not os.path.isfile(path):
                try:
                    with open(path, "wb") as f:
                        f.write(tex.to_png())
                except Exception as e:
                    self.log("Couldn't decode texture %s: %s" % (name, e))
                    stem = None
        self.tex_names[key] = stem
        return stem

    def _veh_material(self, m, mesh, chain, va):
        from .vehicles import paint_slot
        sh = m.shader.lower()
        diffuse = self._veh_texture(m.diffuse, mesh, chain, va)
        bump = self._veh_texture(m.bump, mesh, chain, va)
        slot = paint_slot(sh)
        glass = "glass" in sh
        name = "MI_VEH_" + (ue_name(m.diffuse) if m.diffuse else "untextured")
        name += ("_P%d" % slot if slot else "") + ("_G" if glass else "") + ("_N" if bump else "")
        if "lightsemissive" in sh:
            name += "_L"
        if name not in self.materials:
            mat = {"diffuse": diffuse, "bump": bump, "masked": False, "kind": "glass" if glass else "opaque"}
            if slot:
                mat["paint"] = slot
            if "lightsemissive" in sh and diffuse:
                mat["emissive"] = [True, 3.0]  # head/tail light glass glows at night
            self.materials[name] = mat
        return name

    def _road_network(self, placements, margin=200.0):
        """The drivable road network around the exported districts (Unreal units), used by
        the GTAIVTraffic plugin for cars and, along its curbs, pedestrians."""
        from .roads import load_road_network
        xy = [(r[1], r[2]) for rows in placements.values() for r in rows]
        if not xy:
            return None
        bbox = (min(p[0] for p in xy) - margin, min(p[1] for p in xy) - margin,
                max(p[0] for p in xy) + margin, max(p[1] for p in xy) + margin)
        net = load_road_network(self.game_dir, self.gd.key, bbox)
        nodes, first, count, target, lanes = [], [], [], [], []
        for (x, y, z), out in zip(net.nodes, net.links):
            nodes.append([round(x * 100, 1), round(-y * 100, 1), round(z * 100, 1)])
            first.append(len(target))
            count.append(len(out))
            for t, la in out:
                target.append(t)
                lanes.append(la)
        self.log("Roads: %d nodes, %d drivable links" % (len(nodes), len(target)))
        return {"nodes": nodes, "first": first, "count": count, "target": target, "lanes": lanes, "vehicles": []}

    def _export_traffic(self, placements, batches, batch_meshes):
        """GTA IV's traffic cars (bodies + wheels, paint slots) plus the road network, for the
        GTAIVTraffic Unreal plugin."""
        from .vehicles import VehicleArchive, load_vehicle_defs
        net = self._road_network(placements)
        if net is None:
            return None
        defs, txd_parent = load_vehicle_defs(self.game_dir)
        va = VehicleArchive(self.game_dir, self.gd.key)
        cars = []
        state = {"batch": _GltfBatch(), "no": len(batches)}

        def flush():
            b = state["batch"]
            if b.nodes:
                fn = "veh_%03d.gltf" % state["no"]
                b.write(os.path.join(self.out_dir, "meshes", fn))
                batches.append(fn)
                batch_meshes[fn] = [n["name"] for n in b.nodes]
                state["no"] += 1
                state["batch"] = _GltfBatch()

        try:
            cars = self._export_vehicle_models(sorted(n for n, d in defs.items() if d.in_traffic), defs, txd_parent,
                                               va, state, flush)
            flush()
        finally:
            va.close()
        self.log("Traffic: %d vehicles" % len(cars))
        net["vehicles"] = cars
        return net

    def _vehicle_names(self):
        return getattr(self, "_veh_names", None)

    def _export_vehicle_models(self, names, defs, txd_parent, va, state, flush):
        """Body + wheel meshes, paint combos and handling.dat for the named vehicles."""
        from .vehicles import load_handling
        handling = load_handling(self.game_dir)
        text = None
        try:
            from .gxt import Text
            text = Text(open(os.path.join(self.game_dir, "common", "text", "american.gxt"), "rb").read())
        except Exception:
            pass
        cars = []
        sounds = self._export_vehicle_sounds(names, defs)
        if True:
            for name in names:
                d = defs[name]
                try:
                    vm = va.model(name)
                except Exception as e:
                    self.log("Failed to read vehicle %s: %s" % (name, e))
                    continue
                if vm is None or vm.body is None:
                    continue
                chain, t = [], d.txd
                while t and t not in chain:
                    chain.append(t)
                    t = txd_parent.get(t)
                chain.append("vehshare")
                body_name = "SM_VEH_" + ue_name(name)
                from .vehicles import split_parts
                door = None
                for part, (piece, (hx, hy, hz)) in split_parts(vm).items():
                    pn = "%s_%s" % (body_name, part)
                    state["batch"].add_model(pn, piece, [self._veh_material(m, piece, chain, va) for m in piece.materials])
                    door = {"mesh": pn, "hinge": [hx * 100, -hy * 100, hz * 100]}
                state["batch"].add_model(body_name, vm.body,
                                         [self._veh_material(m, vm.body, chain, va) for m in vm.body.materials])
                wheel_names = {}
                for wname, wm in vm.wheels.items():
                    wn = "%s_%s" % (body_name, ue_name(wname))
                    state["batch"].add_model(wn, wm, [self._veh_material(m, wm, chain, va) for m in wm.materials])
                    wheel_names[wname] = (wn, float((wm.positions[:, 2].max() - wm.positions[:, 2].min()) * 50.0))
                wheels, lowest = [], 0.0
                for bone, (x, y, z), mesh, right in vm.wheel_slots:
                    wn, radius = wheel_names[mesh]
                    wheels.append({"mesh": wn, "offset": [x * 100, -y * 100, z * 100], "right": right,
                                   "steers": bone in ("wheel_lf", "wheel_rf"), "radius": radius})
                    lowest = min(lowest, z * 100 - radius)
                ext = vm.body.positions.max(0) - vm.body.positions.min(0)
                paints = [[[round((c / 255.0) ** 2.2, 4) for c in rgb] for rgb in combo] for combo in d.colors]
                display = ""
                if text is not None and d.game_name:
                    display = text.get(d.game_name) or ""
                def seat(bone):
                    p = vm.seats.get(bone)
                    return [p[0] * 100, -p[1] * 100, p[2] * 100] if p else None
                group = d.anim_group[4:] if d.anim_group.startswith("veh@") else "std"
                cars.append({"name": name, "body": body_name, "wheels": wheels, "paints": paints,
                             "seat": seat("seat_dside_f"), "seat_p": seat("seat_pside_f"), "door": door,
                             "sounds": sounds.get(name, {}),
                             "anim": group if group in VEHICLE_ANIM_GROUPS else "std",
                             "frequency": d.frequency, "length": float(ext[1] * 100), "width": float(ext[0] * 100),
                             "ride_height": float(-lowest if wheels else 60.0),
                             "display": display or d.game_name.title(),
                             "handling": handling.get(d.handling) or handling.get(name.upper()) or {}})
                if len(state["batch"].nodes) >= 60:
                    flush()
        self._export_vehicle_anims()
        return cars

    def _export_vehicle_sounds(self, names, defs):
        """GTA IV's engine, exhaust, horn and door sounds of these cars, and the tyre skids
        (WAVs in vehicle_sounds/, vehicle_sounds.json for the shared ones)."""
        try:
            from .vehicle_audio import export_vehicle_sounds
            out = os.path.join(self.out_dir, "vehicle_sounds")
            cars, shared = export_vehicle_sounds(self.game_dir, self.gd.key, list(names), out, log=self.log,
                                                 aliases={n: (defs[n].game_name, defs[n].handling) for n in names})
            prev = {}
            path = os.path.join(self.out_dir, "vehicle_sounds.json")
            if os.path.isfile(path):
                prev = json.load(open(path, encoding="utf-8"))
            prev.update(shared)
            with open(path, "w", encoding="utf-8") as f:
                json.dump(prev, f)
            return cars
        except Exception as e:
            self.log("No vehicle sounds: %s" % e)
            return {}

    def _export_vehicle_anims(self):
        """veh_anims.json: GTA IV's sitting / steering animations for the drivers."""
        if getattr(self, "_veh_anims_done", False):
            return
        self._veh_anims_done = True
        try:
            from . import anims
            anims.export_clips(self.game_dir, self.gd.key, self.out_dir, clips=anims.vehicle_clips(),
                               log=self.log, file_name="veh_anims.json")
        except Exception as e:
            self.log("No driver animations: %s" % e)

    def _export_vehicles_only(self, batches, batch_meshes):
        """The chosen vehicles (Vehicles tab), drivable in Unreal."""
        from .vehicles import VehicleArchive, load_vehicle_defs
        defs, txd_parent = load_vehicle_defs(self.game_dir)
        names = [n for n in self.vehicle_filter if n in defs]
        if not names:
            return []
        va = VehicleArchive(self.game_dir, self.gd.key)
        state = {"batch": _GltfBatch(), "no": len(batches)}

        def flush():
            b = state["batch"]
            if b.nodes:
                fn = "veh_%03d.gltf" % state["no"]
                b.write(os.path.join(self.out_dir, "meshes", fn))
                batches.append(fn)
                batch_meshes[fn] = [n["name"] for n in b.nodes]
                state["no"] += 1
                state["batch"] = _GltfBatch()
        try:
            cars = self._export_vehicle_models(names, defs, txd_parent, va, state, flush)
            flush()
        finally:
            va.close()
        self.log("Vehicles: %d" % len(cars))
        return cars

    # ------------------------------------------------------------ characters & weapons
    def _tex_from(self, prefix, name, mesh, dicts):
        """Write texture `name` (embedded in mesh or in one of dicts) as <prefix><name>.png."""
        if not name or not self.textures:
            return None
        key = prefix + name
        if key in self.tex_names:
            return self.tex_names[key]
        tex = mesh.textures.get(name)
        for d in dicts:
            if tex is not None:
                break
            tex = d.get(name)
        stem = None
        if tex is not None and tex.data:
            stem = "T_" + ue_name(prefix + name)
            path = os.path.join(self.out_dir, "textures", stem + ".png")
            if not os.path.isfile(path):
                try:
                    with open(path, "wb") as f:
                        f.write(tex.to_png())
                except Exception as e:
                    self.log("Couldn't decode texture %s: %s" % (name, e))
                    stem = None
        self.tex_names[key] = stem
        return stem

    def _char_material(self, prefix, m, mesh, dicts):
        diffuse = self._tex_from(prefix, m.diffuse, mesh, dicts)
        bump = self._tex_from(prefix, m.bump, mesh, dicts)
        kind = surface_kind(m.shader)
        if kind == "decal":
            kind = "masked"  # decals on a character (tattoos, dirt) can't be mesh decals
        name = "MI_" + ue_name(prefix + (m.diffuse or "untextured")) + ("_M" if kind == "masked" else "") + \
            ("_G" if kind == "glass" else "") + ("_N" if bump else "")
        if name not in self.materials:
            self.materials[name] = {"diffuse": diffuse, "bump": bump, "masked": kind == "masked", "kind": kind,
                                    "skinned": True}
        return name

    def _export_characters(self, batches, batch_meshes, progress=None):
        """Every GTA IV ped (default outfit) as a skinned glTF on the shared 80-bone skeleton,
        plus the weapons/held items as static meshes."""
        from .peds import PedArchive, standard_facing
        pa = PedArchive(self.game_dir, self.gd.key)
        peds = []
        try:
            names = pa.ped_names()
            if self.ped_filter is not None:
                names = [n for n in names if n in set(self.ped_filter)]
            # the first ped imported defines the shared skeleton and the retargeting rig:
            # start with a typical one
            names.sort(key=lambda n: n != REFERENCE_PED)
            for k, name in enumerate(names):
                try:
                    p = pa.ped(name)
                except Exception as e:
                    self.log("Failed to read ped %s: %s" % (name, e))
                    continue
                if p is None:
                    continue
                if not standard_facing(p.skeleton):
                    self.log("Skipped ped %s: its skeleton faces the other way from its body" % name)
                    continue
                prefix = "PED_%s_" % name
                dicts = [pa.textures(name)]
                slots = [self._char_material(prefix, m, p.mesh, dicts) for m in p.mesh.materials]
                b = _GltfBatch()
                mesh_name = "SK_PED_" + ue_name(name)
                b.add_skinned_model(mesh_name, p.mesh, slots, p.skeleton)
                fn = "ped_%s.gltf" % ue_name(name)
                b.write(os.path.join(self.out_dir, "meshes", fn))
                peds.append({"name": name, "mesh": mesh_name, "gltf": fn, "parts": p.parts, "slots": slots,
                             "height": float(p.mesh.positions[:, 2].max() - p.mesh.positions[:, 2].min())})
                if k % 25 == 0:
                    self.log("  characters %d / %d" % (k, len(names)))
            player = None
            if self.ped_filter is None or "player" in self.ped_filter:
                try:
                    p = pa.player()
                except Exception as e:
                    self.log("Failed to read Niko (playerped.rpf): %s" % e)
                    p = None
                if p is not None and standard_facing(p.skeleton):
                    dicts = [pa.player_textures(p)]
                    slots = [self._char_material("PED_niko_", m, p.mesh, dicts) for m in p.mesh.materials]
                    b = _GltfBatch()
                    b.add_skinned_model("SK_PED_Niko", p.mesh, slots, p.skeleton)
                    b.write(os.path.join(self.out_dir, "meshes", "ped_niko.gltf"))
                    player = {"name": "niko", "mesh": "SK_PED_Niko", "gltf": "ped_niko.gltf", "parts": p.parts, "slots": slots,
                              "player": True}
                    peds.append(player)
            weapons = []
            b = _GltfBatch()
            for name in pa.weapon_names():
                # Niko's pistol comes with him (the player kit), whatever else is chosen
                if self.weapon_filter is not None and name not in self.weapon_filter and                         not (player is not None and name == PLAYER_WEAPON):
                    continue
                try:
                    w = pa.weapon(name)
                except Exception:
                    w = None
                if w is None:
                    continue
                prefix = "WPN_%s_" % name
                slots = [self._char_material(prefix, m, w, [pa.textures(name)]) for m in w.materials]
                for s_ in slots:
                    self.materials[s_].pop("skinned", None)
                b.add_model("SM_WPN_" + ue_name(name), w, slots)
                weapons.append("SM_WPN_" + ue_name(name))
            if b.nodes:
                fn = "wpn_%03d.gltf" % len(batches)
                b.write(os.path.join(self.out_dir, "meshes", fn))
                batches.append(fn)
                batch_meshes[fn] = [n["name"] for n in b.nodes if "mesh" in n]
        finally:
            pa.close()
        self.log("Characters: %d peds%s, %d weapons/items" % (len(peds) - (player is not None),
                                                             " + Niko" if player else "", len(weapons)))
        out = {"peds": peds, "weapons": weapons}
        if player is not None and "SM_WPN_" + ue_name(PLAYER_WEAPON) in weapons:
            out["player_kit"] = self._player_kit([p["name"] for p in peds if not p.get("player")])
        return out

    def _player_kit(self, ped_models=()):
        """Niko's pistol: gun@handgun animations and the pistol's sounds (resident.rpf/weapons)."""
        from . import anims, audio
        kit = {"weapon": "SM_WPN_" + ue_name(PLAYER_WEAPON), "icon": "T_WPN_%s_icon" % ue_name(PLAYER_WEAPON)}
        try:
            from .peds import PedArchive
            from .anims import _mat_quat
            pa = PedArchive(self.game_dir, self.gd.key)
            bones = pa.weapon_bones(PLAYER_WEAPON)
            pa.close()
            if "gun_grip" in bones:  # GTA IV puts this bone in the ped's hand
                g = bones["gun_grip"]
                kit["grip"] = [*g[:3, 3].tolist(), *_mat_quat(g[:3, :3])]
            if "gun_muzzle" in bones:
                kit["muzzle"] = bones["gun_muzzle"][:3, 3].tolist()
        except Exception as e:
            self.log("Couldn't read the pistol's grip: %s" % e)
        try:
            path = anims.export_clips(self.game_dir, self.gd.key, os.path.join(self.out_dir, "player"), log=self.log)
            kit["anims"] = os.path.relpath(path, self.out_dir)
        except Exception as e:
            self.log("Couldn't read the pistol animations: %s" % e)
            return None
        try:
            from . import audio as _audio
            kit["voices"] = _audio.export_voices(self.game_dir, self.gd.key, ped_models,
                                                 os.path.join(self.out_dir, "player", "voices"), self.out_dir, self.log)
        except Exception as e:
            self.log("Couldn't read the ped voices: %s" % e)
        kit["blood"] = export_blood(self.game_dir, os.path.join(self.out_dir, "player", "blood"), self.out_dir, self.log)
        try:
            sounds = audio.export_bank_sounds(self.game_dir, self.gd.key, audio.PISTOL_SOUNDS,
                                              os.path.join(self.out_dir, "player", "sounds"), log=self.log)
            kit["sounds"] = {k: os.path.relpath(v, self.out_dir) for k, v in sounds.items()}
        except Exception as e:
            self.log("Couldn't read the pistol sounds: %s" % e)
        return kit

    def _run_characters_only(self, progress, t0):
        batches, batch_meshes = [], {}
        characters = self._export_characters(batches, batch_meshes, progress)
        vehicles = self._export_vehicles_only(batches, batch_meshes) if self.vehicle_filter else []
        data = {
            "version": 1,
            "characters_only": True,
            "batches": batches,
            "batch_meshes": batch_meshes,
            "textures": sorted({v for v in self.tex_names.values() if v}),
            "materials": self.materials,
            "placements": {}, "lights": {}, "distant": [], "traffic": None,
            "characters": characters,
            "vehicles": vehicles,
        }
        with open(os.path.join(self.out_dir, "gta4_unreal.json"), "w", encoding="utf-8") as f:
            json.dump(data, f)
        self.log("Wrote characters only: %d textures, %d materials in %.0fs"
                 % (len(data["textures"]), len(self.materials), time.time() - t0))
        return ((len(characters["peds"]) + len(characters["weapons"])) if characters else 0) + len(vehicles)

    def _wanted_area(self, area):
        return any(fnmatch.fnmatch(area, p) for p in self.area_patterns)

    def run(self, progress=None):
        t0 = time.time()
        for sub in ("meshes", "textures"):
            os.makedirs(os.path.join(self.out_dir, sub), exist_ok=True)
        self.gd = gd = GameData(self.game_dir, log=self.log, key_cache=self.key_cache)
        if self.characters_only:
            return self._run_characters_only(progress, t0)
        gd.load_definitions()
        gd.open_archives()
        areas = gd.load_instances(interiors=self.interiors)

        if self.story:
            from .story import mission_areas
            extra = [a for a in mission_areas(gd, areas) if not self._wanted_area(a)]
            if extra:
                self.area_patterns += extra
                self.log("Story mode: adding the districts the first mission is played in: %s" % ", ".join(extra))
        selected = {}
        for area, insts in areas.items():
            if self._wanted_area(area):
                keep = select_detail(gd, insts, area, self.detail, with_lods=self.distances)
                if keep:
                    selected[area] = keep
        if not selected:
            raise RuntimeError("No placements matched areas '%s'" % ",".join(self.area_patterns))
        self._add_subway(gd, areas, selected)
        selected = split_interiors(selected)
        need = {}  # model -> smallest size any of its placements accepts
        for v in selected.values():
            for i, min_size in v:
                need[i.hash] = min(need.get(i.hash, min_size), min_size)
        hashes = sorted(need)
        sizes = {}
        self.log("Detail level %s: %d candidate placements of %d models from %d areas"
                 % (self.detail, sum(len(v) for v in selected.values()), len(hashes), len(selected)))

        models = {}  # hash -> {"name", "batch"}
        batch, batch_no, batches = _GltfBatch(), 0, []
        batch_meshes = {}  # gltf file -> mesh names in it (lets the importer resume)

        def flush():
            nonlocal batch, batch_no
            if batch.nodes:
                fn = "batch_%03d.gltf" % batch_no
                batch.write(os.path.join(self.out_dir, "meshes", fn))
                batches.append(fn)
                batch_meshes[fn] = [n["name"] for n in batch.nodes]
                batch_no += 1
                batch = _GltfBatch()

        for n, h in enumerate(hashes):
            mdef = gd.models[h]
            try:
                mesh = gd.load_model(mdef, self._wdd_cache)
            except Exception as e:
                self.log("Failed to read %s: %s" % (mdef.name, e))
                mesh = None
            if mesh is not None and model_size(mesh) < need[h]:
                mesh = None  # too small for this detail level
            if mesh is not None:
                sizes[h] = model_size(mesh)
                slots = [self._material_name(m, mesh, mdef.txd) for m in mesh.materials]
                sm = "SM_" + ue_name(mdef.name)
                batch.add_model(sm, mesh, slots)
                models[h] = {"name": sm, "batch": "batch_%03d" % batch_no}
                if len(batch.nodes) >= BATCH_MODELS:
                    flush()
            if progress:
                progress(n / len(hashes))
            if n % 1000 == 0:
                self.log("  models %d / %d  (%.0fs)" % (n, len(hashes), time.time() - t0))
        flush()

        placements = {}
        lights = {}
        kept_by_area = {}
        for area, insts in sorted(selected.items()):
            kept_by_area[area] = [i for i, min_size in insts
                                  if models.get(i.hash) is not None and sizes[i.hash] >= min_size]
        cull = self._draw_distances(kept_by_area) if self.distances else {}
        for area, kept in sorted(kept_by_area.items()):
            rows = []
            for i in kept:
                row = [models[i.hash]["name"]] + [round(v, 4) for v in i.pos] + [round(v, 6) for v in _quat(i.rot)]
                if self.distances:
                    row += list(cull.get(id(i), (0.0, 0.0)))
                rows.append(row)
            placements[area] = rows
            if self.lights:
                # [spot, x, y, z, dx, dy, dz, r, g, b, range, inner, outer] in GTA space
                lr = [[int(L.spot)] + [round(v, 3) for v in pos] + [round(v, 4) for v in d]
                      + list(L.color) + [L.range, L.inner, L.outer]
                      for pos, d, L in placed_lights(self.gd, kept)]
                if lr:
                    lights[area] = lr
        distant = self._distant_traffic(placements) if self.lights else []
        if self.traffic:
            traffic = self._export_traffic(placements, batches, batch_meshes)
        else:
            traffic = self._road_network(placements)  # sidewalks for pedestrians, no cars
        characters = self._export_characters(batches, batch_meshes, progress) if self.characters else None
        story, vehicles = None, []
        if self.story:
            from .story import export_story
            from .vehicles import load_vehicle_defs
            vdefs, _ = load_vehicle_defs(self.game_dir)
            try:
                story = export_story(self, batches, batch_meshes, _GltfBatch, ue_name, vdefs)
            except Exception as e:
                import traceback
                self.log("Story mode failed: %s\n%s" % (e, traceback.format_exc()))
            if story:
                if characters is None:
                    characters = {"peds": [], "weapons": []}
                have = {p["mesh"] for p in characters["peds"]}
                characters["peds"] += [p for p in story["peds"] if p["mesh"] not in have]
                self.vehicle_filter = sorted(set(self.vehicle_filter) | set(story["vehicles"]))
        if self.vehicle_filter:
            vehicles = self._export_vehicles_only(batches, batch_meshes)
        data = {
            "version": 1,
            "batches": batches,
            "batch_meshes": batch_meshes,
            "textures": sorted({v for v in self.tex_names.values() if v}),
            "materials": self.materials,
            "placements": placements,  # [mesh, x, y, z, qw, qx, qy, qz] in GTA space (metres, right-handed)
            "lights": lights,
            "distant": distant,
            "traffic": traffic,
            "characters": characters,  # [x, y, z, dx, dy, dz, red, phase]: one moving car light per row
            "vehicles": vehicles,
            "story": "story/story.json" if story else None,
        }
        with open(os.path.join(self.out_dir, "gta4_unreal.json"), "w", encoding="utf-8") as f:
            json.dump(data, f)
        for _, a in gd.archives:
            a.close()
        self.log("Wrote %d mesh batches, %d textures, %d materials in %.0fs"
                 % (len(batches), len(data["textures"]), len(self.materials), time.time() - t0))
        return sum(len(v) for v in placements.values())
