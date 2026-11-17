#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dirk Tostmann
"""The producer inputs of this tree against their manifests and templates and against the versions given out.

  python3 tests/test_issued.py

1. producer/issued.json: every version given out, well formed.
2. Each producer/<class>.xml against templates/<class>/manifest.json and its Berry: application, objects, parameters
   and their types, the parameter segment with its defaults, the embedded template (digest, template version equal to
   the application version), hardware, products, catalog sections.
3. Each producer input against the versions given out (tools/knxprod_gen.py issued_problems): ReplacesVersions names
   every lower version given out, object numbers and parameter ids keep their meaning, a version given out is not
   generated again with other content. Then the same check on altered copies, which it has to refuse.
4. The catalog: one section Zigbee2KNX in every file, a subsection per kind of device, ids as the manufacturers'
   product files build them.
5. The gateway's parameter LongFrames.
Optional, when KNXPROD_PACK (a knxprod packer with split()) and KNX_XSD (the KNX project schema) are set: the three
product files the packer makes of each input are valid against the schema.

The embedded templates must verify with the key that signed them. Whether that key is one the firmware trusts
(tools/kgt_trust.json) is reported; with KGT_TRUSTED_ONLY=1 an unknown key is a failure (for release builds).
"""
import base64
import importlib.util
import json
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import kgt  # noqa: E402
import knxprod_gen as gen  # noqa: E402

NS = "{http://knx.org/xml/project/20}"
FILES = {  # producer input: template directory, catalog subsection (number, name)
    "zigbee-gateway.xml": ("zigbee-gateway", ("Gateway", "Gateway")),
    "zigbee-dimmable-light.xml": ("zigbee-dimmable-light", ("Lights", "Lights")),
    "zigbee-white-spectrum-light.xml": ("zigbee-white-spectrum-light", ("Lights", "Lights")),
    "zigbee-switch.xml": ("zigbee-switch", ("PlugsSwitches", "Plugs and switches")),
    "zigbee-remote.xml": ("zigbee-remote", ("Remotes", "Remotes")),
}
FLAGS = (("R", "ReadFlag"), ("W", "WriteFlag"), ("C", "CommunicationFlag"), ("T", "TransmitFlag"),
         ("U", "UpdateFlag"), ("I", "ReadOnInitFlag"))
failed = 0
count = 0


def check(what, ok, detail=""):
    global failed, count
    count += 1
    if not ok:
        failed += 1
    print(f"  {'ok  ' if ok else 'FAIL'} {what}" + (f": {detail}" if detail and not ok else ""))


