"""GTA IV's radio: every station of pc/audio/sfx/radio_*.rpf, repacked for the GTAIVTraffic plugin.

The audio isn't decoded: each song, ident, DJ line, advert, news and weather report keeps the
game's own IMA ADPCM data, in a small file the plugin decodes while it plays (.gtr):

    "GTR1", u32 sample rate, u16 channels, u16 0, u32 samples per channel
    per channel: u32 segment count, (u32 ADPCM bytes, u32 samples kept) per segment,
                 u32 byte count, the bytes
    A segment decodes to 2 samples per byte (low nibble first, the decoder's state carries on
    across segments and channels start from 0); only its first "kept" samples are heard (the game
    pads its stream blocks to 0x800-byte chunks).

What each file is comes from its name (the names are found in pc/audio/config/sounds.dat15):
songs and talk segments, ID_xx idents, SOLO_xx DJ monologues, and the DJ banks GENERAL (chat
over song intros), TIME (MORNING/AFTERNOON/EVENING/NIGHT_xx) and TO (TO_AD/TO_NEWS/TO_WEATHER).
radio_adverts, radio_news and radio_weather are shared by all stations. The scheduling rules
(what follows a song, how soon things may repeat) come from game.dat16. radio.json lists it all.
"""

import json
import os
import re
import struct

from .audio import CHUNK, Rpf3Archive, joaat

# station id (radio_<id>.rpf), GXT key hash of its name (MAIN table), fallback name, logo (radio_hud*.wtd)
STATIONS = (
    ("the_vibe", 3580001009, "The Vibe 98.8", "thevibe"),
    ("liberty_rock", 663433476, "Liberty Rock Radio 97.8", "lrr"),
    ("jazz_nation", 2569999442, "Jazz Nation Radio 108.5", "jnr"),
    ("bobby_konders", 903499170, "Massive B Soundsystem 96.9", "massiveb"),
    ("k109_the_studio", 1966946980, "K109 The Studio", "k109"),
    ("wktt", 1937280294, "WKTT Radio", "wktt"),
    ("hardcore", 2816848319, "Liberty City Hardcore", "lchc"),
    ("classical_ambient", 1379927661, "The Journey", "thejourney"),
    ("fusion_fm", 2891655391, "Fusion FM", "fusion"),
    ("beat_95", 738568250, "The Beat 102.7", "beat"),
    ("dance_rock", 1156967385, "Radio Broker", "radiobroker"),
    ("vladivostok", 1039748129, "Vladivostok FM", "vladivostok"),
    ("san_juan_sounds", 3532883734, "San Juan Sounds", "sanjuan"),
    ("ny_classics", 3281082191, "The Classics 104.1", "theclassics"),
    ("afro_beat", 2884079698, "IF99 International Funk", "if99"),
    ("babylon", 2265374263, "Tuff Gong Radio", "tuffgong"),
    ("plr", 2652146774, "Public Liberty Radio", "plr"),
    ("lazlow", 3775276027, "Integrity 2.0", "integrity"),
    ("dance_mix", 0, "Electro-Choc", "electrochoc"),
    ("meditation", 0, "Self-Actualization FM", "selfactualizationfm"),
    ("vcfm", 1244330947, "Vice City FM", "vicecityfm"),
    ("ramjamfm", 0, "RamJam FM", "ramjamfm"),
)
TALK = ("wktt", "plr", "lazlow")
ADVERT_POOL = {"vladivostok": "russian", "san_juan_sounds": "spanish"}
LOGO_DICTS = ("radio_hud", "radio_hud_colored_eflc", "radio_hud_noncolored_eflc")
# radio track categories in game.dat16
CATEGORIES = ("advert", "ident", "music", "news", "weather", "solo", "c6", "c7", "to_ad", "to_news")


# ------------------------------------------------------------------ reading the game's audio

def _is_stream(d):
    if len(d) < 0x40:
        return False
    nb, bs = struct.unpack_from("<II", d, 8)
    nch = struct.unpack_from("<I", d, 0x24)[0]
    return 0 < nch < 8 and 0 < bs and nb * bs <= len(d) + 0x800


