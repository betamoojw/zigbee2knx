#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dirk Tostmann
"""gateway_build.py [-o OUT.xml]: the gateway's producer input (C000) from templates/zigbee-gateway.

tools/knxprod_gen.py writes ReplacesVersions="0". The gateway's manifest names the versions its application
replaces (knx.application.replaces); this step puts them in, so that ETS can update a gateway loaded with an
earlier version instead of having it deleted and added again (ETS 6.4.1 offered the update on 2026-11-15 once a
device of the new version had been inserted into the project; whether ReplacesVersions is needed for that is not
tested). The knxprod is then built with OpenKNXproducer create on a Windows PC with ETS.
An output that holds the same application number and version with other content is not overwritten
(--overwrite-version: only for a version never given out).

Default output: producer/zigbee-gateway.xml, written atomically.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TEMPLATE = os.path.join(ROOT, "templates", "zigbee-gateway")
GEN = os.path.join(ROOT, "tools", "knxprod_gen.py")


def atomic_write(path, text):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    fd = os.open(os.path.dirname(os.path.abspath(path)), os.O_RDONLY)
    os.fsync(fd)
    os.close(fd)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-o", "--out", default=os.path.join(HERE, "zigbee-gateway.xml"))
    ap.add_argument("--overwrite-version", action="store_true",
                    help="replace an application of the same number and version that has other content")
    a = ap.parse_args()
    with open(os.path.join(TEMPLATE, "manifest.json"), encoding="utf-8") as f:
        app = json.load(f)["knx"]["application"]
    replaces = [int(v) for v in app.get("replaces", [])]
    if any(v <= 0 or v >= app["version"] for v in replaces):
        sys.exit(f"replaces {replaces}: only versions below {app['version']}")
    with tempfile.TemporaryDirectory() as td:
        raw = os.path.join(td, "gw.xml")
        subprocess.run([sys.executable, GEN, TEMPLATE, "-o", raw], check=True)
        with open(raw, encoding="utf-8") as f:
            xml = f.read()
    old = 'ReplacesVersions="0"'
    if xml.count(old) != 1:
        sys.exit(f"{GEN} wrote {xml.count(old)} times {old}, expected once")
    xml = xml.replace(old, f'ReplacesVersions="{" ".join(str(v) for v in replaces) or "0"}"')
    note = "     ReplacesVersions set by producer/gateway_build.py from knx.application.replaces.\n"
    head = "edit the manifest, not this file.\n"
    if head in xml:
        xml = xml.replace(head, head + note, 1)
    if os.path.exists(a.out) and not a.overwrite_version:
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        from knxprod_gen import version_clash
        clash = version_clash(open(a.out, encoding="utf-8").read(), xml)
        if clash:
            sys.exit(f"gateway_build: {a.out}: {clash}")
    atomic_write(a.out, xml)
    print(f"{a.out}: application version {app['version']}, replaces {replaces or 'none'}")


if __name__ == "__main__":
    main()
