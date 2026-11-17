#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dirk Tostmann
"""distill/knxprod_products.py: the product step of the full run and --append, on the producer inputs and templates
of this tree. Nothing is signed and no ETS library runs: what a build would make of a file is taken from the split
step of an optional knxprod packer (KNXPROD_PACK; its checks are skipped without), which is everything before the
signing.

  python3 tests/test_knxprod_append.py
"""
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "distill"))
import knxprod_products as kp   # noqa: E402

PACK = os.environ.get("KNXPROD_PACK", "")   # optional: a knxprod packer with split(); its checks are skipped without
TOOL = os.path.join(ROOT, "distill", "knxprod_products.py")
CLASSES = ("zigbee-dimmable-light", "zigbee-white-spectrum-light", "zigbee-switch", "zigbee-remote")
PROD = re.compile(r'^[ \t]*<Product [^>]*>\n.*?^[ \t]*</Product>\n', re.M | re.S)
ITEM = re.compile(r'^[ \t]*<CatalogItem [^>]*/>\n', re.M)
failed = 0
count = 0


def check(what, ok, detail=""):
    global failed, count
    count += 1
    if not ok:
        failed += 1
    print(f"  {'ok  ' if ok else 'FAIL'} {what}" + (f": {detail}" if detail and not ok else ""))


def raises(what, fn, part):
    try:
        fn()
    except kp.ProductError as e:
        check(what, part in str(e), str(e))
    else:
        check(what, False, "no error")


def cut(xml, keep):
    """The file with only its first `keep` products and catalog items."""
    for rx in (PROD, ITEM):
        ms = list(rx.finditer(xml))
        for m in reversed(ms[keep:]):
            xml = xml[:m.start()] + xml[m.end():]
    return xml


