# Nirbija — campos do itch

Página sem binário. Fonte: `docs/content.json`.

## Create a new project

| Campo | Valor |
|---|---|
| Title | `Nirbija` |
| Project URL | `https://zedcave.itch.io/nirbija` (criada, **draft**) |
| Short description | `a mixer you play. linux. clap, lv2, vst3.` |
| Classification | **Tools** |
| Kind of project | Downloadable |
| Release status | **In development** |
| Pricing | **No payments** (não tem upload — itch não vende página vazia) |
| Uploads | nenhum |
| Description | cola `DESCRIPTION.html` no modo HTML |
| Genre | — (tools) |
| Tags | `linux`, `audio`, `mixer`, `plugin-host`, `clap`, `lv2`, `vst3`, `pipewire`, `jack` |
| Cover | `cover_630x500.png` |
| Screenshots | `shot_mixer.png`, `shot_strips.png`, `shot_master.png` |
| Visibility | **Restricted** (página pública, sem compra — “não lançou ainda”) |
| Community | comments on |

App store / platforms: **Linux** only.

## Theme (Edit theme, depois de salvar)

| Campo | Valor |
|---|---|
| BG | `#ebe4d4` |
| BG2 | `#e2d8c4` |
| Text | `#161410` |
| Link | `#d24a2e` |
| Font | IBM Plex Mono |
| Headers | IBM Plex Sans |
| Banner | `banner_1920x400.png` |
| Background | cor sólida (sem imagem) |
| Screenshots | Sidebar |

## Depois que a página existir

1. Coloca a URL em `docs/content.json` → `itch.url`
2. `python3 docs/export-itch.py`
3. `docs/publish.sh` e commit no `nirbija-site` — o CTA do landing acende
