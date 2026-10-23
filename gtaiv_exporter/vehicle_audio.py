"""GTA IV's vehicle sounds: every car's engine, exhaust, idle, revs-off, start, horn and door
sounds, plus the tyre skids, as WAVs for the GTAIVTraffic plugin.

How the game ties them together:
  game.dat16  has a settings object per car (named like the model, e.g. SULTAN) with a sound name
              hash (joaat, little-endian) in fixed slots - see SLOTS
  sounds.dat15 turns a sound name into a wave: simple sounds hold the wave's name hash, wrappers
              and random containers point at other sounds (followed until a wave is found)
  the waves are in wave banks: pc/audio/sfx/streamed_vehicles.rpf (one bank per engine type, e.g.
              SUPERCAR_2) and resident.rpf (ENGINE_RESIDENT, HORNS, WHEEL_LOOPS, EXTRAS, ...)

These banks (unlike the speech banks) have fixed 0x20-byte wave infos after the 16-byte rows:
    +0x04 name hash, +0x08 bytes, +0x0C samples, +0x14 u16 sample rate, +0x1C where the wave's
    data ends (0x800-aligned, from the header's data start at +0x18): the waves follow each other
    in info order, the first at the data start. 16-bit PCM when bytes = 2 x samples, else IMA ADPCM.
"""

import json
import os
import re
import struct

import numpy as np

from .audio import Rpf3Archive, _decode_ima, joaat, write_wav

# slot offsets in a car's audio settings object (game.dat16)
SLOTS = {"engine_low": 18, "engine_high": 22, "exhaust_low": 26, "exhaust_high": 30, "revs_off": 34,
         "engine_idle": 146, "exhaust_idle": 150, "start": 206, "horn": 210, "door_open": 214,
         "door_close": 218, "ignition": 310, "shutdown": 314}
LOOPS = ("engine_low", "engine_high", "exhaust_low", "exhaust_high", "engine_idle", "exhaust_idle", "horn")
RESIDENT_BANKS = ("ENGINE_RESIDENT", "HORNS", "WHEEL_LOOPS", "EXTRAS", "DOORS")
# shared by every car (resident.rpf waves)
GLOBAL = {"skid": "MAIN_TARMAC_SKID_A", "skid_side": "SIDE_TARMAC_SKID_A", "skid_wet": "WET_SKID_A",
          "gear": "GEAR_TRANSMISSION"}


def _dat(path):
    """A RAGE audio metadata file -> (data block, {name: (offset, size)})."""
    d = open(path, "rb").read()
    size = struct.unpack_from("<I", d, 4)[0]
    data = d[8:8 + size]

    def table(q):
        cnt = struct.unpack_from("<I", d, q)[0]
        if not 1 < cnt < 300000:
            return None
        q += 8
        out = {}
        for _ in range(cnt):
            if q >= len(d):
                return None
            n = d[q]
            s = d[q + 1:q + 1 + n]
            if n == 0 or not all(32 < c < 127 for c in s):
                return None
            out[s.decode()] = struct.unpack_from("<II", d, q + 1 + n)
            q += 1 + n + 8
        return out
    for q in range(8 + size, len(d) - 8):   # the name table follows other tables of varying size
        t = table(q)
        if t and len(t) > 10:
            return data, t
    return data, {}


def parse_bank(d):
    """-> {wave hash: (rate, samples, bytes, offset, is_pcm)}"""
    hdr = struct.unpack_from("<8I", d, 0)
    n, data0 = hdr[4], hdr[6]
    base = 0x20 + 16 * n
    out = {}
    start = 0
    for k in range(n):
        blk = base + 0x20 * k
        if blk + 0x20 > len(d):
            break
        h = struct.unpack_from("<I", d, blk + 4)[0]
        nbytes, samples = struct.unpack_from("<II", d, blk + 8)
        rate = struct.unpack_from("<H", d, blk + 20)[0]
        end = struct.unpack_from("<I", d, blk + 28)[0]
        if 4000 <= rate <= 96000 and samples and nbytes and data0 + start + nbytes <= len(d):
            out[h] = (rate, samples, nbytes, data0 + start, nbytes >= 2 * samples - 4)
        start = end if end > start else (start + nbytes + 0x7FF) // 0x800 * 0x800
    return out


