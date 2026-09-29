#!/usr/bin/env python3
"""Build jam-showcase.json — one song that uses everything the mixer does.

A-minor, 118 BPM, told in seven scenes along the ribbon: Dawn, Pulse, Groove,
Break, Roll, Peak and Fade out. Press Play and the queue walks the song by
itself; Alt+1..7 jumps; Hold (Alt+H) stays in a part as long as you like.

    Drone   nirbija.drone, glide and space. Ignores scenes (dashed stripe):
            the bed under the whole song, yours to play.
    Drums   nirbija.stepseq (four patterns: pulse, groove, half-time,
            ratchet roll) -> nirbija.sampler on the 808 Trap pack. Plays into
            the Loop strip below it rather than the master.
    Loop    nirbija.looper, empty, count-in on -> nirbija.fxpad. Press Rec
            during Groove to catch a bar of the drums; the FX pad is there to
            bend it.
    Chords  nirbija.stepseq (held chords, then stabs) -> nirbija.chord, one
            key per chord (Am F C G, by degree in C major) -> Odin2 (LV2).
    Arp     nirbija.stepseq holding the triads -> nirbija.arp (up-down, two
            octaves, latch) -> nirbija.script (Lua: a softer touch, and
            nothing above C6) -> Surge XT (CLAP).
    Keys    nirbija.keyboard -> Odin2: type on the computer keyboard and
            play over it (A W S E D F T G Y H U J K). Follows scenes: off
            until Break, where the song leaves room for it.
    Room    bus, nirbija.fxpad reverb. Hall: bus, Dragonfly Hall (LV2).

The scenes switch strips on and off with a fade, walk faders in decibels
and change the sequencers' patterns on the bar line. Nothing outside the
host is needed but Odin2, Surge XT and Dragonfly; a strip whose plugin is
missing still loads, named on screen, and the song still plays.

Rebuild after editing:

    python3 sessions/build-showcase.py
    sessions/run-jam.sh jam-showcase
"""

from __future__ import annotations

import base64
import json
import struct
from pathlib import Path

here = Path(__file__).resolve().parent

ODIN2 = ("LV2", "https://thewavewarden.com/odin2")
SURGE = ("CLAP", "org.surge-synth-team.surge-xt")
HALL = ("LV2", "https://github.com/michaelwillis/dragonfly-reverb")

UNLOCKED = 255  # a step that plays its lane's own note
BARS = 4        # every pattern here is four bars of sixteenths
STEPS = 16 * BARS


def b64(raw: bytes | str) -> str:
    if isinstance(raw, str):
        raw = raw.encode()
    return base64.b64encode(raw).decode("ascii")


def insert(fmt: str, uid: str, state: str | None = None) -> dict:
    out = {"format": fmt, "uid": uid, "bypassed": False, "postFader": False}
    if state:
        out["state"] = state
    return out


# Fixed so the scenes can name the strips; any 16 hex digits would do.
UIDS = {
    "Drone": "5ce0000000000001",
    "Drums": "5ce0000000000002",
    "Loop": "5ce0000000000003",
    "Chords": "5ce0000000000004",
    "Arp": "5ce0000000000005",
    "Keys": "5ce0000000000006",
    "Room": "5ce0000000000007",
    "Hall": "5ce0000000000008",
}


def channel(name, *, bus=False, gain, pan=0.0, inserts, sends=None,
            follow=True, on=True, into=None) -> dict:
    entry = {
        "name": name,
        "uid": UIDS[name],
        "isBus": bus,
        "width": 2,
        "gain": gain,
        "pan": pan,
        "muted": False,
        "soloed": False,
        "armed": False,
        "followScenes": follow,
        "sceneOn": on,
        "destination": -1,
        "destinationKind": "master",
        "inserts": inserts,
        "sends": sends or [],
    }
    if into is not None:
        # Straight into a later channel instead of the master: how the drums
        # reach the looper that records them.
        entry["destinationKind"] = "channel"
        entry["destinationName"] = into
    if not bus:
        entry["audioSource"] = ""
        entry["midiSources"] = []
    return entry


def send(bus_name: str, level: float) -> dict:
    return {"bus": -1, "busName": bus_name, "level": level}


# --- step sequencer ---------------------------------------------------------

