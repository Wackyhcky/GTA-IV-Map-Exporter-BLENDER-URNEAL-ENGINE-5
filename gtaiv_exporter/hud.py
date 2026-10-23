"""GTA IV's in-game HUD, read from the game's data:

    common/data/hud.dat        element positions/sizes/colour/alpha ([HD] section)
    common/data/hudColor.dat   the HUD colour palette
    common/data/info.zon       area name boxes (GXT keys), map.zon the boroughs
    pc/models/cdimages/radar.img   radarNN.wtd minimap tiles: a 12 x 9 grid of 500 m tiles whose
                                   top-left corner is at world (-3000, 3000), tile = row * 12 + col
    pc/textures/hud.wtd, blips.wtd radar ring, player arrow, north, blips, crosshair, fist
"""

import json
import os
import re

import numpy as np

from . import img, texture
from .frontend import _data_file
from .gxt import Text

RADAR_COLS, RADAR_ROWS, RADAR_TILE, RADAR_ORIGIN = 12, 9, 500.0, (-3000.0, 3000.0)
TILE_PX = 341                               # atlas: 12 * 341 = 4092 px wide
WATER = (99, 125, 148)
HUD_TEXTURES = ("radarringback", "radarringfront", "hud_crosshair", "hud_target", "fist_icon", "throw_icon")


def parse_hud(text, section="HD"):
    out, inside = {}, False
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line or line.startswith(";"):
            continue
        if line.startswith("["):
            inside = line == "[%s]" % section
            continue
        if inside:
            p = re.sub(r",\s+", ",", line).split()
            if len(p) >= 5:
                pos = [float(v) for v in p[1].split(",")]
                size = [float(v) for v in p[2].split(",")]
                out[p[0]] = {"pos": pos, "size": size, "colour": p[3], "alpha": int(p[4])}
    return out


def parse_colours(text, section="HD"):
    out, inside = {}, False
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith("["):
            inside = line == "[%s]" % section
            continue
        p = line.split()
        if inside and len(p) >= 4:
            out[p[0]] = [int(p[1]), int(p[2]), int(p[3])]
    return out


def parse_zones(text, gxt):
    zones = []
    for line in text.splitlines():
        p = [s.strip() for s in line.split(",")]
        if len(p) < 10:
            continue
        name = gxt.get(p[-1]) or ""
        if not name:
            continue
        x0, y0, z0, x1, y1, z1 = (float(v) for v in p[2:8])
        zones.append({"name": name, "min": [min(x0, x1), min(y0, y1)], "max": [max(x0, x1), max(y0, y1)]})
    return zones


# network.wtd icons the phone's multiplayer pages use
PHONE_NETWORK_ICONS = ("icon_gta", "icon_invite", "icon_w_player", "icon_w_arrow_right", "icon_w_arrow_up",
                       "icon_w_notconnected", "icon_w_kills", "icon_w_deaths", "icon_w_score", "icon_w_ranking")

def export_hud(game_dir, key, out_dir, log=print):
    from PIL import Image
    os.makedirs(os.path.join(out_dir, "textures"), exist_ok=True)
    gxt = Text(open(os.path.join(game_dir, "common", "text", "american.gxt"), "rb").read())
    read = lambda rel: open(_data_file(game_dir, rel), encoding="latin-1").read()
    layout = parse_hud(read("common/data/hud.dat"))
    colours = parse_colours(read("common/data/hudColor.dat"))
    zones = parse_zones(read("common/data/info.zon"), gxt)

    # radar atlas
    atlas = Image.new("RGB", (RADAR_COLS * TILE_PX, RADAR_ROWS * TILE_PX), WATER)
    arc = img.ImgArchive(os.path.join(game_dir, "pc", "models", "cdimages", "radar.img"), key)
    n = 0
    try:
        for name in arc.names():
            m = re.fullmatch(r"radar(\d+)\.wtd", name)
            if not m:
                continue
            i = int(m.group(1))
            t = list(texture.load_wtd(arc.read(name)).values())[0]
            tile = Image.fromarray(t.to_rgba(), "RGBA")
            bg = Image.new("RGBA", tile.size, WATER + (255,))
            bg.alpha_composite(tile)
            atlas.paste(bg.convert("RGB").resize((TILE_PX, TILE_PX), Image.LANCZOS),
                        ((i % RADAR_COLS) * TILE_PX, (i // RADAR_COLS) * TILE_PX))
            n += 1
    finally:
        arc.close()
    atlas.save(os.path.join(out_dir, "textures", "T_HUD_RadarMap.png"))

    textures = [{"name": "radar_map", "asset": "T_HUD_RadarMap"}]
    for wtd, keep in (("hud", HUD_TEXTURES), ("blips", None), ("network", PHONE_NETWORK_ICONS)):
        texs = texture.load_wtd(open(os.path.join(game_dir, "pc", "textures", wtd + ".wtd"), "rb").read())
        for name, t in texs.items():
            if keep and name not in keep and not (wtd == "hud" and name.startswith("phone_icon_")):
                continue
            stem = "T_HUD_" + name
            with open(os.path.join(out_dir, "textures", stem + ".png"), "wb") as f:
                f.write(t.to_png(level=6))
            textures.append({"name": name, "asset": stem})

    # the phone: the handset Niko holds (weapons.img amb_mobile_1: its face with the screen and keys)
    try:
        from .img import ImgArchive
        arc = ImgArchive(os.path.join(game_dir, "pc", "models", "cdimages", "weapons.img"), key)
        t = texture.load_wtd(arc.read("amb_mobile_1.wtd")).get("cj_mobile_comp")
        if t is not None:
            with open(os.path.join(out_dir, "textures", "T_HUD_phone_body.png"), "wb") as f:
                f.write(t.to_png(level=6))
            textures.append({"name": "phone_body", "asset": "T_HUD_phone_body"})
    except Exception as e:
        log("No phone handset: %s" % e)

    data = {"layout": layout, "colours": colours, "zones": zones, "textures": textures,
            "radar": {"cols": RADAR_COLS, "rows": RADAR_ROWS, "tile": RADAR_TILE, "origin": list(RADAR_ORIGIN)}}
    with open(os.path.join(out_dir, "hud.json"), "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=1)
    log("HUD: %d radar tiles, %d zones, %d textures, %d layout entries" % (n, len(zones), len(textures), len(layout)))
    return data
