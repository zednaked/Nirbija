#!/usr/bin/env python3
"""Build jam-emissaries.json — Berlin-school jam after Radio Massacre
International's *Emissaries* (2005): a pair of interlocking analog-style
step sequences (the MAQ 16/3 trick — different lane lengths at the same
division, so the two drift in and out of phase over several bars) plus
sparse hand percussion and two live-played voices for pad/choir and
theremin-style lead.

calf, dragonfly-reverb-lv2 and padthv1-lv2 were installed (pacman, official
`extra` repo) specifically for this session — padthv1 is the closest free
plugin to a Mellotron (additive, string/choir presets), Dragonfly Room is
the spacious hall RMI's mixes live in, and Calf's Vintage Delay stands in
for the Roland SH-3A repeater / tape echo in their credits.
"""

from __future__ import annotations

import base64
import json
from pathlib import Path

here = Path(__file__).resolve().parent

ODIN2 = "https://thewavewarden.com/odin2"
BLACK_PEARL = "http://gareus.org/oss/lv2/avldrums#BlackPearl"
PADTHV1 = "http://padthv1.sourceforge.net/lv2"
DFLY_ROOM = "urn:dragonfly:room"
CALF_CHORUS = "http://calf.sourceforge.net/plugins/MultiChorus"
CALF_PHASER = "http://calf.sourceforge.net/plugins/Phaser"
CALF_DELAY = "http://calf.sourceforge.net/plugins/VintageDelay"
CALF_TAPE = "http://calf.sourceforge.net/plugins/TapeSimulator"


def b64(text: str) -> str:
    return base64.b64encode(text.encode()).decode("ascii")


def stepseq_v1(*, division: int, length: int, gate: float, transpose: int,
               channel: int, steps: list[tuple[int, int, int]]) -> str:
    """v1 text state: one nirbija.stepseq lane, `steps` = (note, velocity, on)."""
    assert len(steps) == length, f"expected {length} steps, got {len(steps)}"
    lines = [
        f"division {division}",
        f"length {length}",
        f"gate {gate:.4f}",
        f"transpose {transpose}",
        f"channel {channel}",
    ]
    for note, vel, on in steps:
        lines.append(f"step {note} {vel} {on}")
    return b64("\n".join(lines) + "\n")


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


# D dorian: D E F G A B C. MIDI D2=38 A2=45 D3=50 E3=52 F3=53 G3=55 A3=57 C3=48

# 16-step low pulse, MemoryMoog-ish — the steady MAQ 16/3 driving voice.
PULSE = [
    (38, 118, 1), (45, 78, 1), (50, 92, 1), (53, 70, 1),
    (38, 100, 1), (45, 74, 1), (50, 88, 1), (52, 66, 1),
    (38, 118, 1), (45, 78, 1), (50, 92, 1), (53, 70, 1),
    (38, 100, 1), (45, 74, 1), (48, 84, 1), (45, 66, 1),
]

# 18-step higher weave, same 1/16 division as PULSE — 16 vs 18 means the two
# lanes realign only every LCM(16,18)/16 = 9 bars, so the interplay keeps
# slowly changing shape without either sequence itself ever varying.
WEAVE = [
    (50, 96, 1), (53, 68, 1), (57, 104, 1), (50, 82, 1), (53, 64, 1), (55, 92, 1),
    (50, 90, 1), (53, 72, 1), (57, 110, 1), (50, 78, 1), (53, 68, 1), (55, 98, 1),
    (50, 100, 1), (53, 66, 1), (57, 106, 1), (50, 84, 1), (53, 62, 1), (55, 94, 1),
]

# 16-step hand percussion, mostly silent — rim on the off-beats, one soft
# cowbell accent, a single ghost hat. Not a drum-machine groove.
MALLETS = [
    (60, 100, 0), (60, 100, 0), (37, 70, 1), (60, 100, 0),
    (60, 100, 0), (60, 100, 0), (37, 76, 1), (60, 100, 0),
    (56, 60, 1), (60, 100, 0), (37, 70, 1), (60, 100, 0),
    (60, 100, 0), (42, 40, 1), (37, 82, 1), (60, 100, 0),
]

session = {
    "version": 1,
    "tempo": 78,
    "metronome": False,
    "midiMaps": [],
    "master": {"gain": 0.85, "sink": ""},
    "channels": [
        channel(
            "Pulse",
            gain=0.55,
            pan=-0.15,
            inserts=[
                insert(
                    "Internal", "nirbija.stepseq",
                    stepseq_v1(division=2, length=16, gate=0.35, transpose=0,
                               channel=0, steps=PULSE),
                ),
                insert("LV2", ODIN2),
            ],
            sends=[send("Room", 0.22), send("Tape", 0.28)],
        ),
        channel(
            "Weave",
            gain=0.42,
            pan=0.18,
            inserts=[
                insert(
                    "Internal", "nirbija.stepseq",
                    stepseq_v1(division=2, length=18, gate=0.4, transpose=0,
                               channel=0, steps=WEAVE),
                ),
                insert("LV2", ODIN2),
            ],
            sends=[send("Room", 0.3), send("Tape", 0.32)],
        ),
        channel(
            "Mallets",
            gain=0.3,
            inserts=[
                insert(
                    "Internal", "nirbija.stepseq",
                    stepseq_v1(division=2, length=16, gate=0.15, transpose=0,
                               channel=0, steps=MALLETS),
                ),
                insert("LV2", BLACK_PEARL),
            ],
            sends=[send("Room", 0.5), send("Tape", 0.15)],
        ),
        channel(
            "Mellotron",
            gain=0.5,
            inserts=[insert("LV2", PADTHV1), insert("LV2", CALF_CHORUS)],
            sends=[send("Room", 0.75)],
        ),
        channel(
            "Theremin",
            gain=0.45,
            inserts=[insert("LV2", ODIN2), insert("LV2", CALF_PHASER)],
            sends=[send("Room", 0.4), send("Tape", 0.35)],
        ),
        channel(
            "Room",
            bus=True,
            gain=0.88,
            inserts=[insert("LV2", DFLY_ROOM)],
        ),
        channel(
            "Tape",
            bus=True,
            gain=0.8,
            inserts=[insert("LV2", CALF_DELAY), insert("LV2", CALF_TAPE)],
        ),
    ],
}

out = here / "jam-emissaries.json"
out.write_text(json.dumps(session, indent=2) + "\n")
print("wrote", out, f"({out.stat().st_size / 1024:.1f}K)")
