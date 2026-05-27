# Mozilla CCADB CA bundle for EquinoxOS

This directory ships the public-internet root CA store that the browser
(and any other future TLS client in EquinoxOS) trusts by default.

## Files

| File           | What it is                                                       |
|----------------|------------------------------------------------------------------|
| `cacert.pem`   | The verbatim PEM bundle as published by [curl.se](https://curl.se/docs/caextract.html). |
| `ca_bundle.h`  | Generated BearSSL `br_x509_trust_anchor[] TAs_MOZ` table built from the PEM. |
| `README.equos.md` | This file. |

The header is the only thing application code consumes:

```c
#include "third_party/ca_bundle/ca_bundle.h"

br_ssl_client_init_full(&sc, &xc, TAs_MOZ, TAs_MOZ_NUM);
```

`TAs_MOZ_NUM` expands to the current anchor count.

## Provenance

`cacert.pem` is curl.se's mirror of the
[Mozilla CCADB](https://www.ccadb.org/) root program — specifically the
NSS `builtins` module rendered out of `certdata.txt`. We pin curl.se
instead of NSS directly because curl.se does the
`certdata.txt → PEM` conversion for us and ships a stable filename.

Every cert in the bundle is flagged `BR_X509_TA_CA` in the generated
header (which matches reality — these are all roots / intermediates,
not leaf certs).

## How to refresh

```bash
# from the repo root
python tools/fetch_ca_bundle.py
git diff third_party/ca_bundle/
git add third_party/ca_bundle/cacert.pem third_party/ca_bundle/ca_bundle.h
git commit -m "third_party: refresh CA bundle to <date>"
```

`fetch_ca_bundle.py` re-downloads `cacert.pem` from `curl.se/ca/cacert.pem`
and pipes it through `tools/pem_to_anchors.py`, recording the source
date + SHA256 in the header preamble.

## Curve / key-type support

`pem_to_anchors.py` currently understands:

* RSA — any modulus size.
* EC — `secp256r1`, `secp384r1`, `secp521r1` (the only curves that
  currently appear in the Mozilla program).

If a future Mozilla refresh adds Ed25519 or another curve the converter
will fail loudly with `unsupported EC curve ... — extend _EC_CURVE_IDS`.

## Why not load PEM at runtime?

Two reasons:

1. **No filesystem allocator pressure on boot.** BearSSL needs the
   anchors as fixed `br_x509_trust_anchor` records pointing at stable
   storage. Baking them as `static const` puts everything in
   `.rodata`, which is mapped read-only and shared between processes
   if we ever add CoW for the binary.

2. **No ASN.1 parser in the bootstrap path.** Phase 4 of the browser
   roadmap explicitly avoids depending on BearSSL's own PEM/X.509
   decoder during program startup — we'd rather rely on the converter
   tool, which runs once on the developer's machine.

If/when we add a writable system CA store (e.g. `/etc/ssl/extra.pem`
for user-installed certs), it will be a *supplement* to this baked-in
bundle, not a replacement.
