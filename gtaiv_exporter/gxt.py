"""GTA IV text (common/text/american.gxt and the other languages).

header: u16 version (4), u16 char bits (16), "TABL", u32 size, then (name[8], u32 offset) per table
table at offset: name[8] (not for MAIN), "TKEY", u32 size, (u32 data offset, u32 key hash)...,
                 "TDAT", u32 size, UTF-16 strings
Keys are Jenkins one-at-a-time hashes of the key name (as the game's scripts use it).
"""

import struct

from .mapdata import joaat


def load_gxt(data):
    """{table name: {key hash: text}}."""
    assert data[4:8] == b"TABL", "not a GTA IV .gxt"
    size = struct.unpack_from("<I", data, 8)[0]
    tables = {}
    for o in range(12, 12 + size, 12):
        name = data[o:o + 8].split(b"\0")[0].decode("latin-1")
        off = struct.unpack_from("<I", data, o + 8)[0]
        if name != "MAIN":
            off += 8                       # mission tables repeat their name first
        assert data[off:off + 4] == b"TKEY", (name, data[off:off + 4])
        ksize = struct.unpack_from("<I", data, off + 4)[0]
        keys = [struct.unpack_from("<II", data, off + 8 + 8 * i) for i in range(ksize // 8)]
        tdat = off + 8 + ksize
        assert data[tdat:tdat + 4] == b"TDAT"
        base = tdat + 8
        out = {}
        for doff, h in keys:
            p = base + doff
            end = p
            while data[end:end + 2] != b"\0\0":
                end += 2
            out[h] = data[p:end].decode("utf-16-le", "replace")
        tables[name] = out
    return tables


class Text:
    """Lookup of GXT strings by key name (MAIN table first, then the others)."""

    def __init__(self, data):
        self.tables = load_gxt(data)

    def get(self, key, table=None):
        h = joaat(key.lower())
        for name in ([table] if table else []) + ["MAIN"] + list(self.tables):
            t = self.tables.get(name)
            if t and h in t:
                return t[h]
        h = joaat(key)
        for t in self.tables.values():
            if h in t:
                return t[h]
        return None
