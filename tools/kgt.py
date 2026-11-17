#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dirk Tostmann
"""KGT — a device template carried inside the parameter segment of its own KNX application.

ETS writes the parameter segment (load state machine 4) of a System B device from the parameter values of
the product. A template embedded there reaches the gateway with every download, without a separate store
or update path.

Segment layout (offsets from the start of the segment):
    0 .. P-1            the application's parameters, unchanged (P = manifest knx.parameters.segment_size)
    P .. P+N*C-1        the template as base64 text in N hidden text parameters of C characters each;
                        ETS pads the last one with NUL
    P+N*C .. +15        trailer, one hidden text parameter: "KGTE" + 4 hex digits each for the text offset,
                        the text length and P

Template blob (before base64):
    "KGT2" | length of the compressed payload (u32, big endian) | first 8 bytes of its SHA-256 |
    key fingerprint (8) | signature r||s (64) | compressed payload
    payload = JSON {schema, id, version, manifest: runtime part, behaviour: Berry source without comment
    lines, behaviour_file}, zlib-compressed in the blob
The signature is ECDSA P-256 with SHA-256 over the uncompressed payload, by a key the gateway trusts
(kgt_trust.json; fingerprint = first 8 bytes of SHA-256 over the public point). The Berry VM is no
sandbox, so the gateway loads an embedded template only with a valid signature. "KGT1"
is the same without key and signature; it is refused unless a caller allows it explicitly (tests).

The gateway keeps one copy per content: the digest is the SHA-256 of the uncompressed payload, so the same
template in many devices is stored once and runs in one Berry VM.
"""
from __future__ import annotations

import base64
import hashlib
import json
import re
import zlib

MAGIC = b"KGT1"                 # unsigned
MAGIC_SIGNED = b"KGT2"
TRAILER_MAGIC = b"KGTE"
TRAILER_LEN = 16
HEAD = 4 + 4 + 8
HEAD_SIGNED = HEAD + 8 + 64


class KgtError(Exception):
    pass


def minify_berry(src: str) -> str:
    """Drop comment-only and blank lines and trailing blanks. Code lines stay as they are (a '#' inside a
    string literal would make stripping trailing comments unsafe, so they are kept)."""
    out = []
    for line in src.splitlines():
        s = line.rstrip()
        if not s.strip() or s.lstrip().startswith("#"):
            continue
        out.append(s)
    return "\n".join(out) + "\n"


def runtime_manifest(m: dict) -> dict:
    """What the gateway needs at run time; texts, sources and the ETS-only parts stay in the product."""
    knx = m["knx"]
    return {
        "schema": m["schema"], "id": m["id"], "version": m["version"], "radio": m.get("radio"),
        "match": m.get("match", {}), "behaviour": dict(m["behaviour"]),
        "datapoints": {k: v for k, v in m.get("datapoints", {}).items() if not k.startswith("_")},
        "knx": {
            "application": {k: knx["application"][k] for k in ("name", "number", "version", "order_info")
                            if k in knx["application"]},
            "objects": [{k: o[k] for k in ("number", "name", "size", "flags", "dpt")} for o in knx["objects"]],
            "parameters": {"segment_size": knx["parameters"]["segment_size"]},
        },
    }


def payload_json(m: dict, behaviour_src: str) -> bytes:
    p = {"schema": "kgt/1", "id": m["id"], "version": m["version"], "manifest": runtime_manifest(m),
         "behaviour_file": m["behaviour"]["file"], "behaviour": minify_berry(behaviour_src)}
    return json.dumps(p, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")


def digest(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()[:16]


def fingerprint(public_point: bytes) -> str:
    return hashlib.sha256(public_point).hexdigest()[:16]


def load_signer(path: str):
    """PEM private key (P-256) -> sign(payload) -> (fingerprint bytes, signature r||s)."""
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec, utils
    key = serialization.load_pem_private_key(open(path, "rb").read(), password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey) or key.curve.name != "secp256r1":
        raise KgtError(f"{path}: not a P-256 key")
    point = key.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
    fpr = bytes.fromhex(fingerprint(point))

    # RFC 6979: the same payload signed with the same key gives the same signature, so a template that has not
    # changed gives the same producer input, and the same application id, on every run.
    from cryptography.exceptions import UnsupportedAlgorithm
    try:
        algo = ec.ECDSA(hashes.SHA256(), deterministic_signing=True)
    except (TypeError, UnsupportedAlgorithm) as e:
        raise KgtError("deterministic ECDSA (RFC 6979) needs the Python package cryptography 44 or later") from e

    def sign(payload: bytes) -> tuple[bytes, bytes]:
        r, s = utils.decode_dss_signature(key.sign(payload, algo))
        return fpr, r.to_bytes(32, "big") + s.to_bytes(32, "big")
    sign.point = point
    return sign


def load_trust(path: str | None = None, points: list[bytes] | None = None) -> dict:
    """Trusted public keys by fingerprint, from kgt_trust.json and/or raw points."""
    from cryptography.hazmat.primitives.asymmetric import ec
    raw = list(points or [])
    if path:
        raw += [bytes.fromhex(k["p256"]) for k in json.load(open(path))["keys"]]
    return {fingerprint(pt): ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), pt) for pt in raw}


