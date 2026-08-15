# Ambient jam demos

Session files in the same format as `~/.local/share/Nirbija/Nirbija/session.json`.
Plugins start at factory defaults — open the editor, pick a preset, play notes
from the on-screen keyboard.

The live autosave was **not** touched.

## How to load (without wiping your current jam)

`File → Load session…` replaces the mixer **and** becomes the new autosave.
To try a demo in a sandbox (recommended):

```sh
sessions/run-jam.sh jam-pad-hall
```

That sets `NIRBIJA_SESSION` to the demo file and bind-mounts Cardinal's
module data so the rack is not empty. Same thing by hand:

```sh
NIRBIJA_SESSION="$PWD/sessions/jam-pad-hall.json" \
  __GLX_VENDOR_LIBRARY_NAME=mesa \
  ./build/src/ui/nirbija
```

## The five recipes

| File | Tempo | What it tests |
|---|---|---|
| `jam-pad-hall.json` | 72 | padthv1 + Odin2 + looper/chorus → Dragonfly Hall send |
| `jam-modular-fog.json` | 80 | Cardinal Synth + ChowKick + file player/tape → Cardinal FX + Room |
| `jam-tape-drone.json` | 60 | Surge XT CLAP + Odin2 + Airwindows/looper → Plate + CHOW Tape |
| `jam-shimmer-grain.json` | 88 | padthv1/wolf-shaper + Odin/phaser + reverse delay → Hall + ZamVerb |
| `jam-kick-cloud.json` | 84 | ChowKick + padthv1/tape + stepseq/Odin2 → Hall + Airwindows |

Sends use `busName` so they rebind after load. Some channels use
`destinationKind: bus` (serial into the FX) instead of a send (parallel).

## Cardinal data

Cardinal LV2 is in `~/.lv2`, but the binary still looks for `/usr/share/cardinal`.
User-local copy is at `~/.local/share/cardinal`. One-shot to expose it:

```sh
sudo mkdir -p /usr/share/cardinal
sudo cp -a ~/.local/share/cardinal/. /usr/share/cardinal/
```

Or install the distro package later: `sudo pacman -S cardinal-data cardinal-lv2`.
