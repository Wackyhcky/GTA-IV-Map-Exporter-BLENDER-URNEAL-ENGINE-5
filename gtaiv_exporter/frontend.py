"""GTA IV's startup sequence and front-end menus, read from the game's own data:

    common/data/loadingscreens_pc.dat   startup + loading screens: per screen a duration, fade and
                                        animated texture objects (16:9 section)
    common/data/frontend_menus.xml      every menu page and option, display values (On/Off, ...)
    common/data/frontend_pc.dat         menu layout (normalised screen positions/sizes, [HD])
    common/data/fonts.dat               font atlas mapping and proportional widths
    common/text/american.gxt            all text
    pc/textures/loadingscreens.wtd, frontend_360.wtd, fonts.wtd

Files in GTAIV/update/ take priority (the patched PC versions).
"""

import json
import os
import re
import xml.etree.ElementTree as ET

from . import texture
from .gxt import Text

# the PC title menu (SCR_TITLE) is filled in by the game's code; these are its working items
TITLE_ITEMS = [  # (text to find in the GXT, action, target)
    ("Start New Game", "NEW_GAME", ""),
    ("Load Game", "LOAD_GAME", ""),
    ("Options", "MENU", "SCR_TOPTIONS"),
    ("Graphics", "MENU", "SCR_TGRAPHICS"),
    ("Exit Game", "QUIT", ""),
]
# the option pages reachable from the title menu
TITLE_MENUS = ("SCR_TOPTIONS", "SCR_TCONTROL", "SCR_TAUDIO", "SCR_TDISPLAY", "SCR_TGRAPHICS",
               "SCR_KEYBOARD_OPTIONS", "SCR_CONTROLLER_OPTIONS")
TEXTURE_DICTS = ("loadingscreens", "frontend_360", "fonts")
# the PC title screen (Niko, the skyline, the logo) per aspect ratio: GTAIV/update has the
# Complete Edition one (title_no_site), the original has title_offline_<lang>
TITLE_DICTS = ("title_no_site", "title_offline_en")
TITLE_ASPECTS = ("offline_w", "offline_21by9", "offline_16by10", "offline_4by3", "offline_5by4")
TITLE_BAR = ("Start", "Options", "Social Club", "Back")
# GXT keys (hashes) of texts the game builds in code
GXT_LOADING = 1858434502   # "Loading -"
GXT_WASTED = 3384366314    # "Wasted"


def _data_file(game_dir, rel):
    for base in (os.path.join(game_dir, "update"), game_dir):
        p = os.path.join(base, *rel.split("/"))
        if os.path.isfile(p):
            return p
    return None


def parse_loadingscreens(text, section="16:9"):
    """[{flag, duration, fade, objects: [{texture, layer, start, end, move_time, scale_time,
    pos, move, size, scale}]}] for one aspect-ratio section."""
    screens, cur, inside = [], None, False
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]") and line not in ("[BEGIN]", "[END]"):
            inside = line[1:-1] == section
            continue
        if not inside:
            continue
        if line == "[BEGIN]":
            cur = {"flag": "NONE", "duration": 0, "fade": "NONE", "objects": {}}
        elif line == "[END]":
            objs = [cur["objects"][k] for k in sorted(cur["objects"])]
            cur["objects"] = objs
            screens.append(cur)
            cur = None
        elif cur is not None and ":" in line:
            key, val = [s.strip() for s in line.split(":", 1)]
            m = re.match(r"(\w+?)\s*(\d+)$", key)
            if m:  # per-object field, e.g. "StartPos 1"
                name, idx = m.group(1), int(m.group(2))
                o = cur["objects"].setdefault(idx, {})
                nums = [float(v) for v in val.split()] if name != "Texture" else None
                if name == "Texture":
                    parts = val.split()
                    o["texture"] = parts[0]
                    o["layer"] = parts[1] if len(parts) > 1 else "BEFORE"
                elif name == "StartTime":
                    o["start"] = nums[0]
                elif name == "EndTime":
                    o["end"] = nums[0]
                elif name == "MoveTime":
                    o["move_time"] = nums[0]
                elif name == "ScaleTime":
                    o["scale_time"] = nums[0]
                elif name == "StartPos":
                    o["pos"] = nums[:2]
                elif name == "MoveAmount":
                    o["move"] = nums[:2]
                elif name == "StartSize":
                    o["size"] = nums[:2]
                elif name == "ScaleAmount":
                    o["scale"] = nums[:2]
            elif key == "Flag":
                cur["flag"] = val
            elif key == "Duration":
                cur["duration"] = float(val)
            elif key == "Fade":
                cur["fade"] = val
    return screens


