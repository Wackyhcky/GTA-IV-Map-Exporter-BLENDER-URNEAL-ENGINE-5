"""GTA IV streamed audio: RPF3 archives (pc/audio/sfx/*.rpf) and the multichannel IMA ADPCM
streams inside them, e.g. loading.rpf's loading_tune (the loading-screen theme).

RPF3 header (at 0): "RPF3", u32 TOC size, u32 entry count, u32 ?, u32 encrypted
TOC (at 0x800, AES like RPF2): 16-byte entries like RPF2, but the first field is the
    joaat hash of the name instead of a name-table offset (there is no name table).

Stream (all little-endian):
    0x08 u32 block count, 0x0C u32 block size, 0x14 u32 channel table offset,
    0x24 u32 channel count, 0x2C u32 header size (blocks start there)
    channel table: per channel u32 info offset, u32 0, u32 hash, u32 info size;
    channel info (after the table): u64 0, u32 hash, u32 bytes, u32 samples, u32 loop,
        u16 sample rate, s16 headroom, ...
    block: u64 0x18, u64 seek table offset, u64 ?, then per channel
        u32 first chunk, u32 chunk count, u32 ?, u32 samples; the seek table; then the
        0x800-byte chunks (from the next 0x800 boundary). Each channel's chunks are
        IMA ADPCM, low nibble first, with its state running on through the whole stream.
"""

import os
import struct
import wave

import numpy as np

from . import aes

CHUNK = 0x800

# names found in pc/audio/config (sounds.dat15) whose hashes match loading.rpf's files
KNOWN_NAMES = ("loading", "loading_tune", "starting_tune", "menu_streamed", "menu_1",
               "intro_cutscene_track", "end_credits_theme_tune", "30sec_countdown", "testwav",
               "death_01", "death_02", "death_03", "death_04", "death_05", "death_06")

_STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80,
         88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598,
         658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
         3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
         16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]
_ADJ = [-1, -1, -1, -1, 2, 4, 6, 8]


def joaat(s):
    h = 0
    for c in s.lower().encode():
        h = (h + c) & 0xFFFFFFFF
        h = (h + (h << 10)) & 0xFFFFFFFF
        h ^= h >> 6
    h = (h + (h << 3)) & 0xFFFFFFFF
    h ^= h >> 11
    return (h + (h << 15)) & 0xFFFFFFFF


class Rpf3Archive:
    """Files by name hash; read(name) hashes the name."""

    def __init__(self, path, key):
        self.path = path
        self.entries = {}   # hash -> (offset, size, flags)
        self._f = open(path, "rb")
        head = self._f.read(20)
        if head[:4] != b"RPF3":
            raise RuntimeError("%s is not an RPF3 archive" % path)
        toc_size, count, _unk, encrypted = struct.unpack_from("<4I", head, 4)
        self._f.seek(0x800)
        toc = self._f.read(toc_size)
        if encrypted:
            toc = aes.decrypt(toc, key)
        for i in range(count):
            h, size, off, flags = struct.unpack_from("<4I", toc, i * 16)
            if not off & 0x80000000:  # not a directory
                self.entries[h] = (off & 0xFFFFFF00 if flags & 0x80000000 else off, size, flags)

    def read(self, name):
        e = self.entries.get(name if isinstance(name, int) else joaat(name))
        if e is None:
            return None
        self._f.seek(e[0])
        return self._f.read(e[1])

    def close(self):
        self._f.close()


def _tables():
    diff = np.zeros((89, 16), np.int32)
    nxt = np.zeros((89, 16), np.int32)
    for i, s in enumerate(_STEP):
        for n in range(16):
            d = s >> 3
            if n & 4:
                d += s
            if n & 2:
                d += s >> 1
            if n & 1:
                d += s >> 2
            diff[i, n] = -d if n & 8 else d
            nxt[i, n] = min(88, max(0, i + _ADJ[n & 7]))
    # per byte (low nibble first): index after the first nibble and after both
    b = np.arange(256)
    mid = nxt[:, b & 15]
    after = nxt[mid, b >> 4]
    return diff, mid, after


_DIFF, _MID, _AFTER = _tables()


def _decode_ima(data):
    """IMA ADPCM bytes (low nibble first, state from 0) -> int16 samples."""
    if not data:
        return np.zeros(0, np.int16)
    after = _AFTER.tolist()
    idx = np.empty(len(data), np.int32)   # step index before each byte
    i = 0
    out = idx  # local alias for speed
    for k, byte in enumerate(data):
        out[k] = i
        i = after[i][byte]
    b = np.frombuffer(data, np.uint8).astype(np.int32)
    d = np.empty(len(data) * 2, np.int64)
    d[0::2] = _DIFF[idx, b & 15]
    d[1::2] = _DIFF[_MID[idx, b], b >> 4]
    # running sum, clamped to int16 like the decoder: segment by segment, exactly only where it clips
    out = np.empty(len(d), np.int16)
    pred = 0
    seg = 4096
    for a in range(0, len(d), seg):
        part = pred + np.cumsum(d[a:a + seg])
        if part.max() > 32767 or part.min() < -32768:
            p = pred
            vals = d[a:a + seg].tolist()
            for k, v in enumerate(vals):
                p = min(32767, max(-32768, p + v))
                vals[k] = p
            part = np.array(vals, np.int64)
        out[a:a + seg] = part
        pred = int(part[-1])
    return out