def load(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def issued_list():
    print("producer/issued.json")
    issued = json.loads(load(os.path.join(ROOT, "producer", "issued.json")))
    apps = issued["applications"]
    keys = [(e["number"], e["version"]) for e in apps]
    check("every version once, sorted by number and version", keys == sorted(set(keys)), keys)
    for e in apps:
        ok = re.fullmatch(rf"M-00FA_A-{e['number']:04X}-{e['version']:02X}-[0-9A-F]{{4}}", e["id"]) is not None
        ok = ok and re.fullmatch(r"[0-9a-f]{64}", e["application_sha256"]) is not None
        ok = ok and all(p[2] in e["parameter_types"] for p in e["parameters"].values())
        ok = ok and (e["template"] is None) == (e["number"] == 0xC000)
        check(f"{e['id']}: id, hash, types, template", ok, e["id"])
    given = {(n, v) for n, v in keys}
    for n, v in ((0xC000, 1), (0xC000, 2), (0xC000, 3), (0xC001, 2), (0xC001, 3), (0xC002, 2), (0xC002, 3), (0xC002, 4),
                 (0xC003, 1), (0xC003, 2), (0xC003, 3), (0xC004, 1), (0xC004, 2)):
        check(f"0x{n:04X} version {v} is listed as given out", (n, v) in given)
    return issued


def child(e, tag):
    return e.find(NS + tag)


def against_manifest(name, xml, tdir, trust):
    m = json.loads(load(os.path.join(tdir, "manifest.json")))
    knx = m["knx"]
    root = ET.fromstring(xml)
    prog = next(root.iter(NS + "ApplicationProgram"))
    app = knx["application"]
    rv = " ".join(str(v) for v in app.get("replaces", [])) or "0"
    check(f"{name}: application number, version, name, mask, ReplacesVersions as in the manifest",
          (prog.get("ApplicationNumber"), prog.get("ApplicationVersion"), prog.get("Name"), prog.get("MaskVersion"),
           prog.get("ReplacesVersions")) == (str(app["number"]), str(app["version"]), app["name"], app["mask"], rv),
          prog.attrib)
    check(f"{name}: template version = application version", m["version"] == app["version"], m["version"])
    objs = list(root.iter(NS + "ComObject"))
    want = []
    for o in knx["objects"]:
        a = {"Id": f"%AID%_O-{o['number']}", "Name": o["name"], "Text": o["text"], "Number": str(o["number"]),
             "FunctionText": o["function"], "ObjectSize": o["size"], "DatapointType": gen.dpst(o["dpt"])}
        a.update({attr: "Enabled" if f in o["flags"] else "Disabled" for f, attr in FLAGS})
        want.append(a)
    check(f"{name}: {len(want)} objects as in the manifest", [dict(o.attrib) for o in objs] == want,
          [o.attrib.get("Id") for o in objs])
    refs = [r.get("Id") for r in root.iter(NS + "ComObjectRef")]
    check(f"{name}: a ref per object", refs == [f"%AID%_O-{o['number']}_R-{o['number']}" for o in knx["objects"]], refs)
    types = {}
    for pt in root.iter(NS + "ParameterType"):
        k = pt[0]
        if k.tag == NS + "TypeNumber":
            types[pt.get("Name")] = {"bits": int(k.get("SizeInBit")), "number": {
                "type": k.get("Type"), "min": int(k.get("minInclusive")), "max": int(k.get("maxInclusive"))}}
        elif k.tag == NS + "TypeRestriction":
            types[pt.get("Name")] = {"bits": int(k.get("SizeInBit")),
                                     "enum": [[int(e.get("Value")), e.get("Text")] for e in k]}
    check(f"{name}: parameter types as in the manifest", types == knx["parameter_types"], sorted(types))
    params = list(root.iter(NS + "Parameter"))
    got = [(p.get("Id"), p.get("Name"), p.get("Text"), p.get("ParameterType"), p.get("Value"),
            child(p, "Memory").get("Offset")) for p in params]
    want = [(f"%AID%_P-{i}", p["name"], p["text"], f"%AID%_PT-{p['type']}", str(p["default"]), str(p["offset"]))
            for i, p in enumerate(knx["parameters"]["list"], 1)]
    check(f"{name}: {len(want)} parameters in the order of the manifest (P-n), offsets, defaults", got == want,
          [g for g, w in zip(got, want) if g != w][:2] or (len(got), len(want)))
    seg = next(root.iter(NS + "RelativeSegment"))
    size = int(seg.get("Size"))
    p_len = knx["parameters"]["segment_size"]
    img = gen.image(xml)
    defaults = bytearray(p_len)
    for p in knx["parameters"]["list"]:
        n = (knx["parameter_types"][p["type"]]["bits"] + 7) // 8
        defaults[p["offset"]:p["offset"] + n] = int(p["default"]).to_bytes(n, "big")
    check(f"{name}: the segment ETS writes starts with the {p_len} parameter octets of the defaults",
          img[:p_len] == bytes(defaults) and len(img) == size, img[:p_len].hex())
    sizes = {int(x.get("Size")) for x in root.iter(NS + "LdCtrlRelSegment")} | \
        {int(x.get("Size")) for x in root.iter(NS + "LdCtrlWriteRelMem")}
    check(f"{name}: load procedures use the segment size {size}", sizes == {size}, sizes)
    behaviour = knx.get("embed")
    if behaviour:
        data = child(seg, "Data")
        mask = child(seg, "Mask")
        check(f"{name}: Data and Mask fill the segment, Mask all FF", data is not None and mask is not None and
              len(base64.b64decode(data.text)) == size and base64.b64decode(mask.text) == b"\xff" * size)
        src = load(os.path.join(tdir, m["behaviour"]["file"]))
        payload, digest, fpr = gen.embedded_template(xml)
        check(f"{name}: the embedded template is the one of the tree ({digest})",
              digest == kgt.digest(kgt.payload_json(m, src)) and payload["version"] == app["version"] and
              payload["id"] == m["id"], (digest, payload.get("version")))
        trailer = img[-kgt.TRAILER_LEN:].decode()
        off, length = int(trailer[4:8], 16), int(trailer[8:12], 16)
        blob = base64.b64decode(img[off:off + length])
        try:
            kgt.unpack(blob, trust)
            print(f"  note {name}: signed by {fpr}, a key in tools/kgt_trust.json")
            known = True
        except kgt.KgtError as e:
            known = False
            print(f"  note {name}: {e} (tools/kgt_trust.json)")
        if os.environ.get("KGT_TRUSTED_ONLY") == "1":
            check(f"{name}: signed by a key the firmware trusts", known, fpr)
        tail = img[p_len:off]
        check(f"{name}: between the parameters and the template nothing", tail == b"\x00" * len(tail), tail[:8].hex())
    hw = next(e for e in root.iter(NS + "Hardware") if e.get("SerialNumber") is not None)
    h = knx["hardware"]
    want = {"Name": h["name"], "SerialNumber": h["serial_number"], "VersionNumber": str(h["version"]),
            "BusCurrent": str(h["bus_current_ma"])}
    if h.get("nonreg_version"):
        want["NonRegRelevantDataVersion"] = str(h["nonreg_version"])
    got = {k: hw.get(k) for k in ("Name", "SerialNumber", "VersionNumber", "BusCurrent", "NonRegRelevantDataVersion")
           if hw.get(k) is not None}
    check(f"{name}: hardware as in the manifest", got == want, got)
    prods = list(root.iter(NS + "Product"))
    orders = [p.get("OrderNumber") for p in prods]
    check(f"{name}: the class's own product and one per model of the manifest ({len(prods)})",
          orders == [h["order_number"]] + [p["model"] for p in m.get("products", [])], orders[:4])
    h2p = next(root.iter(NS + "Hardware2Program"))
    reg = child(h2p, "RegistrationInfo").get("RegistrationNumber")
    check(f"{name}: registration number 0001/<hardware version><application version>",
          reg == f"0001/%HardwareVersionEncoded%{app['version']}", reg)
    secs = [(s.get("Id"), s.get("Name"), s.get("Number")) for s in root.iter(NS + "CatalogSection")]
    check(f"{name}: catalog sections as in the manifest", secs == gen.catalog_sections(knx["catalog"], knx["manufacturer"]),
          secs)
    return m, secs


def against_issued(issued):
    print("against the versions given out")
    for name in FILES:
        xml = load(os.path.join(ROOT, "producer", name))
        problems = gen.issued_problems(xml, issued)
        mn = gen.application_meaning(xml)
        check(f"{name}: 0x{mn['number']:04X} version {mn['version']} replaces {mn['replaces']}, ids kept", problems == [],
              "; ".join(problems))
    print("what the check refuses")
    ws = load(os.path.join(ROOT, "producer", "zigbee-white-spectrum-light.xml"))
    shifted = ws.replace('Name="CtMinimum"', 'Name="CtNewParameter"', 1)
    check("a parameter id of a version given out with another meaning (P-11)",
          any("P-11" in p for p in gen.issued_problems(shifted, issued)), "")
    nxt = re.sub(r'ApplicationVersion="(\d+)" ReplacesVersions="[^"]*"',
                 lambda m: f'ApplicationVersion="{int(m.group(1)) + 1}" ReplacesVersions="{m.group(1)}"', ws, count=1)
    check("ReplacesVersions without a version given out (the next version names only the last one)",
          nxt != ws and any("ReplacesVersions" in p for p in gen.issued_problems(nxt, issued)), "")
    dim = load(os.path.join(ROOT, "producer", "zigbee-dimmable-light.xml"))
    check("an object of a version given out with another datapoint type",
          any("object 2" in p for p in gen.issued_problems(dim.replace('DatapointType="DPST-3-7"', 'DatapointType="DPST-5-1"',
                                                                        1), issued)), "")
    narrowed = re.sub(r'(<ParameterType Id="%AID%_PT-Percent" Name="Percent">\s*<TypeNumber [^>]*minInclusive=")1"',
                      r'\g<1>5"', dim, count=1)
    check("a parameter type of a version given out that holds fewer values",
          narrowed != dim and any("Percent" in p for p in gen.issued_problems(narrowed, issued)), "")
    sw = load(os.path.join(ROOT, "producer", "zigbee-switch.xml"))
    v = gen.application_meaning(sw)["version"]
    again = sw.replace(f'ApplicationVersion="{v}"', 'ApplicationVersion="2"', 1)
    again = re.sub(r'ReplacesVersions="[^"]*"', 'ReplacesVersions="1"', again, count=1)
    check("a version given out (0xC003 version 2) generated again with other content",
          any("has been given out" in p for p in gen.issued_problems(again, issued)), "")
    gw = load(os.path.join(ROOT, "producer", "zigbee-gateway.xml"))
    check("the gateway without one of its objects of version 2",
          any("object 12" in p for p in gen.issued_problems(re.sub(r'\s*<ComObject Id="%AID%_O-12"[^>]*/>', "", gw, count=1),
                                                                  issued)), "")


def catalog(found):
    print("catalog")
    tops = {s[0] for s in found.values()}
    check("one top section in every file: M-00FA_CS-Zigbee2KNX, Zigbee2KNX",
          tops == {("M-00FA_CS-Zigbee2KNX", "Zigbee2KNX", "Zigbee2KNX")}, tops)
    by_id = {}
    for name, secs in found.items():
        want = FILES[name][1]
        check(f"{name}: in Zigbee2KNX > {want[1]}", len(secs) == 2 and secs[1] == (
            f"M-00FA_CS-Zigbee2KNX-{want[0]}", want[1], want[0]), secs)
        for s in secs:
            by_id.setdefault(s[0], set()).add(s[1:])
    check("a section id has one name and number in all files", all(len(v) == 1 for v in by_id.values()), by_id)
    check("four subsections: Gateway, Lights, Plugs and switches, Remotes", sorted(
        n for i, v in by_id.items() for n, _ in v if i != "M-00FA_CS-Zigbee2KNX") ==
        ["Gateway", "Lights", "Plugs and switches", "Remotes"], by_id)


def gateway():
    print("gateway parameter LongFrames")
    xml = load(os.path.join(ROOT, "producer", "zigbee-gateway.xml"))
    mn = gen.application_meaning(xml)
    check("0xC000 version 3, replaces 1 and 2", (mn["number"], mn["version"], mn["replaces"]) == (0xC000, 3, [1, 2]),
          (mn["number"], mn["version"], mn["replaces"]))
    check("P-5 LongFrames at octet 4", mn["parameters"].get("5") == ["LongFrames", 4, "LongFrames"], mn["parameters"])
    check("enumeration: 0 standard frames only, 1 long frames of 55 octets, 2 of 66",
          mn["parameter_types"].get("LongFrames") == ["enum", 8, [[0, "Standard frames only"], [1, "Long frames, 55 octets"],
                                                                 [2, "Long frames, 66 octets"]]], mn["parameter_types"])
    seg = gen.image(xml)
    check("parameter segment of 5 octets, LongFrames 1 by default", len(seg) == 5 and seg[4] == 1, seg.hex())
    p = next((p for p in ET.fromstring(xml).iter(NS + "Parameter") if p.get("Name") == "LongFrames"), None)
    check("its text: Long frames for downloads", p is not None and p.get("Text") == "Long frames for downloads",
          None if p is None else p.get("Text"))


def record():
    print("the entry of a new version (--issued-record)")
    gen_py = os.path.join(ROOT, "tools", "knxprod_gen.py")
    path = os.path.join(ROOT, "producer", "zigbee-switch.xml")
    mn = gen.application_meaning(load(path))
    aid = f"M-00FA_A-C003-{mn['version']:02X}-1A2B"
    r = subprocess.run([sys.executable, "-B", gen_py, "--issued-record", path, aid], capture_output=True, text=True)
    ok = r.returncode == 0
    rec = json.loads(r.stdout) if ok else {}
    check("prints the entry: id, template, objects, parameters", ok and rec["id"] == aid and
          rec["template"]["version"] == mn["version"] and rec["objects"] == mn["objects"] and
          rec["application_sha256"] == mn["sha256"], r.stderr)
    r = subprocess.run([sys.executable, "-B", gen_py, "--issued-record", path, "M-00FA_A-C002-03-1A2B"],
                       capture_output=True, text=True)
    check("refuses the id of another application", r.returncode != 0 and "not an id of application 0xC003" in r.stderr,
          r.stderr)


def schema():
    pack, xsd = os.environ.get("KNXPROD_PACK", ""), os.environ.get("KNX_XSD", "")
    if not (pack and xsd and os.path.exists(pack) and os.path.exists(xsd)):
        print("  skip: schema check (KNXPROD_PACK and KNX_XSD not set)")
        return
    print("the product files against the KNX schema")
    from lxml import etree
    schema = etree.XMLSchema(etree.parse(xsd))
    spec = importlib.util.spec_from_file_location("knxprod_pack_shared", pack)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    for name in FILES:
        _, _, files = mod.split(load(os.path.join(ROOT, "producer", name)), ("OpenKNXproducer", "4.3.12.0"))
        bad = []
        for f, data in files.items():
            doc = etree.fromstring(data)
            if not schema.validate(doc):
                bad.append(f"{f}: {schema.error_log.last_error}")
        check(f"{name}: {', '.join(sorted(files))} valid", not bad, "; ".join(bad))


def main():
    issued = issued_list()
    trust = kgt.load_trust(os.path.join(ROOT, "tools", "kgt_trust.json"))
    found = {}
    for name, (tdir, _) in FILES.items():
        print(name)
        _, secs = against_manifest(name, load(os.path.join(ROOT, "producer", name)), os.path.join(ROOT, "templates", tdir),
                                   trust)
        found[name] = secs
    against_issued(issued)
    catalog(found)
    gateway()
    record()
    schema()
    print(f"{count} checks, {failed} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