def parse_layout(text, section="HD"):
    out, inside = {}, False
    for raw in text.splitlines():
        line = raw.split("//", 1)[0].strip()
        if not line:
            continue
        if line.startswith("["):
            inside = line == "[%s]" % section
            continue
        if inside:
            parts = line.split()
            if len(parts) >= 3:
                try:
                    out[parts[0]] = [float(parts[1]), float(parts[2])]
                except ValueError:
                    pass
    return out


def parse_all_fonts(text):
    """Every font block of fonts.dat (FONT1, FONT3...) as {section: numbers}."""
    fonts, cur, section, block = [], None, None, []

    def flush():
        if cur is None or section is None:
            return
        cur[section] = [int(t) for line in block for t in line.split() if re.fullmatch(r"-?\d+", t)]

    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if line.startswith("["):
            flush()
            name = line.strip("[]")
            block = []
            if name.startswith("/"):
                section = None
                continue
            if name == "FONT_ID":
                cur = {}
                fonts.append(cur)
            section = name if cur is not None else None
            continue
        if section and line:
            block.append(line)
    flush()
    return fonts


def parse_fonts(text):
    """First font (FONT1: HelveticaCondensed + Pricedown): cell -> char code map, proportional
    trims, spacing; all in the 512-wide base resolution with 32-unit cells."""
    fonts, cur, section, block = [], None, None, []

    def flush():
        if cur is None or section is None:
            return
        nums = [int(t) for line in block for t in line.split() if re.fullmatch(r"-?\d+", t)]
        cur[section] = nums

    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if line.startswith("["):
            flush()
            name = line.strip("[]")
            block = []
            if name.startswith("/"):
                section = None
                continue
            if name == "FONT_ID":
                cur = {}
                fonts.append(cur)
            section = name if cur is not None else None
            continue
        if section and line:
            block.append(line)
    flush()
    f = fonts[0]
    return {"map": f.get("MAP", []), "prop": f.get("PROP", []), "unprop": (f.get("UNPROP") or [32])[0],
            "spacing": f.get("SPACE_BETWEEN_CHARS", [0, 0, 0]), "whitespace": (f.get("WHITESPACE") or [8])[0],
            "main": f.get("MAINFONT", [0, 0]), "sub1": f.get("SUBFONT_1", [0, 0]),
            "common": f.get("COMMON_FONT", [0, 0])}


def parse_menus(xml_text, text):
    root = ET.fromstring(xml_text)

    def T(k):
        """GXT text for a key; the patched PC menus also use plain text (or CamelCase) as the key."""
        if not k:
            return ""
        v = text.get(k)
        if v:
            return v
        if " " in k or not re.fullmatch(r"[A-Z0-9_]+", k):
            return re.sub(r"(?<=[a-z])(?=[A-Z])", " ", k)
        return ""
    displays = {}
    for m in root.find("sMenuDisplayValue"):
        if m.tag not in ("menu", "menupc"):
            continue
        displays[m.get("enum")] = [T(o.get("text")) for o in m if o.tag in ("options", "optionspc")]
    menus = {}
    for m in root.find("sMenuScreen"):
        if m.tag not in ("menu", "menupc"):
            continue
        opts = []
        for o in m:
            if o.tag not in ("options", "optionspc"):
                continue
            if o.get("action") == "END_OF_MENU_OPTIONS":
                break
            opts.append({"label": T(o.get("label")), "key": o.get("label", ""), "action": o.get("action", ""),
                         "value": o.get("value", ""), "scaler": int(o.get("scaler", "0") or 0),
                         "display": o.get("displayValue", "MENU_DISPLAY_NONE")})
        menus[m.get("enum")] = {"header": T(m.get("HeaderText")), "options": opts}
    return menus, displays


def _find_text(text, wanted):
    for table in ("MAIN",):
        for v in text.tables[table].values():
            if v == wanted:
                return v
    return wanted


# pc/audio/sfx/loading.rpf: the loading-screen theme (loops behind the startup and menus), the
# sting played as the game starts, and the menu music
MUSIC = {"loading": "loading_tune", "start": "starting_tune", "menu": "menu_streamed"}