def decode_stream(data):
    """A streamed .ivaud -> (sample rate, int16 array [samples, channels])."""
    nblocks, bsize = struct.unpack_from("<II", data, 8)
    cto = struct.unpack_from("<I", data, 0x14)[0]
    nch = struct.unpack_from("<I", data, 0x24)[0]
    hsize = struct.unpack_from("<I", data, 0x2C)[0]
    info = []
    for c in range(nch):
        off = struct.unpack_from("<I", data, cto + 16 * c)[0]
        base = cto + 16 * nch + off
        samples = struct.unpack_from("<I", data, base + 16)[0]
        rate = struct.unpack_from("<H", data, base + 24)[0]
        info.append((samples, rate))
    chans = [[] for _ in range(nch)]
    keep = [[] for _ in range(nch)]
    for b in range(nblocks):
        blk = data[hsize + b * bsize:hsize + (b + 1) * bsize]
        if len(blk) < 0x18 + 16 * nch:
            break
        seek = struct.unpack_from("<Q", blk, 8)[0]
        ents = [struct.unpack_from("<IIII", blk, 0x18 + 16 * c) for c in range(nch)]
        last = max(e[0] + e[1] for e in ents)
        start = (seek + 8 * last + CHUNK - 1) // CHUNK * CHUNK
        for c, (first, count, _u, samples) in enumerate(ents):
            chans[c].append(blk[start + first * CHUNK:start + (first + count) * CHUNK])
            keep[c].append((count * CHUNK * 2, samples))
    out = []
    for c in range(nch):
        pcm = _decode_ima(b"".join(chans[c]))
        parts, pos = [], 0
        for total, samples in keep[c]:
            parts.append(pcm[pos:pos + min(samples, total)])
            pos += total
        out.append(np.concatenate(parts)[:info[c][0]])
    n = min(len(x) for x in out)
    return info[0][1], np.stack([x[:n] for x in out], axis=1)


def write_wav(path, rate, pcm):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with wave.open(path, "wb") as w:
        w.setnchannels(pcm.shape[1])
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(np.ascontiguousarray(pcm, "<i2").tobytes())


def export_tracks(game_dir, key, names, out_dir, log=print, archive="loading"):
    """Decodes named tracks from pc/audio/sfx/<archive>.rpf to <out_dir>/<name>.wav.
    Returns {name: (wav path, seconds)}."""
    path = os.path.join(game_dir, "pc", "audio", "sfx", archive + ".rpf")
    if not os.path.isfile(path):
        log("No %s audio archive; skipping music" % archive)
        return {}
    rpf = Rpf3Archive(path, key)
    done = {}
    try:
        for name in names:
            data = rpf.read(name)
            if data is None:
                log("Music %s not found in %s.rpf" % (name, archive))
                continue
            rate, pcm = decode_stream(data)
            if pcm.shape[1] > 2:
                pcm = pcm[:, :2]
            wav = os.path.join(out_dir, name + ".wav")
            write_wav(wav, rate, pcm)
            done[name] = (wav, len(pcm) / float(rate))
    finally:
        rpf.close()
    return done


# ------------------------------------------------------------------ wave banks
# A bank (e.g. resident.rpf's "weapons"): 0x10 u32 wave count, 0x18 u32 data start; at 0x20 a
# name-hash table (16 bytes per wave), then per wave 32 bytes: u32 ?, u32 name hash, u32 bytes,
# u32 samples, s32 loop, u16 sample rate, s16 headroom, u32 ?, u32 ?. The waves' 16-bit PCM
# follows from the data start in the same order, each on a 0x800 boundary.

