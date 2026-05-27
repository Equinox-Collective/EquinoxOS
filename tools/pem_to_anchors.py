#!/usr/bin/env python3
"""
pem_to_anchors.py — convert one or more X.509 PEM certificates into a
BearSSL br_x509_trust_anchor[] C header.

Replaces BearSSL's brssl ta utility (which we don't vendor; see
third_party/bearssl/README.equos.md). Used in two modes:

  * Local self-signed cert(s) for tlstest.elf:
        python tools/pem_to_anchors.py app/dev_cert.pem > app/ca_anchors.h

  * Mozilla CCADB / curl.se cacert.pem bundle for the browser:
        python tools/fetch_ca_bundle.py
    which calls this script with --array-name TAs_MOZ
    --var-prefix MOZ --include-guard EQUOS_CA_BUNDLE_H
    --header-comment "<provenance line>" /tmp/cacert.pem
    > third_party/ca_bundle/ca_bundle.h

Each input file can hold one OR MANY PEM blocks; we walk them in order
and emit one trust_anchor[] entry per certificate. Both RSA and EC
(secp256r1 / secp384r1 / secp521r1) keys are supported, which is what
the Mozilla bundle requires today.

Hard constraints encoded here so that the consumer side never has to
care:

  * Anchor flag is always BR_X509_TA_CA. The Mozilla bundle is roots
    and intermediates, every cert is a CA; the local self-signed dev
    cert is also marked CA:TRUE by `openssl req -x509`.

  * EC keys are emitted with their uncompressed point form (0x04 || X
    || Y) — BearSSL's x509_minimal expects exactly this serialization
    in br_ec_public_key.q.

  * The DN we put in br_x500_name.data is the full DER encoding of the
    Subject SEQUENCE (including its tag/length header), which is what
    BearSSL matches against the Issuer of a presented chain cert.

Dependency: the `cryptography` PyPI package — used for PEM/DER parsing
and key extraction so this script stays short and the failure modes
are readable.
"""

from __future__ import annotations

import argparse
import re
import sys
from typing import Iterable, Iterator

from cryptography import x509
from cryptography.hazmat.primitives.asymmetric import rsa, ec
from cryptography.hazmat.primitives.serialization import (
    Encoding,
    PublicFormat,
)


# BearSSL curve IDs (from third_party/bearssl/inc/bearssl_ec.h). Only the
# three NIST curves that actually appear in the Mozilla bundle are listed
# — extend if a future bundle adds a fourth.
_EC_CURVE_IDS = {
    "secp256r1": "BR_EC_secp256r1",
    "secp384r1": "BR_EC_secp384r1",
    "secp521r1": "BR_EC_secp521r1",
}

# Matches one PEM CERTIFICATE block (greedy on body, anchored on the
# armor lines). re.DOTALL so '.' eats newlines.
_PEM_RE = re.compile(
    rb"-----BEGIN CERTIFICATE-----[\s\S]*?-----END CERTIFICATE-----",
)


def _iter_certs(paths: Iterable[str]) -> Iterator[x509.Certificate]:
    """Yield every PEM-encoded certificate found across the given files."""
    for path in paths:
        with open(path, "rb") as f:
            data = f.read()
        blocks = _PEM_RE.findall(data)
        if not blocks:
            raise SystemExit(f"no PEM CERTIFICATE blocks found in {path}")
        for block in blocks:
            yield x509.load_pem_x509_certificate(block)


def _emit_byte_array(name: str, data: bytes) -> None:
    """Print `static const unsigned char NAME[] = { ... };` 12 bytes/line."""
    print(f"static const unsigned char {name}[] = {{")
    for i in range(0, len(data), 12):
        chunk = data[i:i + 12]
        print("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    print("};")


