# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dirk Tostmann
"""Helpers for the template tests: one knxgw-berry VM (the firmware's Berry, built by berry/build_hostvm.sh) per
template, requests as in berry/PROTOCOL.md, and the parameter segment as ETS writes it (big endian).

The VM runs the Berry exactly as it is embedded in the product: the "behaviour" of the payload that tools/kgt.py
packs, written to a temporary file. Line numbers in a VM error refer to that text. The generator writes the
template file in that form already (distill/z2m_to_knx.py, shorten_berry); Template.file_is_embedded says whether
it is."""
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PRELUDE = os.path.join(ROOT, "berry", "prelude.be")
KGT_DIR = os.path.join(ROOT, "tools")
VM = os.environ.get("KNXGW_BERRY", os.path.join(ROOT, "build", "hostvm", "knxgw-berry"))   # berry/build_hostvm.sh

sys.path.insert(0, KGT_DIR)
sys.path.insert(0, os.path.join(ROOT, "distill"))
import kgt  # noqa: E402
from z2m_to_knx import shorten_berry  # noqa: E402


class Template:
    def __init__(self, name):
        self.dir = os.path.join(ROOT, "templates", name)
        self.manifest = json.load(open(os.path.join(self.dir, "manifest.json")))
        self.be = os.path.join(self.dir, self.manifest["behaviour"]["file"])
        src = open(self.be, encoding="utf-8").read()
        self.embedded = json.loads(kgt.payload_json(self.manifest, src))["behaviour"]
        self.file_is_embedded = src == self.embedded

    def segment(self, **over):
        knx = self.manifest["knx"]
        b = bytearray(knx["parameters"]["segment_size"])
        for p in knx["parameters"]["list"]:
            v = over.get(p["name"], p["default"])
            n = (knx["parameter_types"][p["type"]]["bits"] + 7) // 8
            b[p["offset"]:p["offset"] + n] = v.to_bytes(n, "big")
        return bytes(b).hex()

    def ko(self, name):
        """Number of the object with this name."""
        return next(o["number"] for o in self.manifest["knx"]["objects"] if o["name"] == name)


class Vm:
    def __init__(self, tpl: Template, exe=VM):
        self.tmp = tempfile.TemporaryDirectory(prefix="vmtest.")
        be = os.path.join(self.tmp.name, os.path.basename(tpl.be))
        with open(be, "w", encoding="utf-8") as f:
            f.write(tpl.embedded)
        self.p = subprocess.Popen([exe, PRELUDE, be], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True)
        ready = json.loads(self.p.stdout.readline())
        assert ready.get("op") == "ready", ready
        self.n = 0

    def call(self, call, inst=1, **kw):
        self.n += 1
        self.p.stdin.write(json.dumps(dict(call=call, inst=inst, id=self.n, **kw)) + "\n")
        self.p.stdin.flush()
        ops = []
        while True:
            m = json.loads(self.p.stdout.readline())
            if m.get("op") == "done":
                assert m["id"] == self.n, m
                if not m["ok"]:
                    raise AssertionError(f"hook failed: {m.get('err')}")
                return ops
            ops.append(m)

    def close(self):
        self.p.stdin.close()
        self.p.wait(timeout=5)
        self.tmp.cleanup()


def sets(ops):
    return [(o["key"], o["value"]) for o in ops if o["op"] == "dp_set"]


def status(ops):
    return [(o["ko"], o["raw"].lower(), o["initial"]) for o in ops if o["op"] == "ko_status"]  # tohex() is upper case


def effects(ops):
    """What a call does towards the device or KNX (log records left out)."""
    return [o for o in ops if o["op"] != "log"]


class Checks:
    def __init__(self):
        self.cases = []

    def __call__(self, name, cond, got):
        self.cases.append((name, cond))
        print(("PASS " if cond else "FAIL ") + name + ("" if cond else f"  got {got}"))

    def finish(self):
        failed = [n for n, ok in self.cases if not ok]
        print(f"{len(self.cases) - len(failed)}/{len(self.cases)} passed")
        return 1 if failed else 0


def rebuild_checks(check, tpl: Template, app_version: int, replaces: list, available: int, section: tuple):
    """What every class has since its current application version: the Berry as embedded, no bus current (and the
    hardware data marked as changed), the application version and the versions it replaces, the template version equal
    to it, the object "Available" with the number the class keeps, the catalog section of the class."""
    knx = tpl.manifest["knx"]
    check("template file is the Berry as embedded", tpl.file_is_embedded, None)
    check("embedded Berry has no comments, indentation or blank lines",
          shorten_berry(tpl.embedded) == tpl.embedded, None)
    check("BusCurrent 0 mA, NonRegRelevantDataVersion 1",
          knx["hardware"]["bus_current_ma"] == 0 and knx["hardware"].get("nonreg_version") == 1, knx["hardware"])
    app = knx["application"]
    check(f"application version {app_version}, replaces {replaces}",
          app["version"] == app_version and app["replaces"] == replaces, app)
    check(f"template version = application version ({app_version})", tpl.manifest["version"] == app["version"],
          tpl.manifest["version"])
    o = [x for x in knx["objects"] if x["number"] == available]
    check(f"object {available} 'Available': 1 bit, DPT 1.002, flags C R T",
          len(o) == 1 and o[0]["name"] == "Available" and o[0]["size"] == "1 Bit" and o[0]["dpt"] == "1.002" and
          o[0]["flags"] == "CRT" and len({x["number"] for x in knx["objects"]}) == len(knx["objects"]), o)
    check("0.availability declared as an event", "0.availability" in tpl.manifest["datapoints"]["events"],
          tpl.manifest["datapoints"]["events"])
    secs = [(s.get("number"), s.get("name")) for s in knx["catalog"].get("sections", [])]
    check(f"catalog: Zigbee2KNX > {section[1]}", secs == [("Zigbee2KNX", "Zigbee2KNX"), section], knx["catalog"])


def availability_checks(check, tpl: Template, vm: Vm, inst: int):
    """dp_event "availability" (endpoint 0, as firmware zlink.c sends it) on the object "Available": the first true
    after linking only fills the shadow, every later change goes out; nothing else happens."""
    ko = tpl.ko("Available")

    def av(value, initial):
        return effects(vm.call("dp_event", inst=inst, ch=0, key="availability", value=value, initial=initial))

    ops = av(True, True)
    check(f"availability true (initial) -> KO{ko} 1, shadow only", status(ops) == [(ko, "01", True)] and len(ops) == 1,
          ops)
    ops = av(False, False)
    check(f"availability false -> KO{ko} 0, sent", status(ops) == [(ko, "00", False)] and len(ops) == 1, ops)
    ops = av(True, False)
    check(f"availability true again -> KO{ko} 1, sent", status(ops) == [(ko, "01", False)] and len(ops) == 1, ops)
