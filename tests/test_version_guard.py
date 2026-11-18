#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dirk Tostmann
"""The protections of a version that has been given out, without touching the files of this tree.

1. Signatures are deterministic (RFC 6979): a template that has not changed gives the same producer input, and so
   the same application id, on every run.
2. The generators refuse to overwrite an output that holds the same application number and version with other
   content: a full run over producer/<class>.xml with another key (or after a change of the template without a new
   version) stops instead of making a second application under the same number and version.
3. Whatever the switches, a full run writes nothing that does not fit the versions given out (producer/issued.json):
   a version given out with other content, or ReplacesVersions without every lower version given out.

A throwaway key is made for the test; the producer inputs are copied into a temporary directory first.

  python3 tests/test_version_guard.py
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import kgt  # noqa: E402
from knxprod_gen import version_clash  # noqa: E402

GEN = os.path.join(ROOT, "tools", "knxprod_gen.py")
PRODUCTS = os.path.join(ROOT, "distill", "knxprod_products.py")
GATEWAY = os.path.join(ROOT, "producer", "gateway_build.py")
VARIANT = "0xCF03"   # a test variant of the switch class: a number of its own, not in producer/issued.json
failed = 0
count = 0


def check(what, ok, detail=""):
    global failed, count
    count += 1
    if not ok:
        failed += 1
    print(f"  {'ok  ' if ok else 'FAIL'} {what}" + (f": {detail}" if detail and not ok else ""))


def run(*cmd):
    return subprocess.run([sys.executable, *cmd], capture_output=True, text=True)


def throwaway_key(path):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    key = ec.generate_private_key(ec.SECP256R1())
    with open(path, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                  serialization.NoEncryption()))


def main():
    with tempfile.TemporaryDirectory() as td:
        pem = os.path.join(td, "key.pem")
        throwaway_key(pem)

        print("deterministic signatures")
        sign = kgt.load_signer(pem)
        payload = b'{"test":"the same payload twice"}'
        check("the same payload gives the same signature", sign(payload) == sign(payload))
        check("another payload gives another signature", sign(payload)[1] != sign(payload + b" ")[1])
        a, b = os.path.join(td, "a.xml"), os.path.join(td, "b.xml")
        ra = run(GEN, os.path.join(ROOT, "templates", "zigbee-switch"), "--sign-key", pem, "--app-number", VARIANT, "-o", a)
        rb = run(GEN, os.path.join(ROOT, "templates", "zigbee-switch"), "--sign-key", pem, "--app-number", VARIANT, "-o", b)
        check("two generator runs succeed", ra.returncode == 0 and rb.returncode == 0, ra.stderr + rb.stderr)
        same = ra.returncode == 0 and rb.returncode == 0 and open(a, "rb").read() == open(b, "rb").read()
        check("two runs over an unchanged template write the same file", same)

        print("an output with the same version and other content is not overwritten")
        r = run(GEN, os.path.join(ROOT, "templates", "zigbee-switch"), "--sign-key", pem, "--app-number", VARIANT, "-o", a)
        check("the generator writes over its own identical output", r.returncode == 0, r.stderr)
        text = open(a, encoding="utf-8").read()
        changed = text.replace("<Data>", "<Data>A", 1)
        check("version_clash sees other content under the same version", bool(version_clash(text, changed)))
        check("version_clash lets the same content pass", version_clash(text, text) is None)
        check("version_clash ignores another version", version_clash(
            text, changed.replace('ApplicationVersion="', 'ApplicationVersion="9', 1)) is None)
        with open(b, "w", encoding="utf-8") as f:
            f.write(changed)
        r = run(GEN, os.path.join(ROOT, "templates", "zigbee-switch"), "--sign-key", pem, "--app-number", VARIANT, "-o", b)
        check("the generator refuses to overwrite it", r.returncode != 0 and "exists in the output" in r.stderr, r.stderr)
        r = run(GEN, os.path.join(ROOT, "templates", "zigbee-switch"), "--sign-key", pem, "--app-number", VARIANT, "-o", b, "--overwrite-version")
        check("--overwrite-version overwrites it", r.returncode == 0 and open(b, "rb").read() == open(a, "rb").read(),
              r.stderr)

        print("the generator alone keeps to the versions given out")
        c = os.path.join(td, "c.xml")
        r = run(GEN, os.path.join(ROOT, "templates", "zigbee-switch"), "--sign-key", pem, "-o", c)
        check("a version given out, signed with another key into a new file, is refused",
              r.returncode != 0 and "has been given out" in r.stderr, r.stderr[-300:])
        r = run(GEN, os.path.join(ROOT, "templates", "zigbee-switch"), "--sign-key", pem, "-o", c, "--overwrite-version")
        check("--overwrite-version does not change that", r.returncode != 0 and "has been given out" in r.stderr,
              r.stderr[-300:])
        check("nothing was written", not os.path.exists(c))

        print("the producer inputs of this tree are protected")
        for c in ("zigbee-dimmable-light", "zigbee-white-spectrum-light", "zigbee-switch", "zigbee-remote"):
            out = os.path.join(td, c + ".xml")
            shutil.copyfile(os.path.join(ROOT, "producer", c + ".xml"), out)
            before = open(out, "rb").read()
            r = run(PRODUCTS, os.path.join(ROOT, "templates", c), "--sign-key", pem, "-o", out)
            check(f"{c}: a full run with another key stops", r.returncode != 0 and (
                "exists in the output" in r.stderr or "has been given out" in r.stderr), r.stderr[-300:])
            check(f"{c}: the file is unchanged", open(out, "rb").read() == before)
        out = os.path.join(td, "zigbee-gateway.xml")
        shutil.copyfile(os.path.join(ROOT, "producer", "zigbee-gateway.xml"), out)
        before = open(out, "rb").read()
        r = subprocess.run([sys.executable, GATEWAY, "-o", out], capture_output=True, text=True,
                           env=dict(os.environ, ZIGBEE2KNX_SIGN_KEY=pem))
        # the gateway's application embeds no template, so nothing in it is signed: a run gives the same file
        check("zigbee-gateway: a run reproduces the file of this tree", r.returncode == 0, r.stderr[-300:])
        check("zigbee-gateway: byte for byte", open(out, "rb").read() == before)
        with open(out, "w", encoding="utf-8") as f:
            f.write(before.decode("utf-8").replace('Text="Bind', 'Text="(changed) Bind', 1))
        r = subprocess.run([sys.executable, GATEWAY, "-o", out], capture_output=True, text=True,
                           env=dict(os.environ, ZIGBEE2KNX_SIGN_KEY=pem))
        check("zigbee-gateway: a run over a file of the same version with other content stops",
              r.returncode != 0 and "exists in the output" in r.stderr, r.stderr[-300:])

        print("what does not fit the versions given out is not written, whatever the switches")
        t = os.path.join(td, "zigbee-switch")
        shutil.copytree(os.path.join(ROOT, "templates", "zigbee-switch"), t)
        mp = os.path.join(t, "manifest.json")
        m = json.load(open(mp, encoding="utf-8"))
        m["version"] = m["knx"]["application"]["version"] = 2
        m["knx"]["application"]["replaces"] = [1]
        json.dump(m, open(mp, "w", encoding="utf-8"))
        out = os.path.join(td, "again.xml")
        r = run(PRODUCTS, t, "--sign-key", pem, "-o", out, "--overwrite-version")
        check("0xC003 version 2 (given out) from the template of this tree: refused, also with --overwrite-version",
              r.returncode != 0 and "has been given out" in r.stderr and not os.path.exists(out), r.stderr[-300:])
        m["version"] = m["knx"]["application"]["version"] = 9
        m["knx"]["application"]["replaces"] = [2]
        json.dump(m, open(mp, "w", encoding="utf-8"))
        r = run(PRODUCTS, t, "--sign-key", pem, "-o", out)
        check("a new version whose replaces leave out version 1 (given out): refused",
              r.returncode != 0 and "ReplacesVersions" in r.stderr and not os.path.exists(out), r.stderr[-300:])
        m["knx"]["application"]["replaces"] = [1, 2, 3]
        json.dump(m, open(mp, "w", encoding="utf-8"))
        r = run(PRODUCTS, t, "--sign-key", pem, "-o", out)
        check("the same with replaces 1, 2 and 3: written", r.returncode == 0 and os.path.exists(out), r.stderr[-300:])

    print(f"{count} checks, {failed} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