def _stream_parts(d):
    """A streamed .ivaud -> (rate, samples per channel, [per channel ([(bytes, kept)], data)])."""
    nblocks, bsize = struct.unpack_from("<II", d, 8)
    cto = struct.unpack_from("<I", d, 0x14)[0]
    nch = struct.unpack_from("<I", d, 0x24)[0]
    hsize = struct.unpack_from("<I", d, 0x2C)[0]
    info = []
    for c in range(nch):
        off = struct.unpack_from("<I", d, cto + 16 * c)[0]
        base = cto + 16 * nch + off
        info.append((struct.unpack_from("<I", d, base + 16)[0], struct.unpack_from("<H", d, base + 24)[0]))
    chans = [([], []) for _ in range(nch)]
    for b in range(nblocks):
        blk = d[hsize + b * bsize:hsize + (b + 1) * bsize]
        if len(blk) < 0x18 + 16 * nch:
            break
        seek = struct.unpack_from("<Q", blk, 8)[0]
        ents = [struct.unpack_from("<IIII", blk, 0x18 + 16 * c) for c in range(nch)]
        last = max(e[0] + e[1] for e in ents)
        start = (seek + 8 * last + CHUNK - 1) // CHUNK * CHUNK
        for c, (first, count, _u, samples) in enumerate(ents):
            raw = blk[start + first * CHUNK:start + (first + count) * CHUNK]
            chans[c][0].append((len(raw), min(samples, len(raw) * 2)))
            chans[c][1].append(raw)
    return info[0][1], min(i[0] for i in info), [(segs, b"".join(parts)) for segs, parts in chans]


def _bank_parts(d):
    """A wave bank -> {name hash: (rate, samples, ADPCM bytes)} (speech banks are 4-bit ADPCM)."""
    count = struct.unpack_from("<I", d, 0x10)[0]
    start = struct.unpack_from("<I", d, 0x18)[0]
    infos, o = [], 0x20 + 16 * count
    while len(infos) < count and o + 0x18 < start:
        _u, h, nbytes, samples, loop = struct.unpack_from("<IIIIi", d, o)
        rate = struct.unpack_from("<H", d, o + 20)[0]
        if loop == -1 and rate in (8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000) and nbytes and samples:
            infos.append((h, nbytes, samples, rate))
            o += 0x20
        else:
            o += 1
    out, pos = {}, start
    for h, nbytes, samples, rate in infos:
        pos = (pos + CHUNK - 1) // CHUNK * CHUNK
        if samples > nbytes:
            out[h] = (rate, samples, d[pos:pos + nbytes])
        pos += nbytes
    return out


def write_gtr(path, rate, samples, channels):
    """channels: [([(bytes, kept)], data)]"""
    with open(path, "wb") as f:
        f.write(b"GTR1" + struct.pack("<IHHI", rate, len(channels), 0, samples))
        for segs, data in channels:
            f.write(struct.pack("<I", len(segs)))
            f.write(b"".join(struct.pack("<II", n, k) for n, k in segs))
            f.write(struct.pack("<I", len(data)))
            f.write(data)


# ------------------------------------------------------------------ names and rules

def _sound_names(game_dir):
    """RADIO_<station>_<name> sound names in sounds.dat15 -> {joaat(name): (station, name)}."""
    d = open(os.path.join(game_dir, "pc", "audio", "config", "sounds.dat15"), "rb").read()
    ids = sorted((s[0].upper() for s in STATIONS), key=len, reverse=True) + ["ADVERTS", "NEWS", "WEATHER"]
    out = {}
    for m in re.finditer(rb"RADIO_[A-Z0-9_]+", d):
        s = re.sub(r"_(POS|STEREO).*$", "", m.group().decode())
        for sid in ids:
            if s.startswith("RADIO_%s_" % sid):
                name = s[len(sid) + 7:]
                for n in (name, name[:-1]):   # names are followed by a byte that may look like a letter
                    if n:
                        out.setdefault(joaat(n), (sid.lower(), n))
                break
    for kind in ("GENERAL", "MORNING", "AFTERNOON", "EVENING", "NIGHT", "TO_AD", "TO_NEWS", "TO_WEATHER"):
        for i in range(1, 60):
            n = "%s_%02d" % (kind, i)
            out.setdefault(joaat(n), (None, n))
    return out


