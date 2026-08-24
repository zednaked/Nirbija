#!/usr/bin/env python3
"""Turn a Koala Sampler project's pads into a Nirbija sampler pack.

Reads <project>/sampler/sampler.json and its numbered WAVs and writes a
sessions/packs/<name>/ folder in the same pads-only shape build-packs.py's
synth kits use (sessions/sampler_pack.py) — sixteen pads, each pointing at
a copy of the original WAV plus whatever trim, pitch, pan and volume the
Koala pad already had. Koala's tone/EQ/attack/choke-group knobs have no
Nirbija equivalent and are dropped rather than approximated into something
that would look tuned but isn't.

    python3 sessions/import-koala.py <koala-project-dir> <pack-name>
"""

from __future__ import annotations

import base64
import json
import shutil
import struct
import sys
from pathlib import Path

here = Path(__file__).resolve().parent
sys.path.insert(0, str(here))

from sampler_pack import encode_pack  # noqa: E402

packs_dir = here / "packs"

# Nirbija's own factory pad map, in Koala's 0-15 pad order — the two apps
# share this gesture (sampler.h says as much), so a Koala grid position
# lands on the same MIDI note here rather than an arbitrary one.
NOTE_FOR_PAD = [36, 38, 42, 46, 39, 37, 41, 56, 49, 51, 47, 43, 45, 40, 54, 53]


def wav_frame_count(path: Path) -> int:
    """Read the RIFF chunks directly rather than through Python's `wave`
    module, which refuses IEEE-float WAVs (format code 3) — exactly what
    Koala writes and what libsndfile on the engine side reads without
    complaint."""
    data = path.read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"{path} is not a WAV file")
    channels = 0
    bits = 0
    data_size = 0
    i = 12
    while i + 8 <= len(data):
        chunk_id = data[i:i + 4]
        chunk_size = struct.unpack_from("<I", data, i + 4)[0]
        body = i + 8
        if chunk_id == b"fmt ":
            channels = struct.unpack_from("<H", data, body + 2)[0]
            bits = struct.unpack_from("<H", data, body + 14)[0]
        elif chunk_id == b"data":
            data_size = chunk_size
        i = body + chunk_size + (chunk_size & 1)  # chunks pad to an even size
    if channels == 0 or bits == 0 or data_size == 0:
        raise ValueError(f"{path}: no fmt/data chunk found")
    return data_size // (channels * (bits // 8))


def main() -> None:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <koala-project-dir> <pack-name>",
              file=sys.stderr)
        raise SystemExit(1)

    project = Path(sys.argv[1])
    pack_name = sys.argv[2]
    sampler_dir = project / "sampler"
    manifest = json.loads((sampler_dir / "sampler.json").read_text())

    out_dir = packs_dir / pack_name
    samples_out = out_dir / "samples"
    samples_out.mkdir(parents=True, exist_ok=True)

    pads: list[dict] = [
        {"note": NOTE_FOR_PAD[i], "name": "", "path": ""} for i in range(16)
    ]

    for entry in manifest.get("pads", []):
        index = int(entry["pad"])
        if not (0 <= index < 16):
            continue
        sample_id = entry["sampleId"]
        src = sampler_dir / f"{sample_id}.wav"
        if not src.exists():
            print(f"skipping pad {index}: {src.name} missing", file=sys.stderr)
            continue

        frames = wav_frame_count(src)
        dest_name = f"{sample_id}.wav"
        shutil.copyfile(src, samples_out / dest_name)

        label = str(entry.get("label", "")).strip()
        pads[index] = {
            "note": NOTE_FOR_PAD[index],
            "name": label or f"Pad {index + 1}",
            "path": f"samples/{dest_name}",
            "volume": max(0.0, min(2.0, float(entry.get("vol", 1.0)))),
            "pan": max(-1.0, min(1.0, (float(entry.get("pan", 0.5)) - 0.5) * 2)),
            "pitch": max(-24.0, min(24.0, float(entry.get("pitch", 0.0)))),
            "one_shot": str(entry.get("oneshot", "false")).lower() == "true",
            "start": entry.get("start", 0) / frames if frames else 0.0,
            "end": entry.get("end", frames) / frames if frames else 1.0,
        }

    blob = encode_pack(pads)
    document = {
        "version": 1,
        "kind": "samplerPack",
        "state": base64.b64encode(blob).decode("ascii"),
    }
    path = out_dir / f"{pack_name}.pack.json"
    path.write_text(json.dumps(document, indent=2) + "\n")
    filled = sum(1 for p in pads if p["path"])
    print(f"wrote {path} ({path.stat().st_size} bytes, {filled} samples)")


if __name__ == "__main__":
    main()