def _int_to_be(n: int) -> bytes:
    if n == 0:
        return b"\x00"
    return n.to_bytes((n.bit_length() + 7) // 8, "big")


def _sanitize_label(s: str, maxlen: int = 48) -> str:
    """Turn a subject CN into something safe to drop into a C comment."""
    s = re.sub(r"[^\x20-\x7e]", "?", s)
    s = s.replace("*/", "*-/")
    return s[:maxlen]


def _subject_label(cert: x509.Certificate) -> str:
    """One-line CN/O hint for the per-anchor comment."""
    parts: list[str] = []
    for attr in cert.subject:
        try:
            name = attr.oid._name  # e.g. 'commonName'
        except Exception:
            name = "?"
        if name in ("commonName", "organizationName", "organizationalUnitName"):
            parts.append(f"{name}={attr.value}")
    return _sanitize_label("; ".join(parts) or "unknown subject")


def _emit_anchor_block(
    idx: int,
    var_prefix: str,
    cert: x509.Certificate,
) -> dict:
    """Emit the per-cert byte arrays and return a descriptor dict used
    later to assemble the br_x509_trust_anchor[] table."""

    subj_der = cert.subject.public_bytes()
    pub = cert.public_key()

    base = f"{var_prefix}{idx}"
    print(f"/* {idx}: {_subject_label(cert)} */")
    _emit_byte_array(f"{base}_DN", subj_der)

    if isinstance(pub, rsa.RSAPublicKey):
        nums = pub.public_numbers()
        n_bytes = _int_to_be(nums.n)
        e_bytes = _int_to_be(nums.e)
        _emit_byte_array(f"{base}_RSA_N", n_bytes)
        _emit_byte_array(f"{base}_RSA_E", e_bytes)
        print()
        return {"idx": idx, "kind": "rsa", "base": base}

    if isinstance(pub, ec.EllipticCurvePublicKey):
        curve_name = pub.curve.name
        bear_id = _EC_CURVE_IDS.get(curve_name)
        if bear_id is None:
            raise SystemExit(
                f"unsupported EC curve {curve_name!r} in cert "
                f"{_subject_label(cert)!r} — extend _EC_CURVE_IDS."
            )
        # BearSSL stores the public point in uncompressed form: 0x04 || X || Y.
        q = pub.public_bytes(
            Encoding.X962,
            PublicFormat.UncompressedPoint,
        )
        _emit_byte_array(f"{base}_EC_Q", q)
        print()
        return {"idx": idx, "kind": "ec", "base": base, "curve": bear_id}

    raise SystemExit(
        f"unsupported public-key type in cert {_subject_label(cert)!r}"
    )


def _emit_table(array_name: str, anchors: list[dict]) -> None:
    print(f"static const br_x509_trust_anchor {array_name}[] = {{")
    for a in anchors:
        base = a["base"]
        print("    {")
        print(f"        {{ (unsigned char *){base}_DN, sizeof {base}_DN }},")
        print("        BR_X509_TA_CA,")
        print("        {")
        if a["kind"] == "rsa":
            print("            BR_KEYTYPE_RSA,")
            print("            { .rsa = {")
            print(f"                (unsigned char *){base}_RSA_N, sizeof {base}_RSA_N,")
            print(f"                (unsigned char *){base}_RSA_E, sizeof {base}_RSA_E,")
            print("            } }")
        else:  # ec
            print("            BR_KEYTYPE_EC,")
            print("            { .ec = {")
            print(f"                {a['curve']},")
            print(f"                (unsigned char *){base}_EC_Q, sizeof {base}_EC_Q,")
            print("            } }")
        print("        }")
        print("    },")
    print("};")
    print(f"#define {array_name}_NUM "
          f"((size_t)(sizeof {array_name} / sizeof {array_name}[0]))")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("paths", nargs="+", help="PEM file(s); each may hold many certs")
    ap.add_argument(
        "--array-name",
        default="TAs",
        help="C identifier for the emitted br_x509_trust_anchor[] (default: TAs)",
    )
    ap.add_argument(
        "--var-prefix",
        default="TA",
        help="prefix for per-cert byte arrays, e.g. MOZ -> MOZ0_DN, MOZ0_RSA_N (default: TA)",
    )
    ap.add_argument(
        "--include-guard",
        default="_EQUOS_APP_CA_ANCHORS_H",
        help="ifndef/define symbol around the emitted header",
    )
    ap.add_argument(
        "--header-comment",
        default="",
        help="extra line shown at the top of the generated header (e.g. bundle provenance)",
    )
    args = ap.parse_args()

    print("/* GENERATED FILE — do not edit by hand.")
    if args.header_comment:
        print(" *")
        for line in args.header_comment.splitlines():
            print(f" * {line}")
    print(" *")
    print(" * Produced by tools/pem_to_anchors.py from:")
    for p in args.paths:
        print(f" *   {p}")
    print(" */")
    print(f"#ifndef {args.include_guard}")
    print(f"#define {args.include_guard}")
    print()
    print("#include <bearssl.h>")
    print()

    anchors: list[dict] = []
    for idx, cert in enumerate(_iter_certs(args.paths)):
        anchors.append(_emit_anchor_block(idx, args.var_prefix, cert))

    if not anchors:
        raise SystemExit("no anchors produced")

    _emit_table(args.array_name, anchors)
    print()
    print(f"#endif /* {args.include_guard} */")


if __name__ == "__main__":
    main()
