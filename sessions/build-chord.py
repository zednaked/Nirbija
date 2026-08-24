#!/usr/bin/env python3
"""Build jam-chord-demo.json — the chord plugin's demo, `design/chord.md`.

One `nirbija.stepseq` instance drives two of its own lanes into one
`nirbija.chord`: lane 0 plays a I-IV-V-I progression below the split, so the
chord fires by degree; lane 1 plays a melody above the split, with two notes
deliberately outside C major (C#5, F#5) to show the passthrough quantizer
pulling them back onto the scale. Odin2 renders both. No live playing
needed — press Play.

Rebuild after editing the patterns:

    python3 sessions/build-chord.py
"""

from __future__ import annotations

import base64
import json
from pathlib import Path

here = Path(__file__).resolve().parent

ODIN2 = "https://thewavewarden.com/odin2"


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


def send(bus_name: str, level: float) -> dict:
    return {"bus": -1, "busName": bus_name, "level": level}


def stepseq_state(*, lanes: dict[int, tuple], hits: list[tuple]) -> str:
    """v2 text state. `lanes[i] = (note, length, division, direction, channel,
    mute, gate, euclid)`. `hits` = (lane, step, note, vel, active, prob,
    accent, tie) tuples, pattern 0 only — this demo needs no others."""
    lines = [
        "version 2",
        "swing 0.0000",
        "direction 0",
        "scale 0",   # chromatic: the stepseq must not touch the notes itself
        "root 0",    # -- the chord plugin downstream is what reads scale/root
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


def chord_state(*, root=0, scale=0, split=60, octave=0, inversion=0,
                 voices=3, spread=0, passthrough=0, channel=0) -> str:
    lines = [
        f"root {root}", f"scale {scale}", f"split {split}",
        f"octave {octave}", f"inversion {inversion}", f"voices {voices}",
        f"spread {spread}", f"passthrough {passthrough}", f"channel {channel}",
    ]
    return b64("\n".join(lines) + "\n")


# Lane 0 — the progression: I(C) IV(F) V(G) I(C), a beat each, held with tie
# across its four steps so it sustains instead of retriggering every 16th
# (the same idiom jam-lucretia.json's pad lanes use). All below the chord
# plugin's split of 60, degree found by pitch class alone so the octave here
# does not matter to the chord it fires — 48 was picked to sit under the
# melody, nothing more.
def _held(lane: int, start: int, note: int, vel: int) -> list[tuple]:
    return [(lane, start + i, note, vel, 1, 1.0, 0, 1) for i in range(4)]


CHORD_LANE_HITS = (
    _held(0, 0, 48, 90)    # I  — C
    + _held(0, 4, 53, 88)  # IV — F
    + _held(0, 8, 55, 94)  # V  — G
    + _held(0, 12, 48, 90)  # I  — C
)

# Lane 1 — the melody, above the split, unquantized here on purpose: 73 (C#5)
# and 78 (F#5) are not C major, so the chord plugin's own passthrough
# quantizer is what pulls them onto the scale. Two accents mark the halves.
MELODY_LANE_HITS = [
    (1, 0, 72, 88, 1, 1.0, 1, 0),   # C5
    (1, 2, 74, 80, 1, 1.0, 0, 0),   # D5
    (1, 4, 73, 74, 1, 1.0, 0, 0),   # C#5 -> quantized
    (1, 6, 77, 86, 1, 1.0, 0, 0),   # F5
    (1, 8, 79, 90, 1, 1.0, 1, 0),   # G5
    (1, 9, 78, 76, 1, 1.0, 0, 0),   # F#5 -> quantized
    (1, 10, 81, 92, 1, 1.0, 0, 0),  # A5
    (1, 12, 79, 84, 1, 1.0, 0, 0),  # G5
    (1, 14, 77, 80, 1, 1.0, 0, 0),  # F5
    (1, 15, 74, 76, 1, 1.0, 0, 0),  # D5
]

LANES = {
    0: (48, 16, 2, 0, 0, 0, 0.9, 0),
    1: (72, 16, 2, 0, 0, 0, 0.6, 0),
}

session = {
    "version": 1,
    "tempo": 92,
    "metronome": False,
    "midiMaps": [],
    "master": {"gain": 0.88, "sink": ""},
    "channels": [
        channel(
            "Chord Demo",
            gain=0.8,
            inserts=[
                insert(
                    "Internal", "nirbija.stepseq",
                    stepseq_state(lanes=LANES,
                                  hits=CHORD_LANE_HITS + MELODY_LANE_HITS),
                ),
                insert(
                    "Internal", "nirbija.chord",
                    chord_state(root=0, scale=0, split=60, octave=-1,
                                inversion=0, voices=4, spread=0,
                                passthrough=1, channel=0),
                ),
                insert("LV2", ODIN2),
            ],
            sends=[send("Room", 0.3)],
        ),
        channel(
            "Room",
            bus=True,
            gain=0.85,
            inserts=[insert("Internal", "nirbija.fxpad", None)],
        ),
    ],
}

# FX Pad state: one pad (index 4, Reverb) at a fixed amount, off everywhere
# else — same text layout build-sampler.py's fxpad_reverb writes.
_amounts = [0.0] * 16
_amounts[4] = 0.72
session["channels"][1]["inserts"][0]["state"] = b64(
    "0\n" + "".join(f"{v}\n" for v in _amounts)
)

out = here / "jam-chord-demo.json"
out.write_text(json.dumps(session, indent=2) + "\n")
print("wrote", out, f"({out.stat().st_size / 1024:.1f}K)")