def wave_pcm(d, info):
    rate, samples, nbytes, off, pcm = info
    raw = d[off:off + nbytes]
    if pcm:
        return rate, np.frombuffer(raw[:min(nbytes, samples * 2) // 2 * 2], "<i2").copy()
    return rate, _decode_ima(raw)[:samples]


class VehicleSounds:
    def __init__(self, game_dir, key):
        cfg = os.path.join(game_dir, "pc", "audio", "config")
        self.game, self.game_names = _dat(os.path.join(cfg, "game.dat16"))
        self.sounds, self.sound_names = _dat(os.path.join(cfg, "sounds.dat15"))
        self.sound_by_hash = {joaat(n): n for n in self.sound_names}
        sfx = os.path.join(game_dir, "pc", "audio", "sfx")
        self.archives = {r: Rpf3Archive(os.path.join(sfx, r + ".rpf"), key)
                         for r in ("streamed_vehicles", "resident") if os.path.isfile(os.path.join(sfx, r + ".rpf"))}
        self.banks, self.waves = {}, {}   # bank data; wave hash -> (bank key, info)
        if "streamed_vehicles" in self.archives:
            a = self.archives["streamed_vehicles"]
            for h in a.entries:
                self._add_bank(("streamed_vehicles", h), a.read(h))
        if "resident" in self.archives:
            a = self.archives["resident"]
            for name in RESIDENT_BANKS:
                d = a.read(name)
                if d:
                    self._add_bank(("resident", name), d)
        self.names = {}   # wave hash -> name (for file names)
        text = open(os.path.join(cfg, "sounds.dat15"), "rb").read()
        for m in re.finditer(rb"[A-Z0-9_]{3,}", text):
            parts = m.group().decode().split("_")
            for i in range(len(parts)):
                s = "_".join(parts[i:])
                h = joaat(s)
                if h in self.waves:
                    self.names.setdefault(h, s)

    def _add_bank(self, key, d):
        try:
            waves = parse_bank(d)
        except Exception:
            return
        if waves:
            self.banks[key] = d
            for h, info in waves.items():
                self.waves.setdefault(h, (key, info))

    def resolve(self, sound_hash, depth=0):
        """A sound (name hash) -> a wave hash in the banks, following wrappers/containers."""
        if sound_hash in self.waves:
            return sound_hash
        name = self.sound_by_hash.get(sound_hash)
        if name is None or depth > 5:
            return None
        o, sz = self.sound_names[name]
        b = self.sounds[o:o + sz]
        refs = [struct.unpack_from("<I", b, i)[0] for i in range(0, max(0, sz - 3))]
        for v in refs:              # a wave directly
            if v in self.waves:
                return v
        for v in refs:              # another sound
            if v in self.sound_by_hash and v != sound_hash:
                w = self.resolve(v, depth + 1)
                if w:
                    return w
        return None

    def car(self, model, aliases=()):
        """{slot: wave hash} for a car (its settings object is named like the model)."""
        name = next((n for n in (model.upper(),) + tuple(n.upper() for n in aliases if n) if n in self.game_names), None)
        if name is None:
            return {}
        o, sz = self.game_names[name]
        b = self.game[o:o + sz]
        out = {}
        for slot, off in SLOTS.items():
            if off + 4 <= len(b):
                w = self.resolve(struct.unpack_from("<I", b, off)[0])
                if w:
                    out[slot] = w
        # loops whose sound isn't named in sounds.dat: the engine's other waves give the prefix
        prefixes = set()
        for slot in LOOPS[:6] + ("revs_off",):
            n = self.names.get(out.get(slot))
            m = re.match(r"(.+?)_(ENGINE|EXHAUST|ENG|EX|REVS)_", n or "")
            if m:
                prefixes.add(m.group(1))
        guesses = {"engine_low": ("ENGINE_LOW",), "engine_high": ("ENGINE_HIGH",), "exhaust_low": ("EXHAUST_LOW",),
                   "exhaust_high": ("EXHAUST_HIGH",), "engine_idle": ("ENGINE_IDLE", "ENG_IDLE_LOOP"),
                   "exhaust_idle": ("EXHAUST_IDLE", "EX_IDLE_LOOP"), "revs_off": ("REVS_OFF", "REVS_OFF_2")}
        for slot, suffixes in guesses.items():
            for pre in sorted(prefixes):
                for suf in suffixes:
                    h = joaat("%s_%s" % (pre, suf))
                    if slot not in out and h in self.waves:
                        out[slot] = h
                        self.names.setdefault(h, "%s_%s" % (pre, suf))
        return out

    def wav(self, wave_hash, out_dir):
        """Writes the wave as <name>.wav once; returns the name."""
        key, info = self.waves[wave_hash]
        name = self.names.get(wave_hash, "%08X" % wave_hash)
        path = os.path.join(out_dir, name + ".wav")
        if not os.path.isfile(path):
            rate, pcm = wave_pcm(self.banks[key], info)
            write_wav(path, rate, pcm.reshape(-1, 1))
        return name

    def close(self):
        for a in self.archives.values():
            a.close()


def export_vehicle_sounds(game_dir, key, models, out_dir, log=print, aliases=None):
    """WAVs of every listed car's sounds -> ({model: {slot: wav name}}, {global slot: wav name})."""
    os.makedirs(out_dir, exist_ok=True)
    vs = VehicleSounds(game_dir, key)
    try:
        cars = {}
        for m in models:
            slots = vs.car(m, (aliases or {}).get(m, ()))
            cars[m] = {slot: vs.wav(w, out_dir) for slot, w in slots.items()}
        glob = {}
        for slot, wave in GLOBAL.items():
            h = joaat(wave)
            if h in vs.waves:
                glob[slot] = vs.wav(h, out_dir)
    finally:
        vs.close()
    files = len([f for f in os.listdir(out_dir) if f.endswith(".wav")])
    log("Vehicle sounds: %d cars, %d sounds (engines, horns, doors, skids)" % (sum(1 for c in cars.values() if c), files))
    return cars, glob
