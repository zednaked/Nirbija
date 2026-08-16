# landing

Static page. Copy lives in `content.json` so the itch page can be
regenerated from the same words.

Three places, three jobs:

| where | what |
|---|---|
| this repo (private) | source, working tree |
| `zednaked/nirbija-site` (public) | **only** this page (GitHub Pages) |
| itch.io | the download — binary + source tarball |

Live at <https://zednaked.github.io/nirbija-site/>.

Do not publish binaries from git. Do not point this page at the
private repository.

## publish the page

```sh
docs/publish.sh            # defaults to ../../nirbija-site
```

It copies one way — **here to there** — and refuses if the public repo
has uncommitted work, then prints what changed for you to commit and
push. Edit the page here, never over there: the two drifted apart once
because a copy was made by hand and then edited on the far side.

## when the itch page exists

Page is up as a draft: <https://zedcave.itch.io/nirbija>

`content.json` already has the URL. Flip the project to **Public** on itch, then `docs/publish.sh` so the landing CTA lights up. Until the page is public, do not publish the site — visitors would hit a 404.

The itch upload is the AppImage (or tarball) **and** the corresponding
source archive. GPLv3.

## after the kooha tape

1. Upload the session to YouTube.
2. Put the video id in `content.json` → `video.youtube`.
3. Drop a better still over `img/hero.png` if you take one.
4. `python3 docs/export-itch.py` when the words change.
