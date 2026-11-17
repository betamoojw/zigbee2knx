#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dirk Tostmann
"""Signing through an external command (--sign-cmd), as a hardware key does it: the command gets the SHA-256 digest
of the payload as hex on stdin and prints r||s as hex. A stand-in signer in a temporary directory plays the hardware
key with a throwaway key; nothing in this tree is written.

  python3 tests/test_sign_cmd.py
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "tools", "knxprod_gen.py")
PRODUCTS = os.path.join(ROOT, "distill", "knxprod_products.py")
TEMPLATE = os.path.join(ROOT, "templates", "zigbee-switch")
SIGNER = r'''
import sys
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, utils
key = serialization.load_pem_private_key(open(sys.argv[1], "rb").read(), None)
digest = bytes.fromhex(sys.stdin.read().strip())
if len(sys.argv) > 2 and sys.argv[2] == "fail":
    print("press the button (refused)", file=sys.stderr)
    sys.exit(4)
r, s = utils.decode_dss_signature(key.sign(digest, ec.ECDSA(utils.Prehashed(hashes.SHA256()), deterministic_signing=True)))
print((r.to_bytes(32, "big") + s.to_bytes(32, "big")).hex())
'''
failed = 0
count = 0


def check(what, ok, detail=""):
    global failed, count
    count += 1
    if not ok:
        failed += 1
    print(f"  {'ok  ' if ok else 'FAIL'} {what}" + (f": {detail}" if detail and not ok else ""))


def run(*cmd):
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    for k in ("ZIGBEE2KNX_SIGN_KEY", "ZIGBEE2KNX_SIGN_CMD", "ZIGBEE2KNX_SIGN_FPR"):
        env.pop(k, None)
    return subprocess.run([sys.executable, "-B", *cmd], capture_output=True, text=True, env=env)


def key_file(path):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    key = ec.generate_private_key(ec.SECP256R1())
    with open(path, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                  serialization.NoEncryption()))
    return key.public_key().public_bytes(serialization.Encoding.X962,
                                         serialization.PublicFormat.UncompressedPoint).hex()


def main():
    with tempfile.TemporaryDirectory() as td:
        pem, other = os.path.join(td, "a.pem"), os.path.join(td, "b.pem")
        pub, _ = key_file(pem), key_file(other)
        signer = os.path.join(td, "signer.py")
        with open(signer, "w", encoding="utf-8") as f:
            f.write(SIGNER)
        cmd = f"{sys.executable} -B {signer} {pem}"
        out_key, out_cmd = os.path.join(td, "key.xml"), os.path.join(td, "cmd.xml")

        print("an external signer gives the same file as the key itself")
        r1 = run(GEN, TEMPLATE, "--sign-key", pem, "-o", out_key)
        r2 = run(GEN, TEMPLATE, "--sign-cmd", cmd, "--sign-pub", pub, "-o", out_cmd)
        check("both runs succeed", r1.returncode == 0 and r2.returncode == 0, r1.stderr[-300:] + r2.stderr[-300:])
        check("byte for byte the same producer input (deterministic signatures)",
              r1.returncode == 0 and r2.returncode == 0 and open(out_key, "rb").read() == open(out_cmd, "rb").read())

        print("what stops the run")
        r = run(GEN, TEMPLATE, "--sign-cmd", f"{sys.executable} -B {signer} {other}", "--sign-pub", pub,
                "-o", os.path.join(td, "x.xml"))
        check("a signature by another key", r.returncode != 0 and "does not verify" in r.stderr, r.stderr[-300:])
        r = run(GEN, TEMPLATE, "--sign-cmd", cmd + " fail", "--sign-pub", pub, "-o", os.path.join(td, "x.xml"))
        check("a signer that refuses (exit status 4), its prompt passed through",
              r.returncode != 0 and "exit status 4" in r.stderr and "press the button" in r.stderr, r.stderr[-300:])
        r = run(GEN, TEMPLATE, "--sign-cmd", cmd, "-o", os.path.join(td, "x.xml"))
        check("a signer command without its public key", r.returncode != 0 and "--sign-pub or --sign-fpr" in r.stderr,
              r.stderr[-300:])
        r = run(GEN, TEMPLATE, "--sign-cmd", cmd, "--sign-fpr", "0000000000000000", "-o", os.path.join(td, "x.xml"))
        check("a fingerprint that is not in kgt_trust.json", r.returncode != 0 and "is not in" in r.stderr,
              r.stderr[-300:])
        r = run(GEN, TEMPLATE, "--sign-cmd", cmd, "--sign-fpr", "0831a8218bc6ae8a", "-o", os.path.join(td, "x.xml"))
        check("a release key's fingerprint with a signer that holds another key",
              r.returncode != 0 and "does not verify" in r.stderr, r.stderr[-300:])
        r = run(GEN, TEMPLATE, "--sign-key", pem, "--sign-cmd", cmd, "--sign-pub", pub, "-o", os.path.join(td, "x.xml"))
        check("a key and a command at once", r.returncode != 0 and "choose one" in r.stderr, r.stderr[-300:])
        check("nothing was written on a refusal", not os.path.exists(os.path.join(td, "x.xml")))

        print("the full run of the class passes the signer on")
        # on a next version of the class: the version of the tree has been given out, and a full run with another
        # key is refused for it (tests/test_version_guard.py)
        nxt = os.path.join(td, "next")
        shutil.copytree(TEMPLATE, nxt)
        mp = os.path.join(nxt, "manifest.json")
        m = json.load(open(mp, encoding="utf-8"))
        v = m["knx"]["application"]["version"]
        m["version"] = m["knx"]["application"]["version"] = v + 1
        m["knx"]["application"]["replaces"] = list(range(1, v + 1))
        json.dump(m, open(mp, "w", encoding="utf-8"))
        p1, p2 = os.path.join(td, "p1.xml"), os.path.join(td, "p2.xml")
        r1 = run(PRODUCTS, nxt, "--sign-key", pem, "-o", p1)
        r2 = run(PRODUCTS, nxt, "--sign-cmd", cmd, "--sign-pub", pub, "-o", p2)
        check("both full runs succeed", r1.returncode == 0 and r2.returncode == 0, r1.stderr[-300:] + r2.stderr[-300:])
        check("the same producer input with all products",
              r1.returncode == 0 and r2.returncode == 0 and open(p1, "rb").read() == open(p2, "rb").read())

    print(f"{count} checks, {failed} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
