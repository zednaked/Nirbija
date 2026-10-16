#!/usr/bin/env python3
"""Build jam-fm1-full.json — the whole FM-1 strip to try on the device.

Four step sequencers stacked above the `nirbija.fm1` instrument, one per FM-1
track, each with every lane on its track's channel (design/fm1.md):

  T1      ch 1   a bass in A
  T2      ch 2   a sparse line above it
  T3      ch 3   a held two-note pad
  DRUMS   ch 10  kick, snare, closed and open hat, clap - the lanes tuned to
                 the kit as the header's DRUMS button tunes them

The FM-1 starts with no state of its own, so the session reads the device's
project first and never overwrites it. MIDI clock is on (set the FM-1 to
GLO > CLK USB to follow). Two empty scenes, A and B, held: light Record
scene, pick one, CAPTURE a sound in the FM-1 editor, and the scene takes it.

    python3 sessions/build-fm1-full.py
    sessions/run-jam.sh jam-fm1-full
"""

from __future__ import annotations

import base64
import json
from pathlib import Path

here = Path(__file__).resolve().parent

DRUM_NOTES = [36, 38, 42, 46, 39, 45, 49, 51]  # Fm1Instance::kDrumLaneNotes


def b64(text: str) -> str:
    return base64.b64encode(text.encode()).decode("ascii")


def insert(uid: str, state: str | None = None) -> dict:
    out = {"format": "Internal", "uid": uid, "bypassed": False, "postFader": False}
    if state:
        out["state"] = state
    return out


def stepseq(channel: int, lane_notes: list[int], hits: list[tuple], gate: float = 0.5) -> str:
    """Every lane on `channel` (0-based). `hits` = (lane, step, note, vel,
    accent, tie); 255 plays the lane's own note."""
    lines = ["version 2", "swing 0.0000", "direction 0", "scale 0", "root 0",
             "transpose 0", "pattern 0", "next -1", "fill 0", "view 1", "focus 0",
             "macro 1.0000 0.0000 0.0000 1.0000"]
    for lane in range(8):
        note = lane_notes[lane] if lane < len(lane_notes) else 60
        lines.append(f"lane {lane} {note} 16 2 0 {channel} 0 {gate:.4f} 0")
    for lane, step, note, vel, accent, tie in hits:
        lines.append(f"pstep 0 {lane} {step} {note} {vel} 1 1.0000 {accent} {tie} 0.0000 1 0 0")
    return b64("\n".join(lines) + "\n")


BASS = [(0, s, n, v, a, 0) for s, n, v, a in [
    (0, 33, 100, 0), (2, 45, 84, 1), (3, 33, 90, 0), (6, 43, 88, 1),
    (8, 33, 100, 0), (10, 45, 82, 1), (11, 36, 90, 0), (14, 40, 86, 1)]]
LINE = [(0, s, n, v, 0, 0) for s, n, v in [(0, 69, 80), (4, 72, 74), (7, 76, 70), (12, 74, 76)]]
# The pad: two lanes held a whole bar each half (tie), A minor then F.
PAD = ([(0, s, 57, 70, 0, 1) for s in range(8)] + [(1, s, 64, 64, 0, 1) for s in range(8)] +
       [(0, s, 53, 70, 0, 1) for s in range(8, 16)] + [(1, s, 60, 64, 0, 1) for s in range(8, 16)])
DRUMS = ([(0, s, 255, 110, 0, 0) for s in (0, 4, 8, 10, 12)] +             # kick
         [(1, s, 255, 100, 1, 0) for s in (4, 12)] +                       # snare
         [(2, s, 255, 70 + (s % 4 == 0) * 20, 0, 0) for s in range(0, 16, 2)] +  # closed hat
         [(3, 14, 255, 80, 0, 0)] +                                        # open hat
         [(4, 12, 255, 90, 0, 0)])                                         # clap


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
        "audioSource": "",   # the strip finds the FM-1's USB audio itself
        "midiSources": [],
        "inserts": [
            insert("nirbija.stepseq", stepseq(0, [33], BASS, 0.45)),
            insert("nirbija.stepseq", stepseq(1, [69], LINE, 0.7)),
            insert("nirbija.stepseq", stepseq(2, [57, 64], PAD, 0.95)),
            insert("nirbija.stepseq", stepseq(9, DRUM_NOTES, DRUMS, 0.3)),
            insert("nirbija.fm1"),  # no state: the device's project is read, not replaced
        ],
        "sends": [],
    }],
    "scenes": {
        "enabled": True,
        "auto": False,
        "items": [scene(1, "A", 0.58), scene(2, "B", 0.92)],
    },
}

out = here / "jam-fm1-full.json"
out.write_text(json.dumps(session, indent=2) + "\n")
print("wrote", out, f"({out.stat().st_size / 1024:.1f}K)")
