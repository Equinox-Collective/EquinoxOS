#!/usr/bin/env python3
"""
Build site/data.json — a snapshot of GitHub stats for both EquinoxOS and enGUI,
plus a copy of ROADMAP.md. Runs hourly via .github/workflows/site-data.yml.

Uses GITHUB_TOKEN (authenticated 5000 req/hr) instead of the anonymous 60/hr
limit that the browser was hitting.
"""

from __future__ import annotations

import datetime as _dt
import json
import os
import pathlib
import re
import sys
from typing import Any

import requests

REPOS = [
    {"key": "eq", "owner": "Equinox-Collective", "name": "EquinoxOS"},
    {"key": "en", "owner": "Equinox-Collective", "name": "enGUI"},
]

API = "https://api.github.com"


def _headers() -> dict[str, str]:
    h = {
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": "equinoxos-site-data",
    }
    tok = os.environ.get("GITHUB_TOKEN", "")
    if tok:
        h["Authorization"] = f"Bearer {tok}"
    return h


def _get(path: str, **params: Any) -> requests.Response:
    r = requests.get(API + path, headers=_headers(), params=params, timeout=30)
    r.raise_for_status()
    return r


def commit_count(owner: str, name: str) -> int:
    """Total commits on default branch. Uses the Link header trick."""
    r = _get(f"/repos/{owner}/{name}/commits", per_page=1)
    link = r.headers.get("link", "")
    m = re.search(r'[?&]page=(\d+)>;\s*rel="last"', link)
    if m:
        return int(m.group(1))
    arr = r.json()
    return len(arr) if isinstance(arr, list) else 0


def repo_snapshot(owner: str, name: str) -> dict[str, Any]:
    info = _get(f"/repos/{owner}/{name}").json()
    prs = _get(f"/repos/{owner}/{name}/pulls", state="open", per_page=100).json()
    commits = _get(f"/repos/{owner}/{name}/commits", per_page=1).json()
    languages = _get(f"/repos/{owner}/{name}/languages").json()
    latest = commits[0] if commits else None
    return {
        "name":   info["name"],
        "owner":  info["owner"]["login"],
        "full":   info["full_name"],
        "url":    info["html_url"],
        "desc":   info.get("description") or "",
        "stars":  info.get("stargazers_count", 0),
        "forks":  info.get("forks_count", 0),
        "issues": info.get("open_issues_count", 0),
        "prs":    len(prs) if isinstance(prs, list) else 0,
        "commits": commit_count(owner, name),
        "latest": ({
            "sha":     latest["sha"][:7],
            "message": latest["commit"]["message"],
            "date":    latest["commit"]["author"]["date"],
            "url":     latest["html_url"],
        } if latest else None),
        "languages": languages,
        "pushed_at": info.get("pushed_at"),
    }


def main() -> int:
    out: dict[str, Any] = {
        "generated_at": _dt.datetime.now(_dt.timezone.utc).isoformat(timespec="seconds"),
        "repos": {},
    }

    for r in REPOS:
        print(f"-> {r['owner']}/{r['name']}")
        out["repos"][r["key"]] = repo_snapshot(r["owner"], r["name"])

    # Embed the current ROADMAP.md as well so the page never has to fetch it
    # separately (and so language-switch / offline never breaks the section).
    rm_path = pathlib.Path("ROADMAP.md")
    out["roadmap"] = rm_path.read_text(encoding="utf-8") if rm_path.exists() else ""

    out_path = pathlib.Path("site/data.json")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(
        json.dumps(out, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )

    eq, en = out["repos"]["eq"], out["repos"]["en"]
    print(f"   EquinoxOS: {eq['commits']} commits, {eq['stars']}★, {eq['prs']} open PRs")
    print(f"   enGUI:     {en['commits']} commits, {en['stars']}★, {en['prs']} open PRs")
    print(f"   roadmap:   {len(out['roadmap']):,} bytes")
    print(f"   wrote      {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
