#!/bin/sh
# Launch a demo jam without touching the live autosave.
# Usage: sessions/run-jam.sh jam-pad-hall
#        sessions/run-jam.sh /path/to/session.json

set -e
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
name=${1:-jam-pad-hall}
case $name in
  *.json) session=$name ;;
  *) session=$root/sessions/${name}.json ;;
esac
if [ ! -f "$session" ]; then
  echo "no such session: $session" >&2
  echo "try: jam-pad-hall jam-modular-fog jam-tape-drone jam-shimmer-grain jam-kick-cloud jam-black-pearl jam-goth-pearl jam-lucretia jam-more jam-emissaries jam-sampler jam-chord-demo" >&2
  exit 1
fi

app=$root/build/src/ui/nirbija
if [ ! -x "$app" ]; then
  echo "build nirbija first: cmake --build build -j" >&2
  exit 1
fi

export NIRBIJA_SESSION=$session
export QT_QPA_PLATFORM=${QT_QPA_PLATFORM:-xcb}
export __GLX_VENDOR_LIBRARY_NAME=${__GLX_VENDOR_LIBRARY_NAME:-mesa}

cardinal=$HOME/.local/share/cardinal
if [ -d "$cardinal" ] && command -v bwrap >/dev/null 2>&1; then
  exec bwrap --dev-bind / / \
    --overlay-src /usr/share --tmp-overlay /usr/share \
    --bind "$cardinal" /usr/share/cardinal \
    "$app"
fi
exec "$app"
