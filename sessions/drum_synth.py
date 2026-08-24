"""A tiny drum synth, shared by every pack-building script in sessions/.

No samples, no libraries: plain sine and noise generators, so a kit is a
few numbers and a WAV writer rather than another binary asset in the repo.
Every instrument takes tunable knobs with defaults matching the original
house kit in build-sampler.py — call one with no arguments and you get
exactly what that kit already sounded like; pass different numbers and you
get a different kit out of the same engine, the way a drum machine's voices
share circuitry across factory patches.
"""

from __future__ import annotations

import struct
import wave
from pathlib import Path

RATE = 44100


def _noise(n: int, seed: int) -> list[float]:
    s = seed & 0x7FFFFFFF
    out = [0.0] * n
    for i in range(n):
        s = (s * 1103515245 + 12345) & 0x7FFFFFFF
        out[i] = s / 0x40000000 - 1.0
    return out


def _exp(i: int, rate: float, seconds: float) -> float:
    import math

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


def kick(
    seconds: float = 0.42,
    start_freq: float = 148.0,
    base_freq: float = 36.0,
    freq_decay: float = 0.055,
    body_decay: float = 0.22,
    click_freq: float = 1800.0,
    click_decay: float = 0.004,
    click_amt: float = 0.35,
    drive: float = 1.8,
    peak: float = 0.86,
) -> list[float]:
    import math

    n = int(RATE * seconds)
    out = [0.0] * n
    for i in range(n):
        t = i / RATE
        freq = start_freq * _exp(i, RATE, freq_decay) + base_freq
        phase = freq * t * 2 * math.pi
        body = math.sin(phase) * _exp(i, RATE, body_decay)
        click = (
            math.sin(2 * math.pi * click_freq * t)
            * _exp(i, RATE, click_decay)
            * click_amt
        )
        out[i] = math.tanh((body + click) * drive)
    return _norm(out, peak)


def snare(
    seconds: float = 0.24,
    tone_freq: float = 190.0,
    tone_decay: float = 0.12,
    snap_decay: float = 0.07,
    tone_amt: float = 0.7,
    snap_amt: float = 1.1,
    noise_hp: float = 0.55,
    noise_seed: int = 7,
    peak: float = 0.8,
) -> list[float]:
    import math

    n = int(RATE * seconds)
    noise = _hp(_noise(n, noise_seed), noise_hp)
    out = [0.0] * n
    for i in range(n):
        t = i / RATE
        tone = math.sin(2 * math.pi * tone_freq * t) * _exp(i, RATE, tone_decay)
        snap = noise[i] * _exp(i, RATE, snap_decay)
        out[i] = math.tanh(tone * tone_amt + snap * snap_amt)
    return _norm(out, peak)


def hat(
    seconds: float,
    seed: int,
    hp: float = 0.82,
    decay_mul: float = 0.28,
    peak: float = 0.55,
) -> list[float]:
    n = int(RATE * seconds)
    raw = _hp(_noise(n, seed), hp)
    out = [raw[i] * _exp(i, RATE, seconds * decay_mul) for i in range(n)]
    return _norm(out, peak)


def clap(
    seconds: float = 0.22,
    hp: float = 0.6,
    seed: int = 99,
    tap_decay: float = 0.035,
    tail_lp: float = 0.12,
    tail_amt: float = 0.35,
    tail_decay: float = 0.12,
    peak: float = 0.78,
) -> list[float]:
    import math

    n = int(RATE * seconds)
    bursts = _hp(_noise(n, seed), hp)
    out = [0.0] * n
    for delay_ms, amp in ((0.0, 1.0), (0.011, 0.85), (0.019, 0.7), (0.032, 0.45)):
        d = int(delay_ms * RATE)
        for i in range(d, n):
            out[i] += bursts[i - d] * amp * _exp(i - d, RATE, tap_decay)
    tail = _lp(bursts, tail_lp)
    for i in range(n):
        out[i] += tail[i] * tail_amt * _exp(i, RATE, tail_decay)
        out[i] = math.tanh(out[i])
    return _norm(out, peak)


def rim(
    seconds: float = 0.07,
    freq1: float = 850.0,
    freq2: float = 1450.0,
    mix1: float = 0.55,
    mix2: float = 0.45,
    decay: float = 0.012,
    peak: float = 0.7,
) -> list[float]:
    import math

    n = int(RATE * seconds)
    out = [0.0] * n
    for i in range(n):
        t = i / RATE
        env = _exp(i, RATE, decay)
        out[i] = (
            math.sin(2 * math.pi * freq1 * t) * mix1
            + math.sin(2 * math.pi * freq2 * t) * mix2
        ) * env
    return _norm(out, peak)


def tom(
    seconds: float = 0.38,
    start_freq: float = 92.0,
    base_freq: float = 48.0,
    freq_decay: float = 0.08,
    amp_decay: float = 0.2,
    drive: float = 1.4,
    peak: float = 0.8,
) -> list[float]:
    import math

    n = int(RATE * seconds)
    out = [0.0] * n
    for i in range(n):
        t = i / RATE
        freq = start_freq * _exp(i, RATE, freq_decay) + base_freq
        out[i] = math.tanh(
            math.sin(2 * math.pi * freq * t) * _exp(i, RATE, amp_decay) * drive
        )
    return _norm(out, peak)


def cowbell(
    seconds: float = 0.2,
    freq1: float = 540.0,
    freq2: float = 800.0,
    decay: float = 0.09,
    mix: float = 0.7,
    peak: float = 0.6,
) -> list[float]:
    import math

    n = int(RATE * seconds)
    out = [0.0] * n
    for i in range(n):
        t = i / RATE
        env = _exp(i, RATE, decay)
        a = math.sin(2 * math.pi * freq1 * t)
        b = math.sin(2 * math.pi * freq2 * t)
        out[i] = math.tanh((a + b) * mix) * env
    return _norm(out, peak)
