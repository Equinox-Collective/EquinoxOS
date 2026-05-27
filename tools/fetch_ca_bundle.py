#!/usr/bin/env python3
"""
fetch_ca_bundle.py — refresh third_party/ca_bundle/ from curl.se.

Pulls the latest Mozilla CCADB-derived `cacert.pem` from curl.se, drops
it under `third_party/ca_bundle/cacert.pem`, then re-runs
`tools/pem_to_anchors.py` to regenerate the BearSSL-format header used
by anything in EquinoxOS that needs to trust the public CA pool (the
browser, primarily).

The bundle URL is intentionally pinned at curl.se rather than the raw
NSS source tree because curl.se does the certdata.txt -> PEM
transformation for us, signs the result, and ships a stable filename.

Usage:
    python tools/fetch_ca_bundle.py
    python tools/fetch_ca_bundle.py --url https://curl.se/ca/cacert.pem

After running, review the diff of `third_party/ca_bundle/ca_bundle.h`
and commit both files. Network access is required.

Dependency: the `cryptography` package (for pem_to_anchors.py).
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import subprocess
import sys
import urllib.request
from datetime import datetime, timezone


DEFAULT_URL = "https://curl.se/ca/cacert.pem"
REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
BUNDLE_DIR = REPO_ROOT / "third_party" / "ca_bundle"
PEM_PATH = BUNDLE_DIR / "cacert.pem"
HEADER_PATH = BUNDLE_DIR / "ca_bundle.h"
CONVERTER = REPO_ROOT / "tools" / "pem_to_anchors.py"


def _download(url: str, dest: pathlib.Path) -> bytes:
    """Fetch `url` into `dest`, return the raw bytes."""
    print(f"[fetch] GET {url}", file=sys.stderr)
    req = urllib.request.Request(url, headers={"User-Agent": "EquinoxOS-fetch-ca/0.1"})
    with urllib.request.urlopen(req, timeout=30) as r:
        data = r.read()
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(data)
    print(f"[fetch] wrote {dest} ({len(data)} bytes)", file=sys.stderr)
    return data


def _extract_as_of(pem_bytes: bytes) -> str:
    """Pull the 'Certificate data from Mozilla as of: ...' header line.
    The curl.se file always starts with a hash-prefixed banner; if the
    format changes we fall back to the wall clock so the build still
    works."""
    for line in pem_bytes.splitlines()[:30]:
        try:
            text = line.decode("ascii", errors="ignore")
        except Exception:
            continue
        if "as of:" in text:
            return text.split("as of:", 1)[1].strip()
    return datetime.now(timezone.utc).strftime("%a %b %d %H:%M:%S %Y UTC (fallback)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--url", default=DEFAULT_URL, help=f"PEM bundle URL (default: {DEFAULT_URL})")
    args = ap.parse_args()

    pem_bytes = _download(args.url, PEM_PATH)
    sha = hashlib.sha256(pem_bytes).hexdigest()
    as_of = _extract_as_of(pem_bytes)

    # Count anchors for the comment without invoking the converter twice.
    n_certs = pem_bytes.count(b"-----BEGIN CERTIFICATE-----")
    print(f"[fetch] {n_certs} certs, as-of='{as_of}', sha256={sha}",
          file=sys.stderr)

    header_comment = (
        "Mozilla CCADB trust anchor bundle (NSS builtins), repackaged\n"
        f"by curl.se as cacert.pem on {as_of}.\n"
        f"SHA256 of source PEM: {sha}\n"
        "\n"
        f"{n_certs} anchors total. To refresh, run tools/fetch_ca_bundle.py — it\n"
        "re-downloads from https://curl.se/ca/cacert.pem and regenerates\n"
        "this header."
    )

    cmd = [
        sys.executable, str(CONVERTER),
        "--array-name", "TAs_MOZ",
        "--var-prefix", "MOZ",
        "--include-guard", "_EQUOS_CA_BUNDLE_H",
        "--header-comment", header_comment,
        str(PEM_PATH.relative_to(REPO_ROOT)),
    ]
    print(f"[fetch] running converter -> {HEADER_PATH}", file=sys.stderr)
    with HEADER_PATH.open("wb") as out:
        # Run from REPO_ROOT so the path comment in the generated header
        # is a clean repo-relative path.
        rc = subprocess.run(cmd, stdout=out, cwd=str(REPO_ROOT)).returncode
    if rc != 0:
        print(f"[fetch] converter failed with rc={rc}", file=sys.stderr)
        return rc

    print(f"[fetch] OK — {HEADER_PATH.relative_to(REPO_ROOT)} updated", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
