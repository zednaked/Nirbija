#!/usr/bin/env python3
"""Render docs/itch.md from docs/content.json."""

from __future__ import annotations

import json
from pathlib import Path

here = Path(__file__).resolve().parent
data = json.loads((here / "content.json").read_text())
itch = data["itch"]

index = "\n".join(
    f"| {row['id']} {row['name']} | {row['copy']} |" for row in data["index"]
)
does = "\n".join(f"- {line}" for line in data["does"])
does_not = "\n".join(f"- {line}" for line in data["doesNot"])
requires = "\n".join(f"- {line}" for line in data["requires"])
lede = "\n".join(data["lede"])
compat = " · ".join(data["compat"])
tags = ", ".join(f"`{t}`" for t in itch["tags"])

md = f"""# {itch["title"]}

a mixer you play.

{lede}

**{compat}**

---

## what it is

a mixer. plugins sit in the strip. you play, loop, record. there is no timeline.

| | |
|---|---|
{index}

## does

{does}

## does not

{does_not}

## requires

{requires}

## this page

{itch["kind"]}.

{data["coming"]["body"]}
{data.get("itch", {}).get("url") or ""}

---

**itch fields**

- title: `{itch["title"]}`
- short: `{itch["shortText"]}`
- classification: {itch["classification"]}
- type: {itch["type"]}
- price: {itch["price"]}
- tags: {tags}
- kind: {itch["kind"]}

Source of truth: `docs/content.json`. Regenerate this file with `python3 docs/export-itch.py`.
"""

(here / "itch.md").write_text(md)
print("wrote", here / "itch.md")

index_html = "\n".join(
    f"<li><strong>{row['id']} {row['name']}</strong> — {row['copy']}</li>"
    for row in data["index"]
)
does_html = "\n".join(f"<li>{line}</li>" for line in data["does"])
does_not_html = "\n".join(f"<li>{line}</li>" for line in data["doesNot"])
requires_html = "\n".join(f"<li>{line}</li>" for line in data["requires"])
lede_html = "<br>\n".join(data["lede"])
site = data.get("pages", "")
html = f"""<p>a mixer you play.</p>

<p>{lede_html}</p>

<p><strong>{compat}</strong></p>

<hr>

<h2>what it is</h2>

<p>a mixer. plugins sit in the strip. you play, loop, record. there is no timeline.</p>

<ul>
{index_html}
</ul>

<h2>does</h2>
<ul>
{does_html}
</ul>

<h2>does not</h2>
<ul>
{does_not_html}
</ul>

<h2>requires</h2>
<ul>
{requires_html}
</ul>

<h2>this page</h2>
<p>{itch["kind"]}.</p>
<p>{data["coming"]["body"]}</p>
<p>site: <a href="{site}">{site.replace("https://", "")}</a></p>

<p><em>{data["license"]} · {data["footer"]}</em></p>
"""
itch_dir = here / "itch"
itch_dir.mkdir(exist_ok=True)
(itch_dir / "DESCRIPTION.html").write_text(html)
print("wrote", itch_dir / "DESCRIPTION.html")
