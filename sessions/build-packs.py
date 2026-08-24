#!/usr/bin/env python3
"""Build a couple of extra kits for the Sampler's Save/Open Pack.

Same tiny synth as build-sampler.py's house kit (see drum_synth.py) —
different numbers in, a different-sounding kit out. Each pack is a folder
under sessions/packs/: its own samples/ next to a <name>.pack.json that
names them by relative path, so the pair travels together the same way
jam-sampler.json and sessions/samples/ do.

Open one from the Sampler editor's "Open Pack" button. Rebuild after
tuning a voice here:

    python3 sessions/build-packs.py
"""

from __future__ import annotations

import base64
import json
import sys
from pathlib import Path

here = Path(__file__).resolve().parent
sys.path.insert(0, str(here))

from drum_synth import (  # noqa: E402
    RATE,
    clap,
    cowbell,
    hat,
    kick,
    rim,
    snare,
    tom,
    write_wav,
)
from sampler_pack import encode_pack  # noqa: E402

packs_dir = here / "packs"

# The eight pads every pack leaves silent, so a kit still lands on the
# Sampler's own factory note map — the same as build-sampler.py's own
# eight-plus-eight split — and there is room to Rec your own onto the rest.
EXTRA_PADS = [
    (49, "Crash"),
    (51, "Ride"),
    (47, "Mid Tom"),
    (43, "Low Tom"),
    (45, "High Tom"),
    (40, "E-Snare"),
    (54, "Tambourine"),
    (53, "Ride Bell"),
]


def write_pack(dir_name: str, roles: list[tuple[int, str, list[float], float, float]]) -> None:
    pack_dir = packs_dir / dir_name
    samples_dir = pack_dir / "samples"
    pads = []
    for note, name, mono, volume, pan in roles:
        filename = name.lower().replace(" ", "-") + ".wav"
        write_wav(samples_dir / filename, mono)
        pads.append(
            {"note": note, "name": name, "path": f"samples/{filename}",
             "volume": volume, "pan": pan}
        )
    for note, name in EXTRA_PADS:
        pads.append({"note": note, "name": name, "path": ""})

    blob = encode_pack(pads, rate=float(RATE))
    document = {
        "version": 1,
        "kind": "samplerPack",
        "state": base64.b64encode(blob).decode("ascii"),
    }
    path = pack_dir / f"{dir_name}.pack.json"
    path.write_text(json.dumps(document, indent=2) + "\n")
    print(f"wrote {path} ({path.stat().st_size} bytes, {len(roles)} samples)")


def build_808_trap() -> None:
    """Deep and roomy: a long sub kick, a tight clap-forward snare, trap
    hats front to back."""
    roles = [
        (36, "808 Sub", kick(
            seconds=0.9, start_freq=90.0, base_freq=32.0, freq_decay=0.09,
            body_decay=0.55, click_freq=2400.0, click_decay=0.002,
            click_amt=0.25, drive=1.6,
        ), 1.00, 0.00),
        (38, "Trap Snare", snare(
            seconds=0.18, tone_freq=210.0, tone_decay=0.05, snap_decay=0.05,
            tone_amt=0.5, snap_amt=1.3,
        ), 0.88, 0.04),
        (42, "Tight Hat", hat(0.045, 21, hp=0.9, decay_mul=0.22, peak=0.5),
         0.46, 0.28),
        (46, "Sizzle Hat", hat(0.5, 33, hp=0.88, decay_mul=0.4, peak=0.5),
         0.5, 0.34),
        (39, "Big Clap", clap(
            seconds=0.3, hp=0.55, tail_amt=0.5, peak=0.85,
        ), 0.8, -0.12),
        (37, "Click", rim(
            seconds=0.05, freq1=1100.0, freq2=1900.0, decay=0.008, peak=0.65,
        ), 0.6, 0.18),
        (41, "Sub Tom", tom(
            seconds=0.5, start_freq=70.0, base_freq=38.0, freq_decay=0.1,
            amp_decay=0.3, drive=1.3, peak=0.75,
        ), 0.8, -0.32),
        (56, "Blip", cowbell(
            seconds=0.15, freq1=700.0, freq2=1050.0, decay=0.06, mix=0.6,
            peak=0.5,
        ), 0.4, 0.38),
    ]
    write_pack("808-trap", roles)


def build_techno_clang() -> None:
    """Hard and metallic: a punchy distorted kick, harsh hats, a clang
    where the cowbell usually sits."""
    roles = [
        (36, "Punch Kick", kick(
            seconds=0.35, start_freq=200.0, base_freq=45.0, freq_decay=0.03,
            body_decay=0.15, click_freq=3200.0, click_decay=0.006,
            click_amt=0.6, drive=2.6, peak=0.9,
        ), 1.00, 0.00),
        (38, "Noise Snare", snare(
            seconds=0.2, tone_freq=260.0, tone_decay=0.08, snap_decay=0.1,
            tone_amt=0.4, snap_amt=1.4, noise_hp=0.7, peak=0.85,
        ), 0.9, 0.04),
        (42, "Metal Tick", hat(0.03, 55, hp=0.9, decay_mul=0.15, peak=0.6),
         0.5, 0.28),
        (46, "Long Sizzle", hat(0.35, 77, hp=0.75, decay_mul=0.5, peak=0.6),
         0.54, 0.34),
        (39, "Industrial Clap", clap(
            seconds=0.18, hp=0.7, tail_amt=0.2, peak=0.82,
        ), 0.78, -0.12),
        (37, "Clang", rim(
            seconds=0.09, freq1=1600.0, freq2=2600.0, mix1=0.5, mix2=0.5,
            decay=0.02, peak=0.75,
        ), 0.68, 0.18),
        (41, "Steel Tom", tom(
            seconds=0.3, start_freq=140.0, base_freq=60.0, freq_decay=0.05,
            amp_decay=0.12, drive=1.8, peak=0.85,
        ), 0.85, -0.32),
        (56, "Bell Hit", cowbell(
            seconds=0.25, freq1=920.0, freq2=1380.0, decay=0.13, mix=0.8,
            peak=0.65,
        ), 0.55, 0.38),
    ]
    write_pack("techno-clang", roles)


def main() -> None:
    build_808_trap()
    build_techno_clang()


if __name__ == "__main__":
    main()