def _verify(key, sig: bytes, payload: bytes) -> bool:
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import ec, utils
    try:
        key.verify(utils.encode_dss_signature(int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big")),
                   payload, ec.ECDSA(hashes.SHA256()))
        return True
    except InvalidSignature:
        return False


def pack(m: dict, behaviour_src: str, signer=None) -> bytes:
    """Signed (KGT2) with a signer from load_signer, unsigned (KGT1) without."""
    payload = payload_json(m, behaviour_src)
    z = zlib.compress(payload, 9)
    head = len(z).to_bytes(4, "big") + hashlib.sha256(z).digest()[:8]
    if signer is None:
        return MAGIC + head + z
    fpr, sig = signer(payload)
    return MAGIC_SIGNED + head + fpr + sig + z


def unpack(blob: bytes, trust: dict | None = None, allow_unsigned: bool = False) -> tuple[dict, str]:
    """Blob -> (payload dict, digest); payload["_signed_by"] = key fingerprint or None.
    Raises KgtError on any inconsistency, on a missing or wrong signature, or on an unknown key."""
    magic = blob[:4]
    if magic == MAGIC_SIGNED and len(blob) >= HEAD_SIGNED:
        head, fpr, sig = HEAD_SIGNED, blob[16:24].hex(), blob[24:88]
    elif magic == MAGIC and len(blob) >= HEAD:
        if not allow_unsigned:
            raise KgtError("unsigned template (KGT1) refused")
        head, fpr, sig = HEAD, None, None
    else:
        raise KgtError("no KGT magic")
    n = int.from_bytes(blob[4:8], "big")
    z = blob[head:head + n]
    if len(z) != n:
        raise KgtError(f"payload {len(z)} bytes, header says {n}")
    if hashlib.sha256(z).digest()[:8] != blob[8:16]:
        raise KgtError("payload checksum differs")
    try:
        payload = zlib.decompress(z)
    except zlib.error as e:
        raise KgtError(f"payload unreadable: {e}") from None
    if fpr is not None:
        key = (trust or {}).get(fpr)
        if key is None:
            raise KgtError(f"signed by unknown key {fpr}")
        if not _verify(key, sig, payload):
            raise KgtError(f"signature of key {fpr} does not match")
    try:
        p = json.loads(payload)
    except ValueError as e:
        raise KgtError(f"payload unreadable: {e}") from None
    if p.get("schema") != "kgt/1" or not isinstance(p.get("behaviour"), str) or not isinstance(p.get("manifest"), dict):
        raise KgtError("payload is not a kgt/1 template")
    p["_signed_by"] = fpr
    return p, digest(payload)


def text_of(blob: bytes) -> str:
    return base64.b64encode(blob).decode("ascii")


def trailer(text_off: int, text_len: int, params_len: int) -> str:
    return "KGTE%04X%04X%04X" % (text_off, text_len, params_len)


def layout(params_len: int, text_len: int, chunk: int) -> dict:
    n = -(-text_len // chunk)
    text_off = params_len
    trailer_off = text_off + n * chunk
    return {"chunks": n, "text_off": text_off, "trailer_off": trailer_off, "segment_size": trailer_off + TRAILER_LEN}


def extract(seg: bytes, trust: dict | None = None,
            allow_unsigned: bool = False) -> tuple[dict | None, str | None, bytes, str | None]:
    """Parameter segment -> (template payload or None, digest, parameter bytes, error text).

    Without a trailer the whole segment is parameters (a product without embedded template). A template
    that fails a check (format, checksum, signature, key) is not returned; the error text says why."""
    if len(seg) < TRAILER_LEN or seg[-TRAILER_LEN:-TRAILER_LEN + 4] != TRAILER_MAGIC:
        return None, None, seg, None
    t = seg[-TRAILER_LEN:].decode("ascii", "replace")
    if not re.fullmatch(r"KGTE[0-9A-F]{12}", t):
        return None, None, seg, f"trailer {t!r} malformed"
    text_off, text_len, params_len = int(t[4:8], 16), int(t[8:12], 16), int(t[12:16], 16)
    if not (params_len <= text_off and text_off + text_len <= len(seg) - TRAILER_LEN):
        return None, None, seg[:params_len], f"trailer {t} outside the segment of {len(seg)} bytes"
    raw = seg[text_off:text_off + text_len]
    try:
        blob = base64.b64decode(raw, validate=True)
        p, d = unpack(blob, trust, allow_unsigned)
    except (ValueError, KgtError) as e:
        return None, None, seg[:params_len], f"embedded template rejected: {e}"
    return p, d, seg[:params_len], None