def _export_music(game_dir, key, out_dir, log):
    from .audio import export_tracks
    try:
        done = export_tracks(game_dir, key, MUSIC.values(), os.path.join(out_dir, "audio"), log=log)
    except Exception as e:  # music is optional: the front end still works without it
        log("Couldn't read the loading music: %s" % e)
        return {}
    if done:
        log("Music: " + ", ".join("%s (%d:%02d)" % (n, s // 60, s % 60) for n, (_w, s) in done.items()))
    return {role: name for role, name in MUSIC.items() if name in done}


def export_frontend(game_dir, key, out_dir, log=print):
    """Write the front end's textures (PNG) and frontend.json to out_dir; returns the json dict."""
    os.makedirs(os.path.join(out_dir, "textures"), exist_ok=True)
    text = Text(open(os.path.join(game_dir, "common", "text", "american.gxt"), "rb").read())
    read = lambda rel: open(_data_file(game_dir, rel), encoding="latin-1").read()
    screens = parse_loadingscreens(read("common/data/loadingscreens_pc.dat"))
    layout = parse_layout(read("common/data/frontend_pc.dat"))
    font = parse_fonts(read("common/data/fonts.dat"))
    menus, displays = parse_menus(open(_data_file(game_dir, "common/data/frontend_menus.xml"), "rb").read(), text)

    textures = []
    used = {o["texture"] for s in screens for o in s["objects"]}
    for wtd in TEXTURE_DICTS:
        path = os.path.join(game_dir, "pc", "textures", wtd + ".wtd")
        texs = texture.load_wtd(open(path, "rb").read())
        for name, t in texs.items():
            if wtd == "frontend_360" and name not in ("cursor", "mousecursor", "scroll_fade", "horiz_stats_line",
                                                       "icon_w_arrow_left", "icon_w_arrow_up", "controller"):
                continue
            if wtd == "loadingscreens" and name not in used and not re.fullmatch(r"\d+_\d", name):
                continue
            stem = "T_FE_" + name
            with open(os.path.join(out_dir, "textures", stem + ".png"), "wb") as f:
                f.write(t.to_png(level=6))
            textures.append({"name": name, "asset": stem, "dict": wtd, "width": t.width, "height": t.height})

    # title screen backgrounds
    title = {}
    for wtd in TITLE_DICTS:
        path = _data_file(game_dir, "pc/textures/%s.wtd" % wtd)
        if not path or title:
            continue
        for name, t in texture.load_wtd(open(path, "rb").read()).items():
            short = name.split(":")[-1].lower()  # the update's names carry a "memory:$...:" prefix
            if short in TITLE_ASPECTS:
                stem = "T_FE_title_" + short.replace("offline_", "")
                with open(os.path.join(out_dir, "textures", stem + ".png"), "wb") as f:
                    f.write(t.to_png(level=6))
                textures.append({"name": "title_" + short.replace("offline_", ""), "asset": stem, "dict": wtd,
                                 "width": t.width, "height": t.height})
                title[short] = stem
    # font3's "garage" style: the in-game loading text
    all_fonts = parse_all_fonts(read("common/data/fonts.dat"))
    font3 = {}
    if len(all_fonts) > 1:
        f3 = all_fonts[1]
        font3 = {"map": f3.get("MAP", []), "prop": f3.get("PROP", []), "sub2": f3.get("SUBFONT_2", [92, 164]),
                 "whitespace": (f3.get("WHITESPACE") or [8])[0]}
    main = text.tables.get("MAIN", {})

    data = {
        "screens": screens,
        "layout": layout,
        "font": font,
        "font_texture": "T_FE_font1",
        "menus": {k: menus[k] for k in TITLE_MENUS if k in menus},
        "displays": displays,
        "title": {"header": text.get("MH_TITLE") or _find_text(text, "Main Menu"),
                  "items": [{"label": _find_text(text, label), "action": act, "value": val}
                            for label, act, val in TITLE_ITEMS]},
        "strings": {
            "LEGAL_PC": text.get("LEGAL_PC") or "",
            "LEGAL_PC_2": text.get("LEGAL_PC_2") or "",
            "BACK": text.get("FE_BACK") or "Back",
            "SELECT": text.get("FE_SELECT") or "Select",
            "NO_SAVES": _find_text(text, "Load Game"),
        },
        "textures": textures,
        "title_bar": [_find_text(text, t) or t for t in TITLE_BAR],
        "font3": font3,
        "loading_text": main.get(GXT_LOADING, "Loading -"),
        "wasted_text": main.get(GXT_WASTED, "Wasted"),
        "music": _export_music(game_dir, key, out_dir, log),
    }
    with open(os.path.join(out_dir, "frontend.json"), "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=1)
    log("Front end: %d screens, %d menus, %d textures" % (len(screens), len(data["menus"]), len(textures)))
    return data
