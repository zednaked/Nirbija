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