def load(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def packer():
    spec = importlib.util.spec_from_file_location("knxprod_pack_shared", PACK)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def classes():
    print("the four class inputs of this tree")
    for c in CLASSES:
        xml = load(os.path.join(ROOT, "producer", c + ".xml"))
        tdir = os.path.join(ROOT, "templates", c)
        m = json.load(open(os.path.join(tdir, "manifest.json"), encoding="utf-8"))
        models = [p["model"] for p in m["products"]]
        n = len(PROD.findall(xml))
        check(f"{c}: one product per model of the manifest and the class's own", n == 1 + len(models), f"{n} / {len(models)}")
        out, k = kp.add_products(cut(xml, 1), m["products"], m["knx"].get("language", "en"))
        check(f"{c}: the product step of the full run writes this file", out == xml and k == len(models))
        out, added, present, notes = kp.append_products(cut(xml, n - 3), m["products"])
        check(f"{c}: three products appended give the file of the full run", out == xml)
        check(f"{c}: added are the three that were missing, the others are left", added == models[-3:] and
              present == models[:-3] and notes == [], f"{added} {notes}")
        out, added, present, notes = kp.append_products(xml, m["products"])
        check(f"{c}: nothing to add to the whole file", out == xml and added == [] and present == models)
        try:
            _, digest = kp.same_template(xml, tdir)
            check(f"{c}: the application carries the template of this tree ({digest})", True)
        except kp.ProductError as e:
            check(f"{c}: the application carries the template of this tree", False, str(e))


def foreign():
    print("products from a list, class switch")
    xml = load(os.path.join(ROOT, "producer", "zigbee-switch.xml"))
    new = [{"vendor": "Example", "model": "LED 12/3+X", "description": "a plug the table does not know",
            "zigbeeModel": ["Example plug"]},
           {"vendor": "IKEA", "model": "E2204-2", "description": "TRETAKT smart plug, another box",
            "zigbeeModel": ["TRETAKT Smart plug"]},
           {"vendor": "Example", "model": "P&Q <1>", "description": 'quotes " and \' and &'}]
    out, added, present, notes = kp.append_products(xml, new)
    check("three added, none was there", added == [p["model"] for p in new] and present == [], str(added))
    check("a note for the one model the template does not name, none for a listed one and none without a model name",
          len(notes) == 1 and notes[0].startswith("LED 12/3+X:") and "Example plug" in notes[0], str(notes))
    check("the order number is encoded into the ids the way OpenKNXproducer does",
          'Id="%HardwareId%_P-LED.2012.2F3.2BX"' in out and 'Id="%Hardware2ProgramId%_CI-LED.2012.2F3.2BX-1"' in out)
    check("texts with & < > and quotes stay well-formed", "P&amp;Q &lt;1&gt;" in out)
    n = len(PROD.findall(xml))
    try:
        kp.check(out, n + 3 - 1, re.search(r'ReplacesVersions="([^"]*)"', out).group(1))
        check("ids are unique and every catalog item points to its product", True)
    except kp.ProductError as e:
        check("ids are unique and every catalog item points to its product", False, str(e))
    check("application and Hardware2Program are the same text", kp.block(out, "ApplicationPrograms") ==
          kp.block(xml, "ApplicationPrograms") and kp.block(out, "Hardware2Programs") == kp.block(xml, "Hardware2Programs"))
    if PACK and os.path.exists(PACK):
        pk = packer()
        a, b = pk.split(xml, pk.TOOL)[2], pk.split(out, pk.TOOL)[2]
        app = [k for k in a if k not in ("Catalog.xml", "Hardware.xml")]
        check("the packer names the same three files", sorted(a) == sorted(b) and len(app) == 1, str(sorted(b)))
        check("the application file the ETS library signs is the same, byte for byte", a[app[0]] == b[app[0]])
        check("hardware and catalog hold three products more", b["Hardware.xml"].count(b"<Product ") == n + 3 and
              b["Catalog.xml"].count(b"<CatalogItem ") == n + 3 and a["Hardware.xml"].count(b"<Product ") == n)
    else:
        print("  skip: the packer's split step (KNXPROD_PACK not set)")
    out2, added2, present2, _ = kp.append_products(out, new)
    check("a second run adds nothing", out2 == out and added2 == [] and len(present2) == 3)

    print("what it refuses")
    raises("the same order number twice in the list", lambda: kp.append_products(xml, [new[0], new[0]]), "twice")
    raises("a product without a description", lambda: kp.append_products(xml, [{"vendor": "X", "model": "Y"}]), "needs the texts")
    raises("a product that is no object", lambda: kp.append_products(xml, ["E1603b"]), "needs the texts")
    raises("zigbeeModel that is no list", lambda: kp.append_products(
        xml, [{"vendor": "X", "model": "Y", "description": "Z", "zigbeeModel": "one"}]), "list of texts")
    raises("products that are no list", lambda: kp.append_products(xml, {"model": "Y"}), "are a list")
    raises("a file without an embedded template", lambda: kp.append_products(
        load(os.path.join(ROOT, "producer", "zigbee-gateway.xml")), new), "embedded template")
    broken = xml.replace("<Data>", "<Data>AAAA", 1)
    raises("a segment whose container is damaged", lambda: kp.append_products(broken, new), "embedded template")


def changed_template():
    print("the template of the directory against the one in the file")
    xml = load(os.path.join(ROOT, "producer", "zigbee-switch.xml"))
    with tempfile.TemporaryDirectory() as td:
        t = os.path.join(td, "zigbee-switch")
        shutil.copytree(os.path.join(ROOT, "templates", "zigbee-switch"), t)
        be = os.path.join(t, "zigbee-switch.be")
        src = load(be)
        with open(be, "w", encoding="utf-8") as f:
            f.write("# a comment line more\n" + src)
        try:
            kp.same_template(xml, t)
            check("a comment line more is the same template", True)
        except kp.ProductError as e:
            check("a comment line more is the same template", False, str(e))
        with open(be, "w", encoding="utf-8") as f:
            f.write(src + "var one_more = 1\n")
        raises("a code line more is another template: refused", lambda: kp.same_template(xml, t), "changed template")
        with open(be, "w", encoding="utf-8") as f:
            f.write(src)
        mp = os.path.join(t, "manifest.json")
        m = json.load(open(mp, encoding="utf-8"))
        m["products"].append({"vendor": "Example", "model": "NEW-1", "description": "one more box",
                              "zigbeeModel": ["TRETAKT Smart plug"]})
        json.dump(m, open(mp, "w", encoding="utf-8"))
        try:
            kp.same_template(xml, t)
            check("a product more in the manifest is the same template", True)
        except kp.ProductError as e:
            check("a product more in the manifest is the same template", False, str(e))
        m["match"]["zigbeeModel"].append("Example plug")
        json.dump(m, open(mp, "w", encoding="utf-8"))
        raises("a Zigbee model more in the class table is another template: refused",
               lambda: kp.same_template(xml, t), "changed template")
        m["match"]["zigbeeModel"].pop()
        m["knx"]["application"]["version"] += 1
        json.dump(m, open(mp, "w", encoding="utf-8"))
        raises("a manifest of another application version: refused", lambda: kp.same_template(xml, t), "version")


def command_line():
    print("the command")
    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, "in.xml")
        shutil.copy(os.path.join(ROOT, "producer", "zigbee-switch.xml"), src)
        lst = os.path.join(td, "more.json")
        json.dump([{"vendor": "Example", "model": "EX-1", "description": "one more plug"}], open(lst, "w"))
        out = os.path.join(td, "out.xml")
        r = subprocess.run([sys.executable, TOOL, "--append", src, "--products", lst, "-o", out], capture_output=True, text=True)
        check("--append with a list writes the file and says what it did", r.returncode == 0 and os.path.exists(out) and
              "1 products added (EX-1)" in r.stdout and "untouched" in r.stdout, r.stdout + r.stderr)
        check("the input is left alone when -o names another file", load(src) == load(os.path.join(ROOT, "producer", "zigbee-switch.xml")))
        r = subprocess.run([sys.executable, TOOL, "--append", out, "--products", lst], capture_output=True, text=True)
        check("a second run says there is nothing to add", r.returncode == 0 and "nothing to add" in r.stdout, r.stdout + r.stderr)
        t = os.path.join(td, "zigbee-switch")
        shutil.copytree(os.path.join(ROOT, "templates", "zigbee-switch"), t)
        mp = os.path.join(t, "manifest.json")
        m = json.load(open(mp, encoding="utf-8"))
        m["products"].append({"vendor": "Example", "model": "EX-2", "description": "from the manifest",
                              "zigbeeModel": ["Example plug"]})
        json.dump(m, open(mp, "w", encoding="utf-8"))
        r = subprocess.run([sys.executable, TOOL, "--append", src, t], capture_output=True, text=True)
        check("--append with a template directory adds the manifest's new product in place, with the note on its model",
              r.returncode == 0 and "1 products added (EX-2)" in r.stdout and "note: EX-2:" in r.stdout and
              'OrderNumber="EX-2"' in load(src), r.stdout + r.stderr)
        r = subprocess.run([sys.executable, TOOL, "--append", src, t, "--products", lst], capture_output=True, text=True)
        check("both sources at once: refused", r.returncode != 0 and "one of them" in r.stderr, r.stdout + r.stderr)
        r = subprocess.run([sys.executable, TOOL, "--append", src], capture_output=True, text=True)
        check("no source: refused", r.returncode != 0 and "one of them" in r.stderr, r.stdout + r.stderr)
        r = subprocess.run([sys.executable, TOOL, "--append", os.path.join(td, "none.xml"), "--products", lst],
                           capture_output=True, text=True)
        check("a file that is not there: an error line, no traceback", r.returncode != 0 and "Traceback" not in r.stderr and
              "knxprod_products:" in r.stderr, r.stderr)


if __name__ == "__main__":
    classes()
    foreign()
    changed_template()
    command_line()
    print(f"{count} checks, {failed} failed")
    sys.exit(1 if failed else 0)
