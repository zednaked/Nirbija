#!/usr/bin/env python3
"""Build jam-breakbeat.json — a stress test, not a demo.

Four bars of a breakbeat-style groove at 172 BPM, in the spirit of a chopped
Amen break — syncopated kick, ghost snares, a hi-hat driving every 16th —
with the fourth bar handed over to a machine-gun snare roll built entirely
from the step sequencer's own ratchet field (up to 8 retriggers packed into
one 16th note). No sample of the actual Amen break is used or needed; the
roll is the point, not the break, and ratchets are how this engine already
does one. The kit is 808 Trap (sessions/packs/808-trap/), reused in place
rather than duplicated into sessions/samples/.

Rebuild after editing the pattern:

    python3 sessions/build-breakbeat.py
"""

from __future__ import annotations

import base64
import json
import struct
from pathlib import Path

here = Path(__file__).resolve().parent

MAGIC = b"NJSMP02\n"
UNLOCKED = 255
# 808-trap's own generated rate (sessions/drum_synth.py's RATE). Only a
# placeholder in the blob for a pad with no live buffer yet — resolve_paths
# decodes the real file and its real rate right after load_state runs.
RATE = 44100

# Pad map: the eight 808 Trap voices in their usual slots, the rest silent
# and left on the factory note map — same eight-plus-eight split as every
# other kit here.
PADS = [
    (36, "808 Sub"),
    (38, "Trap Snare"),
    (42, "Tight Hat"),
    (46, "Sizzle Hat"),
    (39, "Big Clap"),
    (37, "Click"),
    (41, "Sub Tom"),
    (56, "Blip"),
    (49, "Crash"),
    (51, "Ride"),
    (47, "Mid Tom"),
    (43, "Low Tom"),
    (45, "High Tom"),
    (40, "E-Snare"),
    (54, "Tambourine"),
    (53, "Ride Bell"),
]

# name -> filename, as build-packs.py wrote them under sessions/packs/808-trap/
KIT_FILES = {
    "808 Sub": "808-sub.wav",
    "Trap Snare": "trap-snare.wav",
    "Tight Hat": "tight-hat.wav",
    "Sizzle Hat": "sizzle-hat.wav",
    "Big Clap": "big-clap.wav",
    "Click": "click.wav",
    "Sub Tom": "sub-tom.wav",
    "Blip": "blip.wav",
}


def b64(raw: bytes | str) -> str:
    if isinstance(raw, str):
        raw = raw.encode()
    return base64.b64encode(raw).decode("ascii")


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


def sampler_blob() -> str:
    """NJSMP02, pads pointing at sessions/packs/808-trap/samples/ — the pack
    folder is the kit, this session just borrows it rather than copying the
    WAVs a second time. Count-in ships on, same as jam-sampler.json."""
    out = bytearray(MAGIC)
    # Gain trimmed down from the usual 1.0: the fourth bar stacks kick, hat
    # and an eight-deep snare roll at once, and a stress test that clips is
    # not testing much beyond the clip.
    out += struct.pack("<fiii", 0.65, 0, 0, 16)
    for note, name in PADS:
        filename = KIT_FILES.get(name)
        if filename is not None:
            rel = f"packs/808-trap/samples/{filename}"
            one_shot, pitch, start, end = 1, 0.0, 0.0, 1.0
        else:
            rel = ""
            one_shot, pitch, start, end = 1, 0.0, 0.0, 1.0
        name_b = name.encode()[:31]
        path_b = rel.encode()
        out += struct.pack(
            "<ii3f2dQdI",
            note, one_shot, 1.0, 0.0, pitch, start, end,
            0,  # frames: the file is the audio
            float(RATE), len(name_b),
        )
        out += name_b
        out += struct.pack("<I", len(path_b))
        out += path_b
    out += struct.pack("<i", 1)  # count-in on, trailing and optional
    return b64(bytes(out))


def fxpad_reverb(amount: float = 0.6) -> str:
    amounts = [0.0] * 16
    amounts[4] = amount  # Reverb
    text = "0\n" + "".join(f"{v}\n" for v in amounts)
    return b64(text)


# --- the pattern -------------------------------------------------------
#
# Hit = (step, velocity, accent, ratchet, microtiming, probability).
# Only step and velocity are ever required; the rest default to a plain,
# on-the-grid, always-fires hit. Four bars of 16th notes, one pattern,
# length 64 — the classic Amen loop length, without the sample.

STEP_BEATS = 0.25  # 1/16 note, division index 2 — see step_sequencer.cpp


def hit(step, vel, *, accent=False, ratchet=1, micro=0.0, prob=1.0):
    return (step, vel, accent, ratchet, micro, prob)


