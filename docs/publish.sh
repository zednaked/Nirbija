#!/usr/bin/env bash
# Pushes this page to the public repo that GitHub Pages serves.
#
# The page is written here and only deployed there. It drifted once because the
# copy was done by hand and then edited on the far side; this script copies one
# way and says so if the far side has changes of its own.
#
#   docs/publish.sh [path-to-site-repo]      default ../nirbija-site
set -euo pipefail

docs=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
site=${1:-$(cd "$docs/../.." && pwd)/nirbija-site}

[ -d "$site/.git" ] || { echo "publish: $site is not a git repo" >&2; exit 1; }

if [ -n "$(git -C "$site" status --porcelain)" ]; then
  echo "publish: $site has uncommitted changes - they would be overwritten:" >&2
  git -C "$site" status --short >&2
  exit 1
fi

# publish.sh, export-itch.py and the working notes are the source side's
# business; the public repo carries the page and its own README.
#
# Cleared first so a file deleted here is deleted there too - tar alone would
# leave it behind. .git and the public README are what the far side owns.
find "$site" -mindepth 1 -maxdepth 1 \
  ! -name .git ! -name README.md -exec rm -rf {} +

# Two things live here because they are written against the same words as the
# page, and neither is the page: announce.md is the copy for the launch posts,
# and itch/ is the artwork to upload there. Publishing either would put them
# out before their author meant to.
tar -C "$docs" -cf - \
  --exclude=publish.sh --exclude=export-itch.py --exclude=README.md \
  --exclude=announce.md --exclude=./itch . |
  tar -C "$site" -xf -

cd "$site"
if [ -z "$(git status --porcelain)" ]; then
  echo "publish: nothing changed"
  exit 0
fi

git status --short
echo
echo "review the above, then:"
echo "  cd $site && git add -A && git commit && git push"
