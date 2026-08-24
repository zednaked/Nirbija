#!/usr/bin/env python3
"""Build jam-sampler.json — the sampler demo.

Nothing but built-in plugins: a step sequencer feeding the host's own 16-pad
sampler. The kit is eight WAV files in sessions/samples/, the way Koala
keeps a pack of samples next to the project, plus a shared FX-pad reverb
bus. No DrumGizmo, no AVL, no SFZ. Press Play.

Rebuild after editing the kit or the patterns:

    python3 sessions/build-sampler.py
"""

from __future__ import annotations

import base64
import json
import math
import struct
import wave
from pathlib import Path

here = Path(__file__).resolve().parent
samples_dir = here / "samples"

RATE = 44100
MAGIC = b"NJSMP02\n"
UNLOCKED = 255

# Factory pad map, with pad 7 retuned to cowbell so eight sequencer lanes
# cover a kit without a leftover crash and a missing bell.
PADS = [
    (36, "Kick"),
    (38, "Snare"),
    (42, "Closed Hat"),
    (46, "Open Hat"),
    (39, "Clap"),
    (37, "Rim"),
    (41, "Floor Tom"),
    (56, "Cowbell"),
    (49, "Crash"),
    (51, "Ride"),
    (47, "Mid Tom"),
    (43, "Low Tom"),
    (45, "High Tom"),
    (40, "E-Snare"),
    (54, "Tambourine"),
    (53, "Ride Bell"),
]


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


# --- tiny drum synth -------------------------------------------------------

def _noise(n: int, seed: int) -> list[float]:
    s = seed & 0x7FFFFFFF
    out = [0.0] * n
    for i in range(n):
        s = (s * 1103515245 + 12345) & 0x7FFFFFFF
        out[i] = s / 0x40000000 - 1.0
    return out


def _exp(i: int, rate: float, seconds: float) -> float:
    return math.exp(-i / (rate * seconds))


def _hp(x: list[float], coeff: float) -> list[float]:
    y = [0.0] * len(x)
    prev_x = prev_y = 0.0
    a = coeff
    for i, sample in enumerate(x):
        y[i] = a * (prev_y + sample - prev_x)
        prev_x, prev_y = sample, y[i]
    return y


def _lp(x: list[float], coeff: float) -> list[float]:
    y = [0.0] * len(x)
    acc = 0.0
    for i, sample in enumerate(x):
        acc += coeff * (sample - acc)
        y[i] = acc
    return y


def _norm(x: list[float], peak: float = 0.86) -> list[float]:
    m = max((abs(v) for v in x), default=0.0)
    if m < 1e-9:
        return x
    g = peak / m
    return [v * g for v in x]