def bar(offset: int, kick_steps, snare_steps, hat_pattern, extra=()):
    """kick_steps/snare_steps: plain step offsets within the bar, full vel.
    hat_pattern: velocity per 16th (16 values) or None to skip a step.
    extra: already-built hit() tuples, offset applied here too."""
    hits = []
    for s in kick_steps:
        hits.append(("kick", hit(offset + s, 118, accent=s == 0)))
    for s in snare_steps:
        hits.append(("snare", hit(offset + s, 112, accent=True)))
    for i, vel in enumerate(hat_pattern):
        if vel is None:
            continue
        hits.append(("hat", hit(offset + i, vel, accent=(i % 4 == 0))))
    for lane, h in extra:
        hits.append((lane, (offset + h[0],) + h[1:]))
    return hits


def breakbeat_pattern():
    lanes: dict[str, list] = {"kick": [], "snare": [], "hat": [], "openhat": [],
                              "clap": [], "click": [], "tom": [], "blip": []}

    hats_a = [72, None, 58, None, 72, None, 58, 66,
              72, None, 58, None, 72, None, 58, None]
    hats_b = [72, None, 58, 50, 72, None, 58, None,
              72, None, 58, None, 72, 50, 58, None]

    # Bars 1-2: the groove settles in.
    for b, hats in ((0, hats_a), (16, hats_b)):
        for lane, h in bar(b, [0, 6, 10], [4, 12], hats):
            lanes[lane].append(h)
        lanes["snare"].append(hit(b + 14, 46, prob=0.75))  # a ghost before 4
        lanes["click"].append(hit(b + 3, 34, prob=0.6, micro=0.015))
        lanes["click"].append(hit(b + 11, 30, prob=0.5, micro=-0.01))
    lanes["openhat"].append(hit(14, 64))
    lanes["openhat"].append(hit(30, 64))
    lanes["blip"].append(hit(0, 90, accent=True))  # the one-bar intro stab

    # Bar 3: the same groove, pushed harder and dirtier.
    for lane, h in bar(32, [0, 6, 9, 10], [4, 12], hats_a):
        lanes[lane].append(h)
    lanes["snare"].append(hit(32 + 2, 40, prob=0.6))
    lanes["snare"].append(hit(32 + 14, 50, prob=0.85))
    lanes["click"].append(hit(32 + 7, 36, prob=0.7, micro=0.02))
    lanes["openhat"].append(hit(32 + 14, 70))

    # Bar 4: the fill. Kick just anchors the top; everything else clears
    # out for a snare roll built from ratchet alone — up to 8 retriggers
    # inside one 16th, climbing in velocity into the loop point.
    lanes["kick"].append(hit(48, 118, accent=True))
    hats_fill = [70, None, 56, None, 70, None, 56, None,
                 None, None, None, None, None, None, None, None]
    for i, vel in enumerate(hats_fill):
        if vel is not None:
            lanes["hat"].append(hit(48 + i, vel))
    lanes["tom"].append(hit(48, 80, accent=True))

    roll_steps = [56, 58, 60, 61, 62, 63]
    roll_ratchets = [3, 3, 4, 4, 6, 8]
    roll_vels = [70, 78, 86, 96, 108, 122]
    for step, ratchet, vel in zip(roll_steps, roll_ratchets, roll_vels):
        lanes["snare"].append(hit(step, vel, accent=vel > 100, ratchet=ratchet))

    return lanes


def stepseq_state() -> str:
    lanes_order = ["kick", "snare", "hat", "openhat", "clap", "click", "tom", "blip"]
    notes = [n for n, _ in PADS[:8]]
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
        # density, chaos, ratchet, master-prob — ratchet at 1.0 so the
        # roll's stored ratchet counts actually fire at full strength.
        "macro 1.0000 0.0000 1.0000 1.0000",
    ]
    for lane, note in enumerate(notes):
        lines.append(f"lane {lane} {note} 64 2 0 0 0 0.5000 0")

    hits = breakbeat_pattern()
    for lane_index, lane_name in enumerate(lanes_order):
        for step, vel, acc, ratchet, micro, prob in hits[lane_name]:
            lines.append(
                f"pstep 0 {lane_index} {step} {UNLOCKED} {vel} 1 {prob:.4f} "
                f"{1 if acc else 0} 0 {micro:.4f} {ratchet} 0 0"
            )
    return b64("\n".join(lines) + "\n")


def main() -> None:
    session = {
        "version": 1,
        "tempo": 172,
        "metronome": False,
        "midiMaps": [],
        "master": {"gain": 0.85, "sink": ""},
        "channels": [
            channel(
                "Break",
                gain=0.85,
                inserts=[
                    insert("Internal", "nirbija.stepseq", stepseq_state()),
                    insert("Internal", "nirbija.sampler", sampler_blob()),
                ],
                sends=[send("Room", 0.22)],
            ),
            channel(
                "Room",
                bus=True,
                gain=0.85,
                inserts=[insert("Internal", "nirbija.fxpad", fxpad_reverb(0.6))],
            ),
        ],
    }
    path = here / "jam-breakbeat.json"
    path.write_text(json.dumps(session, indent=2) + "\n")
    raw = base64.b64decode(session["channels"][0]["inserts"][1]["state"])
    assert raw.startswith(MAGIC), "sampler blob magic is wrong"
    print(f"wrote {path} ({path.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