def _rules(game_dir):
    """game.dat16's track category rules: weights after music etc., minimum seconds between repeats."""
    d = open(os.path.join(game_dir, "pc", "audio", "config", "game.dat16"), "rb").read()
    size = struct.unpack_from("<I", d, 4)[0]
    data, p = d[8:8 + size], 8 + size + 8
    cnt = struct.unpack_from("<I", d, p)[0]
    q, names = p + 8, {}
    for _ in range(cnt):
        n = d[q]
        names[d[q + 1:q + 1 + n].decode("latin-1")] = struct.unpack_from("<II", d, q + 1 + n)
        q += 1 + n + 8
    rules = {}
    for key, name in (("after_music", "RADIO_TRACK_CATEGORY_WEIGHTS_FROM_MUSIC"),
                      ("after_two_songs", "RADIO_TRACK_CATEGORY_WEIGHTS_FROM_BACK_TO_BACK_MUSIC"),
                      ("min_repeat_ms", "RADIO_TRACK_CATEGORY_MIN_TIME_BETWEEN_REPEATS")):
        if name not in names:
            continue
        o, sz = names[name]
        b = data[o:o + sz]
        k = b.index(b"\x00\x0a") + 2
        rules[key] = {CATEGORIES[b[k + 5 * i]]: struct.unpack_from("<I", b, k + 5 * i + 1)[0] for i in range(10)
                      if b[k + 5 * i] < len(CATEGORIES)}
    return rules


def _display_names(game_dir):
    try:
        from .gxt import Text
        main = Text(open(os.path.join(game_dir, "common", "text", "american.gxt"), "rb").read()).tables["MAIN"]
    except Exception:
        main = {}
    return {sid: (main.get(h) if h else None) or name for sid, h, name, _logo in STATIONS}


def export_logos(game_dir, out_dir, log=print):
    """Station logos (colour and grey) as PNGs -> {logo name: path}."""
    from PIL import Image
    from . import texture
    os.makedirs(out_dir, exist_ok=True)
    found = {}
    for dct in LOGO_DICTS:
        p = os.path.join(game_dir, "pc", "textures", dct + ".wtd")
        if not os.path.isfile(p):
            continue
        for name, tex in texture.load_wtd(open(p, "rb").read()).items():
            if name in found:
                continue
            path = os.path.join(out_dir, "T_Radio_%s.png" % name)
            rgba = tex.to_rgba()
            if (rgba[..., 3] == 255).all():
                # the colour logos are opaque on black (the game draws the black away): alpha from
                # brightness, colours un-darkened to match
                import numpy as np
                rgb = rgba[..., :3].astype(np.float32)
                a = np.clip(rgb.max(axis=2) * 4.0, 0, 255)
                rgba = rgba.copy()
                rgba[..., :3] = np.clip(rgb * (255.0 / np.maximum(a, 1.0))[..., None], 0, 255).astype(np.uint8)
                rgba[..., 3] = a.astype(np.uint8)
            Image.fromarray(rgba).save(path)
            found[name] = path
    log("Radio logos: %d" % len(found))
    return found


# ------------------------------------------------------------------ the export

