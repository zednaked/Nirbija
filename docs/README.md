# landing

Static page. Copy lives in `content.json` so the itch page can be
regenerated from the same words.

Three places, three jobs:

| where | what |
|---|---|
| this repo (private) | source, working tree |
| a public git repo | **only** this page (GitHub Pages) |
| itch.io | the download — binary + source tarball |

Do not publish binaries from git. Do not point this page at the
private repository.

## publish the page

Copy `docs/` to the public pages repo (or make it that repo's root).
Enable Pages there.

## when the itch page exists

```json
"itch": { "url": "https://<you>.itch.io/nirbija" }
```

Nav and the CTA light up. Until then they stay off.

The itch upload is the AppImage (or tarball) **and** the corresponding
source archive. GPLv3.

## after the kooha tape

1. Upload the session to YouTube.
2. Put the video id in `content.json` → `video.youtube`.
3. Drop a better still over `img/hero.png` if you take one.
4. `python3 docs/export-itch.py` when the words change.
