#!/usr/bin/env python3
"""
pem_to_anchors.py — convert one or more X.509 PEM certificates into a
BearSSL br_x509_trust_anchor[] C header.

This is a tiny in-tree replacement for BearSSL's `brssl ta` utility — we
deliberately did NOT vendor the brssl tool (see third_party/bearssl/
README.equos.md), so this script fills the same hole.

Usage:
    python tools/pem_to_anchors.py cert.pem [cert2.pem ...] > app/ca_anchors.h

Output: a self-contained C header that defines

    static const br_x509_trust_anchor TAs[];
    #define TAs_NUM ...

ready to be passed to br_ssl_client_init_full(&sc, &xc, TAs, TAs_NUM).

Only RSA anchors are emitted right now — that's all phase 3c needs. EC
support is a TODO; the unreached branch raises explicitly.

Dependency: the `cryptography` PyPI package (`pip install cryptography`).
We use it instead of pure-Python ASN.1 parsing so the script stays short
and the failure modes are clear (bad PEM -> readable exception, not a
hand-rolled parser stack trace).
"""

import sys
from cryptography import x509
from cryptography.hazmat.primitives.asymmetric import rsa, ec


def _emit_byte_array(name: str, data: bytes) -> None:
    """Print `static const unsigned char NAME[] = { ... };` 12 bytes/line."""
    print(f"static const unsigned char {name}[] = {{")
    for i in range(0, len(data), 12):
        chunk = data[i:i + 12]
        print("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    print("};")
    print()


def _int_to_be(n: int) -> bytes:
    """Big-endian unsigned encoding of a positive integer, minimal length."""
    if n == 0:
        return b"\x00"
    return n.to_bytes((n.bit_length() + 7) // 8, "big")


def main(paths: list[str]) -> None:
    print("/* GENERATED FILE — do not edit by hand.")
    print(" *")
    print(" * Produced by tools/pem_to_anchors.py from:")
    for p in paths:
        print(f" *   {p}")
    print(" *")
    print(" * Regenerate with:")
    print(" *   python tools/pem_to_anchors.py <pem...> > app/ca_anchors.h")
    print(" */")
    print("#ifndef _EQUOS_APP_CA_ANCHORS_H")
    print("#define _EQUOS_APP_CA_ANCHORS_H")
    print()
    print("#include <bearssl.h>")
    print()

    rsa_anchors: list[int] = []  # indices of RSA entries

    for idx, path in enumerate(paths):
        with open(path, "rb") as f:
            cert = x509.load_pem_x509_certificate(f.read())

        subject_der = cert.subject.public_bytes()
        pub = cert.public_key()

        if isinstance(pub, rsa.RSAPublicKey):
            nums = pub.public_numbers()
            n_bytes = _int_to_be(nums.n)
            e_bytes = _int_to_be(nums.e)
            _emit_byte_array(f"TA{idx}_DN", subject_der)
            _emit_byte_array(f"TA{idx}_RSA_N", n_bytes)
            _emit_byte_array(f"TA{idx}_RSA_E", e_bytes)
            rsa_anchors.append(idx)
        elif isinstance(pub, ec.EllipticCurvePublicKey):
            raise NotImplementedError(
                "EC trust anchors not implemented yet — extend "
                "pem_to_anchors.py if you need them."
            )
        else:
            raise SystemExit(f"unsupported public-key type in {path}")

    if not rsa_anchors:
        raise SystemExit("no anchors produced")

    print("static const br_x509_trust_anchor TAs[] = {")
    for idx in rsa_anchors:
        print("    {")
        print(f"        {{ (unsigned char *)TA{idx}_DN, sizeof TA{idx}_DN }},")
        print("        BR_X509_TA_CA,")
        print("        {")
        print("            BR_KEYTYPE_RSA,")
        print("            { .rsa = {")
        print(f"                (unsigned char *)TA{idx}_RSA_N, sizeof TA{idx}_RSA_N,")
        print(f"                (unsigned char *)TA{idx}_RSA_E, sizeof TA{idx}_RSA_E,")
        print("            } }")
        print("        }")
        print("    },")
    print("};")
    print("#define TAs_NUM ((size_t)(sizeof TAs / sizeof TAs[0]))")
    print()
    print("#endif /* _EQUOS_APP_CA_ANCHORS_H */")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit("usage: pem_to_anchors.py <cert.pem> [more.pem ...]")
    main(sys.argv[1:])
