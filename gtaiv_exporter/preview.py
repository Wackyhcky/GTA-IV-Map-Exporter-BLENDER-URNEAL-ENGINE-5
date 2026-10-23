"""Small preview images of peds and weapons, drawn straight from the game files.

A simple software renderer: every triangle is filled with its texture's colour at the triangle's
UV centre, lit by one directional light, and drawn back to front (painter's algorithm) at twice
the size, then scaled down for smooth edges. Needs Pillow.
"""

import numpy as np

BG = (43, 43, 43)          # the app's card colour
LIGHT = np.array([-0.35, -0.6, 0.72])


def _rgba_small(tex, max_side=128):
    """RGBA array of a texture, reduced so sampling is cheap (uses a smaller mip when it can)."""
    rgba = tex.to_rgba()
    step = max(1, max(rgba.shape[0], rgba.shape[1]) // max_side)
    return rgba[::step, ::step]


def render(mesh, textures, size=160, kind="ped", bg=BG, paint=None):
    """PIL image of `mesh` (a drawable.Mesh). textures: name -> texture.Texture (the mesh's own
    embedded ones are used too). kind "ped": front view of a standing character (Unreal-facing
    ped, see peds.TO_UNREAL); "item": side view along the object's thinnest axis; "vehicle":
    three-quarter view from the front left, its paint materials tinted with paint[slot-1]."""
    from PIL import Image, ImageDraw

    P = np.asarray(mesh.positions, np.float64)
    F = np.asarray(mesh.faces, np.int64)
    if not len(P) or not len(F):
        return Image.new("RGB", (size, size), bg)
    if kind == "ped":
        # peds face -X... in the exporter's Unreal orientation they face +Y (Unreal) = -Y (GTA space)
        right, up, toward = P[:, 0], P[:, 2], -P[:, 1]
        sx, sy, sz = -1.0, 1.0, 1.0
        X, Y, Z = sx * right, sy * up, sz * toward
    elif kind == "vehicle":
        # GTA space: x right, y forward, z up; turn 35 degrees and look 22 degrees down
        yaw, pitch = np.radians(-145.0), np.radians(-22.0)
        x0 = P[:, 0] * np.cos(yaw) - P[:, 1] * np.sin(yaw)
        y0 = P[:, 0] * np.sin(yaw) + P[:, 1] * np.cos(yaw)
        X = x0
        Y = P[:, 2] * np.cos(pitch) - y0 * np.sin(pitch)
        Z = -(P[:, 2] * np.sin(pitch) + y0 * np.cos(pitch))
    else:
        ext = P.max(0) - P.min(0)
        view = int(np.argmin(ext))       # look along the thinnest axis
        rest = [k for k in range(3) if k != view]
        up = 2 if 2 in rest else max(rest, key=lambda k: ext[k])   # keep the game's up axis if we can
        across = next(k for k in rest if k != up)
        X, Y, Z = P[:, across], P[:, up], P[:, view]
    S = 2 * size
    margin = 0.08 * S
    w = max(X.max() - X.min(), 1e-6)
    h = max(Y.max() - Y.min(), 1e-6)
    scale = (S - 2 * margin) / max(w, h)
    px = (X - (X.min() + X.max()) / 2) * scale + S / 2
    py = S / 2 - (Y - (Y.min() + Y.max()) / 2) * scale

    # per-face shading from the face normal (in view space)
    V = np.stack([X, Y, Z], 1)
    n = np.cross(V[F[:, 1]] - V[F[:, 0]], V[F[:, 2]] - V[F[:, 0]])
    n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
    light = LIGHT / np.linalg.norm(LIGHT)
    shade = 0.45 + 0.55 * np.abs(n @ light)      # two-sided: GTA's winding varies between parts

    # per-face colour: texture at the face's UV centre
    col = np.full((len(F), 3), 170.0)
    alpha = np.ones(len(F))
    uv = np.asarray(mesh.uvs, np.float64)
    fm = np.asarray(mesh.face_mats, np.int64)
    cache = {}
    for mi, m in enumerate(mesh.materials):
        sel = fm == mi
        if not sel.any():
            continue
        slot = 0
        if paint:
            from .vehicles import paint_slot
            slot = paint_slot(m.shader or "")
        tint = np.array(paint[min(slot, len(paint)) - 1], np.float64) / 255.0 if slot else None
        if tint is not None:
            col[sel] = 255.0 * tint
        name = (m.diffuse or "").lower()
        tex = mesh.textures.get(m.diffuse) or textures.get(m.diffuse) or textures.get(name)
        if tex is None:
            continue
        if name not in cache:
            try:
                cache[name] = _rgba_small(tex)
            except Exception:
                cache[name] = None
        img = cache[name]
        if img is None:
            continue
        th, tw = img.shape[:2]
        c = uv[F[sel]].mean(1)
        u = np.mod(c[:, 0], 1.0)
        v = np.mod(c[:, 1], 1.0)
        tx = np.clip((u * tw).astype(int), 0, tw - 1)
        ty = np.clip((v * th).astype(int), 0, th - 1)
        texel = img[ty, tx].astype(np.float64)
        col[sel] = texel[:, :3] * tint if tint is not None else texel[:, :3]
        alpha[sel] = texel[:, 3] / 255.0
    rgb = np.clip(col * shade[:, None], 0, 255).astype(np.uint8)

    depth = Z[F].mean(1)
    keep = alpha > 0.35                              # cut-out hair / lace
    order = np.argsort(depth)                         # far (small Z) first
    im = Image.new("RGB", (S, S), bg)
    d = ImageDraw.Draw(im)
    tri_x, tri_y = px[F], py[F]
    for i in order:
        if not keep[i]:
            continue
        r, g, b = rgb[i]
        d.polygon([(tri_x[i, 0], tri_y[i, 0]), (tri_x[i, 1], tri_y[i, 1]), (tri_x[i, 2], tri_y[i, 2])],
                  fill=(int(r), int(g), int(b)))
    return im.resize((size, size), Image.LANCZOS)
