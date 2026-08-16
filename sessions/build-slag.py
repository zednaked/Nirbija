#!/usr/bin/env python3
"""Build jam-slag.json — dark industrial ambient mixer.

Bakes Surge factory .fxp blobs into CLAP state, and writes LV2 turtle
state for the step sequencers and the hall so the session is not init-patch.
"""

from __future__ import annotations

import base64
import json
from pathlib import Path

here = Path(__file__).resolve().parent
surge_factory = Path("/usr/share/surge-xt/patches_factory")


def b64(raw: bytes) -> str:
    return base64.b64encode(raw).decode("ascii")


def surge_fxp(rel: str) -> str:
    blob = (surge_factory / rel).read_bytes()
    i = blob.find(b"sub3")
    if i < 0:
        raise SystemExit(f"no sub3 chunk in {rel}")
    return b64(blob[i:])


def lv2_state(uri: str, ports: dict[str, float], extra: str = "") -> str:
    blocks = []
    for symbol, value in ports.items():
        blocks.append(
            f"\t[\n\t\tlv2:symbol \"{symbol}\" ;\n\t\tpset:value {value}\n\t]"
        )
    extra_ttl = f" ;\n{extra}" if extra else ""
    text = (
        "@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n"
        "@prefix pset: <http://lv2plug.in/ns/ext/presets#> .\n"
        "@prefix state: <http://lv2plug.in/ns/ext/state#> .\n"
        "\n"
        "<urn:nirbija:state>\n"
        "\ta pset:Preset ;\n"
        f"\tlv2:appliesTo <{uri}> ;\n"
        "\tlv2:port\n"
        + " ,\n".join(blocks)
        + extra_ttl
        + " .\n"
    )
    return b64(text.encode())


def stepseq(bpm: float, div: int, notes: list[int], hits: dict[tuple[int, int], int]) -> str:
    """hits: (step 1-8, row 1-8) -> velocity 0-127."""
    uri = "http://gareus.org/oss/lv2/stepseq#s8n8"
    ports: dict[str, float] = {
        "sync": 1.0,
        "bpm": float(bpm),
        "div": float(div),
        "swing": 0.0,
        "drummode": 0.0,
        "chn": 0.0,
    }
    for i, n in enumerate(notes, start=1):
        ports[f"note{i}"] = float(n)
    for step in range(1, 9):
        for row in range(1, 9):
            ports[f"grid_{step}_{row}"] = float(hits.get((step, row), 0))
    return lv2_state(uri, ports)


def insert(fmt: str, uid: str, state: str | None = None) -> dict:
    out = {"format": fmt, "uid": uid, "bypassed": False, "postFader": False}
    if state:
        out["state"] = state
    return out


def channel(
    name: str,
    *,
    bus: bool = False,
    gain: float,
    pan: float = 0.0,
    dest: str = "master",
    dest_name: str | None = None,
    inserts: list,
    sends: list | None = None,
) -> dict:
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
        "destinationKind": dest,
        "inserts": inserts,
        "sends": sends or [],
    }
    if dest != "master" and dest_name:
        entry["destinationName"] = dest_name
    if not bus:
        entry["audioSource"] = ""
        entry["midiSources"] = []
    return entry


def send(bus_name: str, level: float) -> dict:
    return {"bus": -1, "busName": bus_name, "level": level}


# C2 / Eb2 / G2 / G1 — industrial minor, two octaves of iron
PULSE_NOTES = [36, 39, 43, 31, 48, 51, 55, 43]
PULSE_HITS = {
    (1, 1): 110,  # C2
    (3, 2): 70,   # Eb2 offbeat
    (5, 1): 95,   # C2
    (7, 3): 80,   # G2
    (8, 4): 60,   # G1 pickup
}
KICK_NOTES = [36, 38, 40, 41, 43, 45, 47, 48]
KICK_HITS = {
    (1, 1): 120,
    (5, 1): 85,
}

HALL = "https://github.com/michaelwillis/dragonfly-reverb"
TAPE = "https://github.com/jatinchowdhury18/AnalogTapeModel"
CRUSH = "http://calf.sourceforge.net/plugins/Crusher"
KICK = "http://github.com/Chowdhury-DSP/ChowKick"
SURGE = "org.surge-synth-team.surge-xt"
STOCHAS = "ABCDEF019182FAEB70726F6A53746F63"

session = {
    "version": 1,
    "tempo": 64,
    "metronome": False,
    "midiClock": False,
    "timeNumerator": 4,
    "timeDenominator": 4,
    "midiMaps": [],
    "master": {"gain": 0.78, "sink": "", "dim": False, "mute": False, "mono": False},
    "channels": [
        channel(
            "Drone",
            gain=0.52,
            inserts=[
                insert("CLAP", SURGE, surge_fxp("Pads/Yeti Funeral.fxp")),
            ],
            sends=[send("Hall", 0.48), send("Tape", 0.22)],
        ),
        channel(
            "Pulse",
            gain=0.42,
            pan=-0.12,
            inserts=[
                insert("LV2", "http://gareus.org/oss/lv2/stepseq#s8n8", stepseq(64, 3, PULSE_NOTES, PULSE_HITS)),
                insert("CLAP", SURGE, surge_fxp("Basses/Eighties Drone.fxp")),
            ],
            sends=[send("Hall", 0.22), send("Tape", 0.18)],
        ),
        channel(
            "Chance",
            gain=0.38,
            pan=0.16,
            inserts=[
                insert("VST3", STOCHAS),
                insert("CLAP", SURGE, surge_fxp("FX/Metal Pluck.fxp")),
                insert("LV2", CRUSH),
            ],
            sends=[send("Tape", 0.28), send("Hall", 0.12)],
        ),
        channel(
            "Kick",
            gain=0.68,
            inserts=[
                insert("LV2", "http://gareus.org/oss/lv2/stepseq#s8n8", stepseq(64, 3, KICK_NOTES, KICK_HITS)),
                insert("LV2", KICK),
            ],
            sends=[send("Hall", 0.08), send("Tape", 0.1)],
        ),
        channel(
            "Hall",
            bus=True,
            gain=0.88,
            inserts=[
                insert(
                    "LV2",
                    HALL,
                    lv2_state(
                        HALL,
                        {
                            "dry_level": 0.0,
                            "early_level": 16.0,
                            "late_level": 78.0,
                            "size": 52.0,
                            "width": 120.0,
                            "delay": 18.0,
                            "diffuse": 85.0,
                            "low_cut": 40.0,
                            "low_xo": 400.0,
                            "low_mult": 1.6,
                            "high_cut": 3800.0,
                            "high_xo": 2800.0,
                            "high_mult": 0.35,
                            "spin": 1.4,
                            "wander": 22.0,
                            "decay": 6.8,
                            "early_send": 20.0,
                            "modulation": 20.0,
                        },
                    ),
                )
            ],
        ),
        channel(
            "Tape",
            bus=True,
            gain=0.8,
            inserts=[
                insert("LV2", TAPE),
                insert("LV2", "http://calf.sourceforge.net/plugins/Saturator"),
            ],
        ),
    ],
}

out = here / "jam-slag.json"
out.write_text(json.dumps(session, indent=2) + "\n")
print("wrote", out, f"({out.stat().st_size / 1024:.0f}K)")