def read_bank(data, wanted=None):
    """-> {name hash: (sample rate, int16 samples)}. Weapon banks hold 16-bit PCM, speech banks
    IMA ADPCM (bytes = samples / 2); the per-wave infos vary in size (they're found by their
    loop marker and sample rate) and the data follows from 0x18 in the same order, 0x800-aligned."""
    count = struct.unpack_from("<I", data, 0x10)[0]
    start = struct.unpack_from("<I", data, 0x18)[0]
    infos, o = [], 0x20 + 16 * count
    while len(infos) < count and o + 0x18 < start:
        u, h, nbytes, samples, loop = struct.unpack_from("<IIIIi", data, o)
        rate = struct.unpack_from("<H", data, o + 20)[0]
        if loop == -1 and rate in (8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000) and nbytes and samples:
            infos.append((h, nbytes, samples, rate))
            o += 0x20
        else:
            o += 1
    out = {}
    pos = start
    for h, nbytes, samples, rate in infos:
        pos = (pos + CHUNK - 1) // CHUNK * CHUNK
        if wanted is not None and h not in wanted:
            pos += nbytes
            continue
        raw = data[pos:pos + nbytes]
        if samples > nbytes:  # 4-bit ADPCM
            pcm = _decode_ima(raw)[:samples]
        else:
            pcm = np.frombuffer(raw, "<i2", min(samples, nbytes // 2)).copy()
        out[h] = (rate, pcm)
        pos += nbytes
    return out


# the pistol's sounds in resident.rpf/weapons: exported name -> waves (two waves = left/right)
PISTOL_SOUNDS = {
    "pistol_shot": ("PISTOL_SHOT1_L", "PISTOL_SHOT1_R"),
    "pistol_lows": ("PISTOL_LOWS",),
    "pistol_echo": ("PISTOL_ECHO",),
    "pistol_dry": ("PISTOL_DRY",),
    "handgun_clip_in": ("HANDGUN_INSERT_CLIP_A",),
    "handgun_cock": ("HANDGUN_COCK_A_01",),
    "pistol_impact_1": ("PISTOL_IMPACT_01",),
    "pistol_impact_2": ("PISTOL_IMPACT_05",),
    "pistol_impact_3": ("PISTOL_IMPACT_08",),
}


def export_bank_sounds(game_dir, key, sounds, out_dir, log=print, archive="resident", bank="weapons"):
    """Writes <out_dir>/<name>.wav for each entry of sounds; returns {name: wav path}."""
    path = os.path.join(game_dir, "pc", "audio", "sfx", archive + ".rpf")
    if not os.path.isfile(path):
        return {}
    rpf = Rpf3Archive(path, key)
    try:
        data = rpf.read(bank)
    finally:
        rpf.close()
    if data is None:
        log("Sound bank %s not found" % bank)
        return {}
    waves = read_bank(data)
    done = {}
    for name, parts in sounds.items():
        got = [waves.get(joaat(p)) for p in parts]
        if not all(got):
            log("Sound %s not found in %s" % (name, bank))
            continue
        rate = got[0][0]
        n = min(len(g[1]) for g in got)
        pcm = np.stack([g[1][:n] for g in got], axis=1)
        wav = os.path.join(out_dir, name + ".wav")
        write_wav(wav, rate, pcm)
        done[name] = wav
    return done


# ------------------------------------------------------------------ ped voices (speech.rpf)
# a ped model's voice bank is named after it (M_M_BUSINESS_02); contexts are numbered _01.._NN
VOICE_LINES = {"panic": ("GUN_RUN", 5), "scream": ("SHOCKED", 2), "fight": ("FIGHT", 4)}
PAIN_LINES = {"pain_high": ("PAIN_HIGH", 5), "pain_low": ("PAIN_LOW", 5), "death_high": ("DEATH_HIGH", 2),
              "death_low": ("DEATH_LOW", 2)}
PAIN_BANKS = {"male": ["PAIN_MALE_%02d" % i for i in range(1, 5)], "female": ["PAIN_FEMALE_%02d" % i for i in range(1, 4)]}


def _bank_lines(rpf, bank, lines, out_dir, rel_to):
    e = rpf.entries.get(joaat(bank))
    if e is None:
        return None
    names = {joaat("%s_%02d" % (ctx, i)): (key, "%s_%02d" % (ctx, i))
             for key, (ctx, n) in lines.items() for i in range(1, n + 1)}
    waves = read_bank(rpf.read(bank), set(names))
    out = {k: [] for k in lines}
    for h, (key, name) in sorted(names.items(), key=lambda kv: kv[1][1]):
        if h in waves:
            rate, pcm = waves[h]
            wav = os.path.join(out_dir, name.lower() + ".wav")
            write_wav(wav, rate, pcm[:, None])
            out[key].append(os.path.relpath(wav, rel_to))
    return out


def export_voices(game_dir, key, models, out_dir, rel_to, log=print):
    """Voice lines for the given ped models and the pain/death cries.
    Returns {"voices": {model: {"female", "panic", "scream", "fight"}}, "pain": {"male"|"female": {...}}}."""
    path = os.path.join(game_dir, "pc", "audio", "sfx", "speech.rpf")
    if not os.path.isfile(path):
        return {}
    rpf = Rpf3Archive(path, key)
    result = {"voices": {}, "pain": {}}
    try:
        for model in models:
            got = _bank_lines(rpf, model.upper(), VOICE_LINES, os.path.join(out_dir, model.lower()), rel_to)
            if got and got["panic"] and got["fight"]:
                got["female"] = model.lower().startswith("f_")
                result["voices"][model.lower()] = got
        for sex, banks in PAIN_BANKS.items():
            merged = {k: [] for k in PAIN_LINES}
            for bank in banks:
                got = _bank_lines(rpf, bank, PAIN_LINES, os.path.join(out_dir, "_" + bank.lower()), rel_to)
                for k, v in (got or {}).items():
                    merged[k] += v
            result["pain"][sex] = merged
    finally:
        rpf.close()
    log("Voices: %d ped voices, pain and death cries" % len(result["voices"]))
    return result