def stepseq(lanes: dict[int, tuple], patterns: list[list[tuple]]) -> str:
    """v2 text state. `lanes[i] = (note, length, gate)`, sixteenths.
    `patterns[p]` = (lane, step, note, vel, accent, tie, ratchet, prob)."""
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
        "macro 1.0000 0.0000 1.0000 1.0000",
    ]
    for i in range(8):
        note, length, gate = lanes.get(i, (60, STEPS, 0.5))
        lines.append(f"lane {i} {note} {length} 2 0 0 0 {gate:.4f} 0")
    for p, hits in enumerate(patterns):
        for lane, step, note, vel, accent, tie, ratchet, prob in hits:
            lines.append(
                f"pstep {p} {lane} {step} {note} {vel} 1 {prob:.4f} "
                f"{1 if accent else 0} {1 if tie else 0} 0.0000 {ratchet} 0 0"
            )
    return b64("\n".join(lines) + "\n")


def hit(lane, step, vel, *, note=UNLOCKED, accent=False, tie=False,
        ratchet=1, prob=1.0):
    return (lane, step, note, vel, accent, tie, ratchet, prob)


# --- drums: 808 Trap, borrowed in place from sessions/packs/ ------------------

SMP_MAGIC = b"NJSMP02\n"
RATE = 44100
PADS = [
    (36, "808 Sub", "808-sub.wav"),
    (38, "Trap Snare", "trap-snare.wav"),
    (42, "Tight Hat", "tight-hat.wav"),
    (46, "Sizzle Hat", "sizzle-hat.wav"),
    (39, "Big Clap", "big-clap.wav"),
    (37, "Click", "click.wav"),
    (41, "Sub Tom", "sub-tom.wav"),
    (56, "Blip", "blip.wav"),
    (49, "Crash", None), (51, "Ride", None), (47, "Mid Tom", None),
    (43, "Low Tom", None), (45, "High Tom", None), (40, "E-Snare", None),
    (54, "Tambourine", None), (53, "Ride Bell", None),
]
KICK, SNARE, HAT, OPEN, CLAP, CLICK, TOM, BLIP = range(8)


def sampler_blob() -> str:
    out = bytearray(SMP_MAGIC)
    out += struct.pack("<fiii", 0.8, 0, 0, 16)
    for note, name, filename in PADS:
        rel = f"packs/808-trap/samples/{filename}" if filename else ""
        name_b = name.encode()[:31]
        path_b = rel.encode()
        out += struct.pack("<ii3f2dQdI", note, 1, 1.0, 0.0, 0.0, 0.0, 1.0,
                           0, float(RATE), len(name_b))
        out += name_b
        out += struct.pack("<I", len(path_b))
        out += path_b
    out += struct.pack("<i", 1)  # count-in on
    return b64(bytes(out))


def every_bar(fn):
    return [h for b in range(BARS) for h in fn(16 * b, b)]


def drums_pulse(o, b):  # scene Pulse: kick on the beat, a hat between
    hits = [hit(KICK, o + s, 104, accent=s == 0) for s in (0, 8)]
    hits += [hit(HAT, o + s, 58) for s in (4, 12)]
    if b == BARS - 1:
        hits.append(hit(OPEN, o + 14, 60))
    return hits


def drums_groove(o, b):  # scene Groove and Peak
    hits = [hit(KICK, o + s, 118, accent=s == 0) for s in (0, 7, 10)]
    hits += [hit(SNARE, o + s, 112, accent=True) for s in (4, 12)]
    hits += [hit(HAT, o + s, 76 if s % 4 == 0 else 54) for s in range(0, 16, 2)]
    hits += [hit(HAT, o + 15, 44, ratchet=2, prob=0.6)]
    hits += [hit(CLAP, o + 12, 84)]
    hits += [hit(CLICK, o + 3, 38, prob=0.7), hit(CLICK, o + 11, 34, prob=0.5)]
    if b % 2 == 1:
        hits.append(hit(OPEN, o + 14, 70))
    return hits


def drums_halftime(o, b):  # scene Break: the floor drops out
    hits = [hit(KICK, o, 110, accent=True), hit(SNARE, o + 8, 96)]
    hits += [hit(HAT, o + s, 46) for s in range(2, 16, 4)]
    if b == BARS - 1:
        hits += [hit(TOM, o + 12, 84), hit(TOM, o + 14, 90)]
    return hits