def write_wav(path: Path, mono: list[float]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(RATE)
        frames = b"".join(
            struct.pack("<h", int(max(-1.0, min(1.0, s)) * 32767)) for s in mono
        )
        wav.writeframes(frames)


def kick(seconds: float = 0.42) -> list[float]:
    n = int(RATE * seconds)
    out = [0.0] * n
    for i in range(n):
        t = i / RATE
        freq = 148.0 * _exp(i, RATE, 0.055) + 36.0
        phase = freq * t * 2 * math.pi
        body = math.sin(phase) * _exp(i, RATE, 0.22)
        click = math.sin(2 * math.pi * 1800 * t) * _exp(i, RATE, 0.004) * 0.35
        out[i] = math.tanh((body + click) * 1.8)
    return _norm(out)


def snare(seconds: float = 0.24) -> list[float]:
    n = int(RATE * seconds)
    noise = _hp(_noise(n, 7), 0.55)
    out = [0.0] * n
    for i in range(n):
        t = i / RATE
        tone = math.sin(2 * math.pi * 190 * t) * _exp(i, RATE, 0.12)
        snap = noise[i] * _exp(i, RATE, 0.07)
        out[i] = math.tanh(tone * 0.7 + snap * 1.1)
    return _norm(out, 0.8)


def hat(seconds: float, seed: int) -> list[float]:
    n = int(RATE * seconds)
    raw = _hp(_noise(n, seed), 0.82)
    out = [raw[i] * _exp(i, RATE, seconds * 0.28) for i in range(n)]
    return _norm(out, 0.55)


def clap(seconds: float = 0.22) -> list[float]:
    n = int(RATE * seconds)
    bursts = _hp(_noise(n, 99), 0.6)
    out = [0.0] * n
    for delay_ms, amp in ((0.0, 1.0), (0.011, 0.85), (0.019, 0.7), (0.032, 0.45)):
        d = int(delay_ms * RATE)
        for i in range(d, n):
            out[i] += bursts[i - d] * amp * _exp(i - d, RATE, 0.035)
    tail = _lp(bursts, 0.12)
    for i in range(n):
        out[i] += tail[i] * 0.35 * _exp(i, RATE, 0.12)
        out[i] = math.tanh(out[i])
    return _norm(out, 0.78)


def rim(seconds: float = 0.07) -> list[float]:
    n = int(RATE * seconds)
    out = [0.0] * n
    for i in range(n):
        t = i / RATE
        env = _exp(i, RATE, 0.012)
        out[i] = (
            math.sin(2 * math.pi * 850 * t) * 0.55
            + math.sin(2 * math.pi * 1450 * t) * 0.45
        ) * env
    return _norm(out, 0.7)


def tom(seconds: float = 0.38) -> list[float]:
    n = int(RATE * seconds)
    out = [0.0] * n
    for i in range(n):
        t = i / RATE
        freq = 92.0 * _exp(i, RATE, 0.08) + 48.0
        out[i] = math.tanh(math.sin(2 * math.pi * freq * t) * _exp(i, RATE, 0.2) * 1.4)
    return _norm(out, 0.8)


def cowbell(seconds: float = 0.2) -> list[float]:
    n = int(RATE * seconds)
    out = [0.0] * n
    for i in range(n):
        t = i / RATE
        env = _exp(i, RATE, 0.09)
        a = math.sin(2 * math.pi * 540 * t)
        b = math.sin(2 * math.pi * 800 * t)
        out[i] = math.tanh((a + b) * 0.7) * env
    return _norm(out, 0.6)


# note, name, file, volume, pan
KIT = [
    (36, "Kick", "kick.wav", 1.00, 0.00),
    (38, "Snare", "snare.wav", 0.90, 0.04),
    (42, "Closed Hat", "closed-hat.wav", 0.48, 0.28),
    (46, "Open Hat", "open-hat.wav", 0.52, 0.34),
    (39, "Clap", "clap.wav", 0.78, -0.12),
    (37, "Rim", "rim.wav", 0.62, 0.18),
    (41, "Floor Tom", "floor-tom.wav", 0.82, -0.32),
    (56, "Cowbell", "cowbell.wav", 0.42, 0.38),
]


def write_kit() -> None:
    generators = {
        "kick.wav": kick,
        "snare.wav": snare,
        "closed-hat.wav": lambda: hat(0.07, 3),
        "open-hat.wav": lambda: hat(0.28, 11),
        "clap.wav": clap,
        "rim.wav": rim,
        "floor-tom.wav": tom,
        "cowbell.wav": cowbell,
    }
    for _note, _name, filename, _vol, _pan in KIT:
        write_wav(samples_dir / filename, generators[filename]())


def sampler_blob() -> str:
    """NJSMP02: pads point at files. Rec takes still embed; this kit does not."""
    by_name = {p[1]: p for p in KIT}
    out = bytearray(MAGIC)
    out += struct.pack("<fiii", 1.0, 0, 0, 16)
    for note, name in PADS:
        kit = by_name.get(name)
        if kit is not None:
            _n, _name, filename, vol, pan = kit
            rel = f"samples/{filename}"
            one_shot, pitch, start, end = 1, 0.0, 0.0, 1.0
        else:
            rel = ""
            vol, pan, one_shot, pitch, start, end = 1.0, 0.0, 1, 0.0, 0.0, 1.0
        name_b = name.encode()[:31]
        path_b = rel.encode()
        out += struct.pack(
            "<ii3f2dQdI",
            note,
            one_shot,
            vol,
            pan,
            pitch,
            start,
            end,
            0,  # frames: the file is the audio
            float(RATE),
            len(name_b),
        )
        out += name_b
        out += struct.pack("<I", len(path_b))
        out += path_b
    return b64(bytes(out))


def stepseq_v2(banks: list[dict[int, list[tuple[int, int, int]]]]) -> str:
    """v2 text state. `banks[p][lane] = [(step, vel, accent), ...]`."""
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
    notes = [n for n, _ in PADS[:8]]
    for lane, note in enumerate(notes):
        lines.append(f"lane {lane} {note} 16 2 0 0 0 0.5000 0")
    for p, bank in enumerate(banks):
        for lane, hits in bank.items():
            for step, vel, acc in hits:
                lines.append(
                    f"pstep {p} {lane} {step} {UNLOCKED} {vel} 1 1.0000 "
                    f"{acc} 0 0.0000 1 0 0"
                )
    return b64("\n".join(lines) + "\n")


def fxpad_reverb(amount: float = 0.78) -> str:
    amounts = [0.0] * 16
    amounts[4] = amount  # Reverb
    text = "0\n" + "".join(f"{v}\n" for v in amounts)
    return b64(text)


# lane: 0 kick, 1 snare, 2 closed hat, 3 open hat, 4 clap, 5 rim, 6 tom, 7 cowbell
HOUSE = {
    0: [(0, 120, 1), (4, 108, 0), (8, 120, 1), (12, 108, 0)],
    1: [(4, 112, 1), (12, 118, 1)],
    2: [(i, 62 if i % 4 else 78, 0) for i in range(0, 16, 2) if i != 14],
    3: [(14, 90, 0)],
    4: [(12, 96, 0)],
}

BOOM_BAP = {
    0: [(0, 122, 1), (10, 100, 0)],
    1: [(4, 118, 1), (12, 120, 1)],
    2: [(i, 48 if i % 2 else 64, 0) for i in range(16)],
    5: [(3, 58, 0), (11, 52, 0)],
}

DEMBOW = {
    0: [(0, 120, 1), (6, 110, 0)],
    1: [(4, 114, 1), (12, 118, 1)],
    2: [(2, 70, 0), (3, 52, 0), (8, 70, 0), (10, 58, 0), (14, 64, 0)],
    4: [(4, 88, 0), (12, 92, 0)],
    7: [(0, 80, 0), (8, 74, 0)],
}

BREAK = {
    0: [(0, 122, 1), (6, 100, 0), (10, 108, 0)],
    1: [(4, 118, 1), (12, 120, 1), (13, 86, 0)],
    2: [(i, 58, 0) for i in range(0, 16, 2)],
    3: [(10, 80, 0)],
    6: [(14, 96, 1), (15, 88, 0)],
}


def main() -> None:
    write_kit()
    session = {
        "version": 1,
        "tempo": 108,
        "metronome": False,
        "midiMaps": [],
        "master": {"gain": 0.88, "sink": ""},
        "channels": [
            channel(
                "Kit",
                gain=0.82,
                inserts=[
                    insert(
                        "Internal",
                        "nirbija.stepseq",
                        stepseq_v2([HOUSE, BOOM_BAP, DEMBOW, BREAK]),
                    ),
                    insert("Internal", "nirbija.sampler", sampler_blob()),
                ],
                sends=[send("Room", 0.28)],
            ),
            channel(
                "Room",
                bus=True,
                gain=0.88,
                inserts=[insert("Internal", "nirbija.fxpad", fxpad_reverb(0.78))],
            ),
        ],
    }
    path = here / "jam-sampler.json"
    path.write_text(json.dumps(session, indent=2) + "\n")
    raw = base64.b64decode(session["channels"][0]["inserts"][1]["state"])
    assert raw.startswith(MAGIC), "sampler blob magic is wrong"
    wavs = sorted(p.name for p in samples_dir.glob("*.wav"))
    print(f"wrote {path} ({path.stat().st_size} bytes)")
    print(f"wrote {len(wavs)} samples in {samples_dir}: {', '.join(wavs)}")


if __name__ == "__main__":
    main()
