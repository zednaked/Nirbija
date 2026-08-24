"""The Sampler pack file format, shared by every script that writes one.

A pack is SamplerInstance::save_pads()'s own shape: a magic, sixteen pad
records (tuning plus a path or embedded audio), then one fade pair per pad,
trailing and optional the same way Count-in is on the full session blob.
Getting this wrong is a silent corruption, not an import error - worth one
canonical encoder instead of every generator script carrying its own copy.
"""

from __future__ import annotations

import struct

PACK_MAGIC = b"NJSMPK1\n"


def encode_pack(pads: list[dict], *, rate: float = 48000.0) -> bytes:
    """pads: sixteen dicts, note/name/path required, volume/pan/pitch/
    one_shot/start/end/fade_in/fade_out optional (see the defaults below)."""
    out = bytearray(PACK_MAGIC)
    out += struct.pack("<i", len(pads))
    for p in pads:
        name_b = p.get("name", "").encode()[:31]
        path_b = p.get("path", "").encode()
        out += struct.pack(
            "<ii3f2dQdI",
            p["note"],
            1 if p.get("one_shot", True) else 0,
            p.get("volume", 1.0),
            p.get("pan", 0.0),
            p.get("pitch", 0.0),
            p.get("start", 0.0),
            p.get("end", 1.0),
            0,  # frames: the file is the audio
            rate,
            len(name_b),
        )
        out += name_b
        out += struct.pack("<I", len(path_b))
        out += path_b
    for p in pads:
        out += struct.pack("<dd", p.get("fade_in", 0.0), p.get("fade_out", 0.0))
    return bytes(out)