def drums_roll(o, b):  # scene Roll: one bar, a snare roll built from ratchets
    if b > 0:
        return []
    hits = [hit(KICK, 0, 118, accent=True), hit(BLIP, 0, 90)]
    for step, ratchet, vel in zip(range(4, 16, 2), (2, 2, 3, 4, 6, 8),
                                  (64, 72, 82, 94, 106, 122)):
        hits.append(hit(SNARE, step, vel, accent=vel > 100, ratchet=ratchet))
    return hits


DRUM_LANES = {lane: (PADS[lane][0], STEPS, 0.5) for lane in range(8)}
DRUM_PATTERNS = [every_bar(drums_pulse), every_bar(drums_groove),
                 every_bar(drums_halftime), every_bar(drums_roll)]


# --- chords: one key per chord, the chord plugin builds it -------------------

PROGRESSION = [57, 53, 48, 55]  # A F C G: vi IV I V in C major, below the split


def chords_held(o, b):
    return [hit(0, o + s, 84, tie=True, note=PROGRESSION[b]) for s in range(16)]


def chords_stabs(o, b):
    return [hit(0, o + s, 96 if s == 0 else 78, accent=s == 0, note=PROGRESSION[b])
            for s in (0, 3, 6, 10, 12)]


CHORD_LANES = {0: (57, STEPS, 0.8)}
CHORD_PATTERNS = [every_bar(chords_held), every_bar(chords_stabs)]


def chord_state() -> str:
    lines = ["root 0", "scale 0", "split 60", "octave 0", "inversion 1",
             "voices 4", "spread 1", "passthrough 1", "channel 0"]
    return b64("\n".join(lines) + "\n")


# --- arp: the triads held, the arpeggiator walks them ------------------------

TRIADS = [(57, 60, 64), (53, 57, 60), (48, 52, 55), (55, 59, 62)]


def arp_held(o, b):
    return [hit(lane, o + s, 90, tie=True, note=TRIADS[b][lane])
            for lane in range(3) for s in range(16)]


ARP_LANES = {lane: (TRIADS[0][lane], STEPS, 0.95) for lane in range(3)}


def arp_state() -> str:
    # mode 2 up-down, two octaves, sixteenths, latch so a tie never gaps it.
    return b64("division 2\nmode 2\noctaves 2\ngate 0.4500\n"
               "transpose 12\nlatch 1\nthru 0\n")


SCRIPT = """-- The arpeggio's touch. Knob 1 softens every velocity towards a
-- steady middle; nothing above C6 gets through, so the second octave of
-- the arp never turns shrill.
function build(knob)
  local soften = knob[1]
  local velocity_map, note_map = {}, {}
  for v = 0, 127 do
    if v == 0 then velocity_map[v] = 0
    else velocity_map[v] = math.floor(v + (72 - v) * soften + 0.5) end
  end
  for n = 0, 127 do
    note_map[n] = n <= 84 and n or -1
  end
  return { velocity_map = velocity_map, note_map = note_map }
end
"""


def script_state() -> str:
    knobs = [0.6, 0.0, 0.0, 0.0]
    text = "".join(f"knob {i} {v:.6f}\n" for i, v in enumerate(knobs))
    return b64(text + "script\n" + SCRIPT)


# --- the rest -----------------------------------------------------------------

def drone_state(**overrides: float) -> str:
    ids = {"swell": 24, "rise": 25, "root": 26, "just": 27, "drift": 28,
           "tide": 29, "cutoff": 30, "resonance": 31, "motion": 32,
           "space": 33, "grit": 34, "width": 35, "glide": 36}
    lines = ["drone 1"] + [f"{ids[k]} {v:.6f}" for k, v in overrides.items()]
    return b64("\n".join(lines) + "\n")


def fxpad(amounts_by_pad: dict[int, float]) -> str:
    amounts = [0.0] * 16
    for pad, amount in amounts_by_pad.items():
        amounts[pad] = amount
    return b64("0\n" + "".join(f"{v}\n" for v in amounts))


# --- scenes -------------------------------------------------------------------

def gate(strip, on):
    return {"strip": UIDS[strip], "what": "gate", "value": 1.0 if on else 0.0}


def level(strip, db):
    return {"strip": UIDS[strip], "what": "level",
            "value": 0.0 if db is None else round(10 ** (db / 20.0), 6)}


def pattern(strip, index):
    # The sequencer is the first plugin on both strips that change pattern.
    return {"strip": UIDS[strip], "what": "pattern", "insert": 0,
            "value": float(index)}