def export_radio(game_dir, key, data_dir, logo_dir, log=print, stations=None):
    """Writes data_dir/<station>/*.gtr, the shared adverts/news/weather and data_dir/radio.json.
    Returns the manifest (dict)."""
    sfx = os.path.join(game_dir, "pc", "audio", "sfx")
    names = _sound_names(game_dir)
    display = _display_names(game_dir)
    os.makedirs(data_dir, exist_ok=True)
    total = [0]

    def save(folder, name, rate, samples, chans):
        os.makedirs(os.path.join(data_dir, folder), exist_ok=True)
        rel = "%s/%s.gtr" % (folder, name)
        write_gtr(os.path.join(data_dir, rel), rate, samples, chans)
        total[0] += 1
        return {"file": rel, "len": round(samples / float(rate), 3)}

    def bank_entries(folder, data):
        out = {}
        for h, (rate, samples, raw) in _bank_parts(data).items():
            n = names.get(h, (None, None))[1]
            if n:
                out[n] = save(folder, n, rate, samples, [([(len(raw), samples)], raw)])
        return out

    def archive(rpf):
        path = os.path.join(sfx, rpf + ".rpf")
        return Rpf3Archive(path, key) if os.path.isfile(path) else None

    manifest = {"format": 1, "stations": [], "rules": _rules(game_dir)}

    # shared programmes
    ads = {"generic": [], "russian": [], "spanish": []}
    a = archive("radio_adverts")
    if a:
        for h in a.entries:
            n = names.get(h, (None, None))[1]
            d = a.read(h)
            if not n or not _is_stream(d):
                continue
            pool = "russian" if "RUSSIAN" in n else "spanish" if "SPANISH" in n else "generic"
            ads[pool].append(save("adverts", n, *_stream_parts(d)))
        a.close()
    news = []
    a = archive("radio_news")
    if a:
        for h in a.entries:
            n = names.get(h, (None, None))[1]
            d = a.read(h)
            if n and _is_stream(d):
                news.append(save("news", n, *_stream_parts(d)))
        a.close()
    weather = {}
    a = archive("radio_weather")
    if a:
        for h in a.entries:
            n = names.get(h, (None, None))[1]
            d = a.read(h)
            if n and _is_stream(d):
                weather.setdefault(n.split("_")[0].lower(), []).append(save("weather", n, *_stream_parts(d)))
        a.close()
    manifest.update(adverts=ads, news=news, weather=weather)
    log("Radio: %d adverts, %d news, %d weather reports" % (sum(len(v) for v in ads.values()), len(news),
                                                            sum(len(v) for v in weather.values())))

    # songs some stations keep in radio_extratracks.rpf
    extra = {}
    a = archive("radio_extratracks")
    if a:
        for h in a.entries:
            sid, n = names.get(h, (None, None))
            if sid and n:
                extra.setdefault(sid, []).append((h, n))
        a.close()

    for sid, _gxt, _fallback, logo in STATIONS:
        if stations is not None and sid not in stations:
            continue
        a = archive("radio_" + sid)
        if a is None:
            continue
        st = {"id": sid, "name": display[sid], "logo": logo, "talk": sid in TALK,
              "adverts": ADVERT_POOL.get(sid, "generic"),
              "music": [], "idents": [], "solo": [], "general": [], "time": {}, "to_ad": [], "to_news": [],
              "to_weather": []}
        for h in a.entries:
            d = a.read(h)
            n = names.get(h, (None, None))[1]
            if not _is_stream(d):
                for line, e in bank_entries(sid, d).items():
                    kind = line.rsplit("_", 1)[0].lower()
                    if kind in ("morning", "afternoon", "evening", "night"):
                        st["time"].setdefault(kind, []).append(e)
                    elif kind in ("general", "to_ad", "to_news", "to_weather"):
                        st[kind].append(e)
                continue
            if not n:
                n = "%08x" % h
            e = save(sid, n, *_stream_parts(d))
            if re.match(r"ID_\d+$", n):
                st["idents"].append(e)
            elif re.match(r"SOLO_\d+$", n):
                st["solo"].append(e)
            elif e["len"] > 45 or st["talk"]:
                st["music"].append(e)
            else:
                st["idents"].append(e)
        a.close()
        if sid in extra:
            a = archive("radio_extratracks")
            for h, n in extra[sid]:
                d = a.read(h)
                if _is_stream(d):
                    st["music"].append(save(sid, n, *_stream_parts(d)))
            a.close()
        if not st["music"]:
            continue
        manifest["stations"].append(st)
        log("Radio: %s - %d songs, %d idents, %d DJ lines" % (st["name"], len(st["music"]), len(st["idents"]),
                                                             len(st["solo"]) + len(st["general"]) +
                                                             sum(len(v) for v in st["time"].values())))
    with open(os.path.join(data_dir, "radio.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
    manifest["logos"] = export_logos(game_dir, logo_dir, log)
    log("Radio: %d stations, %d audio files" % (len(manifest["stations"]), total[0]))
    return manifest
