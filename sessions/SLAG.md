# slag — dark industrial ambient

The session that shows what Nirbija is: a mixer you play.
Four voices, two sequencers of different kinds, hall and tape.
No timeline. The arrangement is the strips.

```sh
sessions/run-jam.sh jam-slag
```

64 BPM. C minor industrial (C, Eb, G).

## The rack

| Strip | What | Already baked | You do |
|---|---|---|---|
| **Drone** | Surge XT — *Yeti Funeral* | patch | hold C1 + G1 on the strip keys |
| **Pulse** | stepseq 8×8 → Surge *Eighties Drone* | grid + patch | listen; tweak the grid if you want |
| **Chance** | Stochas → Surge *Metal Pluck* → Crusher | pluck patch | open Stochas, paint a sparse 16-step on C/Eb/G |
| **Kick** | stepseq → ChowKick | 1 and 5 | optional: open ChowKick, longer decay |
| **Hall** | Dragonfly Hall, wet, 6.8s, dark cut | ports | — |
| **Tape** | CHOW Tape + Calf Saturator | factory | pull saturation if it stays too clean |

Pulse and Kick free-run against the host clock. The drone only exists while you hold it. That is the point.

## Playing it

1. Press play on the transport if the seqs are waiting for clock.
2. Hold two notes on **Drone**. The room fills.
3. Leave Pulse and Kick alone. They are the machine.
4. Open Stochas on **Chance**. A few hits, not a groove. Mute the strip when it talks too much.
5. Ride Hall and Tape. Mute Pulse. Bring it back. That is the product.

## If something is silent

- Surge opened on init, not the factory name — open the editor, load the patch from the table.
- Stepseq empty — `python3 sessions/build-slag.py` and reload.
- Stochas silent — it has no baked pattern. Draw one.
- Kick tick but no body — ChowKick editor, raise the pulse / decay.

Rebuild after editing the recipe:

```sh
python3 sessions/build-slag.py
```