HUES = [0.58, 0.08, 0.83, 0.47, 0.00, 0.30, 0.72]

SCENES = [
    ("Dawn", 8, 0, [
        gate("Drums", False), gate("Arp", False), gate("Keys", False),
        gate("Chords", True), level("Chords", -15), pattern("Chords", 0),
        gate("Loop", True),
    ]),
    ("Pulse", 8, 2, [
        gate("Drums", True), level("Drums", -12), pattern("Drums", 0),
        gate("Arp", True), level("Arp", -16),
    ]),
    ("Groove", 16, 1, [
        level("Drums", -10), pattern("Drums", 1),
        level("Chords", -14), pattern("Chords", 1),
        level("Arp", -10),
    ]),
    ("Break", 8, 4, [
        level("Drums", -14), pattern("Drums", 2),
        level("Chords", -10), pattern("Chords", 0),
        gate("Arp", False), gate("Keys", True),
    ]),
    ("Roll", 1, 0, [
        level("Drums", -7), pattern("Drums", 3), gate("Keys", False),
    ]),
    ("Peak", 16, 1, [
        level("Drums", -9), pattern("Drums", 1),
        level("Chords", -13), pattern("Chords", 1),
        gate("Arp", True), level("Arp", -8),
    ]),
    ("Fade out", 0, 8, [
        gate("Drums", False), gate("Chords", False), gate("Arp", False),
        gate("Loop", False),
    ]),
]


def scenes() -> dict:
    return {
        "auto": True,
        "items": [
            {"name": name, "hue": HUES[i % len(HUES)], "bars": bars,
             "fade": fade, "targets": targets}
            for i, (name, bars, fade, targets) in enumerate(SCENES)
        ],
    }


def main() -> None:
    session = {
        "version": 1,
        "tempo": 118,
        "metronome": False,
        "timeNumerator": 4,
        "timeDenominator": 4,
        "midiMaps": [],
        "master": {"gain": 0.85, "sink": "", "limiter": True},
        "channels": [
            channel("Drone", gain=0.35, follow=False, inserts=[
                insert("Internal", "nirbija.drone",
                       drone_state(root=45.0, glide=0.5, space=0.7,
                                   drift=0.2, swell=0.55)),
            ], sends=[send("Hall", 0.35)]),
            channel("Drums", gain=0.5, into="Loop", on=False, inserts=[
                insert("Internal", "nirbija.stepseq",
                       stepseq(DRUM_LANES, DRUM_PATTERNS)),
                insert("Internal", "nirbija.sampler", sampler_blob()),
            ], sends=[send("Room", 0.18)]),
            channel("Loop", gain=0.9, inserts=[
                insert("Internal", "nirbija.looper"),
                insert("Internal", "nirbija.fxpad", fxpad({})),
            ]),
            channel("Chords", gain=0.18, pan=-0.15, inserts=[
                insert("Internal", "nirbija.stepseq",
                       stepseq(CHORD_LANES, CHORD_PATTERNS)),
                insert("Internal", "nirbija.chord", chord_state()),
                insert(*ODIN2),
            ], sends=[send("Hall", 0.3)]),
            channel("Arp", gain=0.16, pan=0.2, on=False, inserts=[
                insert("Internal", "nirbija.stepseq",
                       stepseq(ARP_LANES, [every_bar(arp_held)])),
                insert("Internal", "nirbija.arp", arp_state()),
                insert("Internal", "nirbija.script", script_state()),
                insert(*SURGE),
            ], sends=[send("Room", 0.25), send("Hall", 0.2)]),
            channel("Keys", gain=0.5, on=False, inserts=[
                insert("Internal", "nirbija.keyboard"),
                insert(*ODIN2),
            ], sends=[send("Hall", 0.4)]),
            channel("Room", bus=True, gain=0.8, inserts=[
                insert("Internal", "nirbija.fxpad", fxpad({4: 0.7})),
            ]),
            channel("Hall", bus=True, gain=0.8, inserts=[insert(*HALL)]),
        ],
        "scenes": scenes(),
    }
    out = here / "jam-showcase.json"
    out.write_text(json.dumps(session, indent=2) + "\n")
    print("wrote", out, f"({out.stat().st_size / 1024:.1f}K)")


if __name__ == "__main__":
    main()
