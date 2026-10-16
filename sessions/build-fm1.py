#!/usr/bin/env python3
"""Build jam-fm1.json — the FM-1 strip to try on the device (design/fm1.md).

One strip: a `nirbija.stepseq` playing two of the FM-1's tracks - lane 0 a
bass on MIDI channel 1 (track 1), lane 1 a line on channel 2 (track 2) - into
the `nirbija.fm1` instrument, whose input is the FM-1's USB audio (the strip
finds it by itself). The FM-1 starts with no state of its own here, so the
first thing the session does is read the device's project, never overwrite
it. MIDI clock is on: set the FM-1 to GLO > CLK USB to follow it.

Two empty scenes, A and B, hold the list still: light Record scene, pick a
scene, CAPTURE a sound on a track in the FM-1 editor, and the scene takes it.

    python3 sessions/build-fm1.py
    sessions/run-jam.sh jam-fm1
"""

from __future__ import annotations

import base64
import json
from pathlib import Path

here = Path(__file__).resolve().parent


def b64(text: str) -> str:
    return base64.b64encode(text.encode()).decode("ascii")


def insert(uid: str, state: str | None = None) -> dict:
    out = {"format": "Internal", "uid": uid, "bypassed": False, "postFader": False}
    if state:
        out["state"] = state
    return out


def stepseq_state(*, lanes: dict[int, tuple], hits: list[tuple]) -> str:
    """The stepseq's v2 text state, as build-chord.py writes it.
    `lanes[i] = (note, length, division, direction, channel, mute, gate,
    euclid)`; `hits` = (lane, step, note, vel, active, prob, accent, tie)."""
    lines = [
        "version 2", "swing 0.0000", "direction 0", "scale 0", "root 0",
        "transpose 0", "pattern 0", "next -1", "fill 0", "view 1", "focus 0",
        "macro 1.0000 0.0000 0.0000 1.0000",
    ]
    for i in range(8):
        note, length, division, direction, ch, mute, gate, euclid = lanes.get(
            i, (60, 16, 2, 0, 0, 0, 0.5, 0))
        lines.append(f"lane {i} {note} {length} {division} {direction} {ch} "
                     f"{mute} {gate:.4f} {euclid}")
    for lane, step, note, vel, active, prob, accent, tie in hits:
        lines.append(f"pstep 0 {lane} {step} {note} {vel} {active} {prob:.4f} "
                     f"{accent} {tie} 0.0000 1 0 0")
    return b64("\n".join(lines) + "\n")


# Lane 0, track 1: an acid-ish bass in A, accents on the off-beats.
BASS = [(0, s, n, v, 1, 1.0, a, 0) for s, n, v, a in [
    (0, 33, 100, 0), (2, 45, 84, 1), (3, 33, 90, 0), (6, 43, 88, 1),
    (8, 33, 100, 0), (10, 45, 82, 1), (11, 36, 90, 0), (14, 40, 86, 1),
]]
# Lane 1, track 2: a sparse line above it.
LINE = [(1, s, n, v, 1, 1.0, 0, 0) for s, n, v in [
    (0, 69, 80), (4, 72, 74), (7, 76, 70), (12, 74, 76),
]]
LANES = {
    0: (33, 16, 2, 0, 0, 0, 0.45, 0),  # MIDI channel 1 -> FM-1 track 1
    1: (69, 16, 2, 0, 1, 0, 0.70, 0),  # MIDI channel 2 -> FM-1 track 2
}


def scene(sid: int, name: str, hue: float) -> dict:
    return {"id": sid, "name": name, "hue": hue, "bars": 8, "fade": 1, "targets": []}


session = {
    "version": 1,
    "tempo": 118,
    "metronome": False,
    "midiClock": True,
    "followMidiClock": False,
    "timeNumerator": 4,
    "timeDenominator": 4,
    "midiMaps": [],
    "master": {"gain": 0.85, "sink": ""},
    "channels": [{
        "name": "FM-1",
        "isBus": False,
        "width": 2,
        "gain": 0.9,
        "pan": 0.0,
        "muted": False,
        "soloed": False,
        "armed": False,
        "destination": -1,
        "destinationKind": "master",
        "followScenes": True,
        "audioSource": "",   # the strip wires the FM-1's USB audio itself
        "midiSources": [],
        "inserts": [
            insert("nirbija.stepseq", stepseq_state(lanes=LANES, hits=BASS + LINE)),
            insert("nirbija.fm1"),  # no state: the device's project is read, not replaced
        ],
        "sends": [],
    }],
    "scenes": {
        "enabled": True,
        "auto": False,  # held: you pick the scene, nothing walks while you record
        "items": [scene(1, "A", 0.58), scene(2, "B", 0.92)],
    },
}

out = here / "jam-fm1.json"
out.write_text(json.dumps(session, indent=2) + "\n")
print("wrote", out, f"({out.stat().st_size / 1024:.1f}K)")
