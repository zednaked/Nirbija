#!/usr/bin/env python3
"""Build jam-drone-seq.json — a step sequencer re-rooting the Drone.

One `nirbija.stepseq` above one `nirbija.drone` in the same strip. The
sequencer's lane 0 walks four roots, sixteen 1/16 steps each at 60 BPM - four
seconds a root, sixteen seconds round - and every note it sends becomes the
drone's new root, which the six strings glide to over about a second. No
plugin outside the host is needed; press Play.

    D2 ─── A1 ─── G1 ─── C2 ─── (round again)

Rebuild after editing:

    python3 sessions/build-drone-seq.py
"""

from __future__ import annotations

import base64
import json
from pathlib import Path

here = Path(__file__).resolve().parent


def b64(text: str) -> str:
    return base64.b64encode(text.encode()).decode("ascii")


def insert(fmt: str, uid: str, state: str | None = None) -> dict:
    out = {"format": fmt, "uid": uid, "bypassed": False, "postFader": False}
    if state:
        out["state"] = state
    return out


def channel(name, *, bus=False, gain, pan=0.0, inserts, sends=None) -> dict:
    entry = {
        "name": name,
        "isBus": bus,
        "width": 2,
        "gain": gain,
        "pan": pan,
        "muted": False,
        "soloed": False,
        "armed": False,
        "destination": -1,
        "destinationKind": "master",
        "inserts": inserts,
        "sends": sends or [],
    }
    if not bus:
        entry["audioSource"] = ""
        entry["midiSources"] = []
    return entry


def stepseq_state(*, lanes: dict[int, tuple], hits: list[tuple]) -> str:
    """v2 text state, same layout build-chord.py writes. `lanes[i] = (note,
    length, division, direction, channel, mute, gate, euclid)`; `hits` =
    (lane, step, note, vel, active, prob, accent, tie), pattern 0 only."""
    lines = [
        "version 2",
        "swing 0.0000",
        "direction 0",
        "scale 0",
        "root 0",
        "transpose 0",
        "pattern 0",
        "next -1",
        "fill 0",
        "view 1",
        "focus 0",
        "macro 1.0000 0.0000 0.0000 1.0000",
    ]
    for i in range(8):
        note, length, division, direction, ch, mute, gate, euclid = lanes.get(
            i, (60, 16, 2, 0, 0, 0, 0.5, 0))
        lines.append(
            f"lane {i} {note} {length} {division} {direction} {ch} {mute} "
            f"{gate:.4f} {euclid}"
        )
    for lane, step, note, vel, active, prob, accent, tie in hits:
        lines.append(
            f"pstep 0 {lane} {step} {note} {vel} {active} {prob:.4f} "
            f"{accent} {tie} 0.0000 1 0 0"
        )
    return b64("\n".join(lines) + "\n")


def drone_state(**overrides: float) -> str:
    """Only the parameters that differ from the factory defaults: a missing
    id keeps its default on load, so the file stays short and a default that
    improves later still reaches this session."""
    ids = {
        "swell": 24, "rise": 25, "root": 26, "just": 27, "drift": 28,
        "tide": 29, "cutoff": 30, "resonance": 31, "motion": 32,
        "space": 33, "grit": 34, "width": 35, "glide": 36,
    }
    lines = ["drone 1"] + [f"{ids[k]} {v:.6f}" for k, v in overrides.items()]
    return b64("\n".join(lines) + "\n")


# Lane 0: four roots, each held across its sixteen steps with `tie` so the
# sequencer sends one note-on per root rather than sixteen. The drone would
# not mind sixteen - a note-on for the root it already has changes nothing -
# but a tied note is what the pattern means.
def _held(start: int, note: int) -> list[tuple]:
    return [(0, start + i, note, 100, 1, 1.0, 0, 1) for i in range(16)]


ROOTS = [38, 33, 31, 36]  # D2  A1  G1  C2
HITS = sum((_held(16 * i, note) for i, note in enumerate(ROOTS)), [])

LANES = {
    0: (38, 64, 2, 0, 0, 0, 0.95, 0),  # 64 steps of 1/16, one lane
}

session = {
    "version": 1,
    "tempo": 60,
    "metronome": False,
    "midiMaps": [],
    "master": {"gain": 0.8, "sink": "", "limiter": True},
    "channels": [
        channel(
            "Drone",
            gain=0.8,
            inserts=[
                insert("Internal", "nirbija.stepseq",
                       stepseq_state(lanes=LANES, hits=HITS)),
                # A second of glide so each root arrives as a slide; the room
                # up so the old root hangs in the air while the new one comes.
                insert("Internal", "nirbija.drone",
                       drone_state(glide=0.6, rise=6.0, space=0.8, drift=0.25)),
            ],
        ),
    ],
}

out = here / "jam-drone-seq.json"
out.write_text(json.dumps(session, indent=2) + "\n")
print("wrote", out, f"({out.stat().st_size / 1024:.1f}K)")
