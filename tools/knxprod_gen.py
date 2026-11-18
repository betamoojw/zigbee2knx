#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dirk Tostmann
"""knxprod_gen — the ETS product of a device template, generated from its manifest.json.

Writes the input of OpenKNXproducer (`create`, on a Windows PC with ETS, turns it into the signed .knxprod):
the application program of one virtual device (mask MV-07B0, System B) with its parameter segment
(LSM 4), objects, parameter types and one parameter block, plus catalog and hardware. The load procedure
is the one that passed ETS6 on 2026-11-10.

  knxprod_gen.py TEMPLATE_DIR [-o OUT.xml]          write (atomic) or print the producer input
  knxprod_gen.py TEMPLATE_DIR --check REF.xml       compare with a reference, ignoring comments and layout
  knxprod_gen.py TEMPLATE_DIR --image OUT.bin       the parameter segment as ETS writes it from the defaults
  options for a test variant of the embedding: --embed-carrier text|rawdata|data|inline,
  --embed-access none|unreferenced|read (carrier text only), --app-number N, --app-name TEXT (so several
  variants can be imported into ETS side by side)
  --sign-key PEM   key that signs the embedded template (default: $ZIGBEE2KNX_SIGN_KEY); --unsigned for tests

Embedding (manifest knx.embed, tools/kgt.py): the template itself travels in the parameter segment, behind
the application's parameters, and a 16-character trailer says where. What carries it there is the carrier:
  text     hidden text parameters of 30 characters each (the first carrier)
  data     the static image of the segment, <Data> and <Mask> in the RelativeSegment (as Zennio and Siemens
           ship application data in LSM 4 of 07B0 devices)
  rawdata  one hidden TypeRawData parameter (as ABB and Siemens ship blobs); ETS puts 4 bytes in front
  inline   InlineData of a second LdCtrlWriteRelMem (the schema allows it, no product in the corpus does it)
text, data and inline give the same bytes in the device. Every generated embedding is checked here: the
segment ETS would write from the defaults is computed from the generated XML and unpacked again
(`kgt.extract`), and it must give back exactly the packed template.

Ids follow the producer's conventions (%AID%, P-n with n = position in the parameter list, O-n with
n = object number, one ref per parameter and object: P-n_R-n, O-n_R-n). Catalog sections nest as the manifest's
knx.catalog.sections lists them, outermost first; their ids follow the manufacturers' product files: the outermost
<manufacturer>_CS-<Number>, every inner one <id of its parent>-<Number>. ETS merges sections of the same id across
product files, so a section keeps its number and name in every file.

Versions given out (producer/issued.json): an application that has been imported into an ETS keeps its ids and
their meaning in every later version, a later version replaces all lower versions given out, and a version given out
is never generated again with other content. issued_problems() checks a producer input against that list, and -o
never writes a version given out with other content, whatever the switches; --issued-record prints the entry of
a version after its import, for that list.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kgt  # noqa: E402

NS = "http://knx.org/xml/project/20"
ISSUED = os.path.join(ROOT, "producer", "issued.json")
ORDER_MAX = 50                    # OrderNumber is String50_t in the KNX schema
SECTION_NUMBER = re.compile(r"[A-Za-z0-9]{1,20}")   # CatalogSection Number is String20_t; kept to what an id holds as is
FLAGS = (("R", "ReadFlag"), ("W", "WriteFlag"), ("C", "CommunicationFlag"), ("T", "TransmitFlag"),
         ("U", "UpdateFlag"), ("I", "ReadOnInitFlag"))
SEG_ID = "%AID%_RS-04-00000"
TRAILER_CHARS = kgt.TRAILER_LEN
CARRIERS = ("text", "rawdata", "data", "inline")
RAW_HEAD = 4        # bytes ETS keeps in front of a TypeRawData value: in the ABB i-bus product of the corpus the
                    # parameters lie MaxSize + 4 apart. Taken as the length, big endian; not seen on a device yet.
SIGN_KEY = os.environ.get("ZIGBEE2KNX_SIGN_KEY", "")   # PEM private key (P-256) that signs the embedded templates
SIGN_CMD = os.environ.get("ZIGBEE2KNX_SIGN_CMD", "")   # or an external signer: digest hex on stdin, r||s hex on stdout
SIGN_FPR = os.environ.get("ZIGBEE2KNX_SIGN_FPR", "")   # its key, looked up by fingerprint in tools/kgt_trust.json
TRUST_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "kgt_trust.json")


class GenError(Exception):
    pass


APP_RE = re.compile(r'<ApplicationProgram [^>]*?ApplicationNumber="(\d+)" ApplicationVersion="(\d+)".*?</ApplicationProgram>',
                    re.S)


def version_clash(old_xml: str, new_xml: str) -> str | None:
    """A message if new_xml carries an application with the number and version of one in old_xml but other content.

    The ETS signing library derives an application's id from its content, so such a file would make a second
    application under the same number and version, and whoever has the first one in a project could not update to
    it. Catalog products may differ; they are not part of the application.
    """
    old = {(m.group(1), m.group(2)): m.group(0) for m in APP_RE.finditer(old_xml)}
    for m in APP_RE.finditer(new_xml):
        prev = old.get((m.group(1), m.group(2)))
        if prev is not None and prev != m.group(0):
            return (f"application 0x{int(m.group(1)):04X} version {m.group(2)} exists in the output with other "
                    f"content; raise knx.application.version, or pass --overwrite-version if that version was never "
                    f"given out")
    return None


def load_issued(path: str = ISSUED) -> dict:
    """The versions given out (producer/issued.json); none where the file is missing."""
    if not os.path.exists(path):
        return {"applications": [], "history": []}
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def issued_below(issued: dict, number: int, version: int) -> list[int]:
    """The versions of an application given out below `version`: what its ReplacesVersions has to name."""
    return sorted(e["version"] for e in issued.get("applications", []) if e["number"] == number and e["version"] < version)


def application_meaning(xml_text: str) -> dict:
    """What the ids of the application in a producer input stand for. Objects by number: [Name, ObjectSize,
    DatapointType]; parameters by the n of their id P-n: [Name, offset in the segment, parameter type]; parameter
    types by name: [kind, bits, values or range]. Also number, version, ReplacesVersions, the ids of the refs and the
    SHA-256 of the ApplicationProgram element as the file writes it."""
    m = APP_RE.search(xml_text)
    if not m:
        raise GenError("no ApplicationProgram in the file")
    root = ET.fromstring(xml_text)
    q = lambda tag: f"{{{NS}}}{tag}"                                       # noqa: E731
    prog = next(root.iter(q("ApplicationProgram")))
    aid = prog.get("Id")

    def local(ident: str, prefix: str) -> str:
        if not ident.startswith(f"{aid}_{prefix}"):
            raise GenError(f"id {ident}: not {aid}_{prefix}…")
        return ident[len(aid) + 1 + len(prefix):]

    types = {}
    for pt in root.iter(q("ParameterType")):
        kind = pt[0]
        tag = kind.tag.split("}")[1]
        if tag == "TypeRestriction":
            t = ["enum", int(kind.get("SizeInBit")), [[int(e.get("Value")), e.get("Text")] for e in kind.iter(q("Enumeration"))]]
        elif tag == "TypeNumber":
            t = ["number", int(kind.get("SizeInBit")), kind.get("Type"), int(kind.get("minInclusive")),
                 int(kind.get("maxInclusive"))]
        elif tag == "TypeText":
            t = ["text", int(kind.get("SizeInBit"))]
        elif tag == "TypeRawData":
            t = ["raw", int(kind.get("MaxSize")) * 8]
        else:
            t = [tag, 0]
        types[local(pt.get("Id"), "PT-")] = t
    params = {}
    for p in root.iter(q("Parameter")):
        mem = p.find(q("Memory"))
        params[local(p.get("Id"), "P-")] = [p.get("Name"), int(mem.get("Offset")) if mem is not None else None,
                                            local(p.get("ParameterType"), "PT-")]
    objects = {}
    for o in root.iter(q("ComObject")):
        n = local(o.get("Id"), "O-")
        if n != str(int(o.get("Number"))):
            raise GenError(f"object {o.get('Id')} has the number {o.get('Number')}")
        objects[n] = [o.get("Name"), o.get("ObjectSize"), o.get("DatapointType")]
    refs = sorted(local(r.get("Id"), "") for tag in ("ParameterRef", "ComObjectRef") for r in root.iter(q(tag)))
    rv = [int(v) for v in prog.get("ReplacesVersions", "0").split() if int(v) != 0]
    man = next(root.iter(q("Manufacturer"))).get("RefId")
    return {"manufacturer": man, "number": int(m.group(1)), "version": int(m.group(2)), "replaces": rv,
            "sha256": hashlib.sha256(m.group(0).encode("utf-8")).hexdigest(), "objects": objects, "parameters": params,
            "parameter_types": types, "refs": refs}


def _type_keeps(old: list, new: list | None) -> bool:
    """A parameter type of a later version still holds every value the earlier one could hold, with the same meaning."""
    if new is None or old[:2] != new[:2]:
        return False
    if old[0] == "enum":
        return all(v in new[2] for v in old[2])
    if old[0] == "number":
        return old[2] == new[2] and new[3] <= old[3] and new[4] >= old[4]
    return old == new


def issued_problems(xml_text: str, issued: dict | None = None) -> list[str]:
    """What keeps this producer input from going out next to the versions given out (producer/issued.json): a version
    given out, generated again with other content; ReplacesVersions that does not name every lower version given out;
    an object or parameter id of such a version that is gone or stands for something else. Empty: nothing."""
    issued = load_issued() if issued is None else issued
    mn = application_meaning(xml_text)
    num, ver = mn["number"], mn["version"]
    out = []
    same = [e for e in issued.get("applications", []) if e["number"] == num and e["version"] == ver]
    if same and same[0]["application_sha256"] != mn["sha256"]:
        out.append(f"application 0x{num:04X} version {ver} has been given out ({same[0]['id']}); this file holds other "
                   f"content under that number and version")
    want = issued_below(issued, num, ver)
    if not same and mn["replaces"] != want:
        out.append(f"ReplacesVersions names {mn['replaces'] or 'none'}; versions of 0x{num:04X} given out below {ver}: "
                   f"{want or 'none'}")
    for e in issued.get("applications", []):
        if e["number"] != num or e["version"] >= ver:
            continue
        for n, old in e["objects"].items():
            new = mn["objects"].get(n)
            if new != old:
                out.append(f"object {n} {old} of {e['id']}: {new or 'missing'} here")
            if f"O-{n}_R-{n}" not in mn["refs"]:
                out.append(f"object {n} of {e['id']}: no ref O-{n}_R-{n} here")
        for n, old in e["parameters"].items():
            new = mn["parameters"].get(n)
            if new != old:
                out.append(f"parameter P-{n} {old} of {e['id']}: {new or 'missing'} here")
            elif not _type_keeps(e["parameter_types"][old[2]], mn["parameter_types"].get(old[2])):
                out.append(f"parameter type {old[2]} of {e['id']} {e['parameter_types'][old[2]]}: "
                           f"{mn['parameter_types'].get(old[2])} here")
            if f"P-{n}_R-{n}" not in mn["refs"]:
                out.append(f"parameter P-{n} of {e['id']}: no ref P-{n}_R-{n} here")
    return out


def embedded_template(xml_text: str) -> tuple[dict, str, str | None] | None:
    """(payload, digest, signing key fingerprint) of the template in the parameter segment of a producer input, the
    signature not checked; None for a product without one."""
    seg = image(xml_text)
    if len(seg) < kgt.TRAILER_LEN or seg[-kgt.TRAILER_LEN:-kgt.TRAILER_LEN + 4] != kgt.TRAILER_MAGIC:
        return None
    t = seg[-kgt.TRAILER_LEN:].decode("ascii", "replace")
    if not re.fullmatch(r"KGTE[0-9A-F]{12}", t):
        raise GenError(f"trailer {t!r} malformed")
    off, length = int(t[4:8], 16), int(t[8:12], 16)
    return kgt.peek(base64.b64decode(seg[off:off + length], validate=True))


def issued_record(xml_text: str, app_id: str) -> dict:
    """The entry of producer/issued.json for the application of this producer input, imported into ETS as app_id (the id
    with the fingerprint the ETS signing gave it)."""
    mn = application_meaning(xml_text)
    if not re.fullmatch(rf"{mn['manufacturer']}_A-{mn['number']:04X}-{mn['version']:02X}-[0-9A-F]{{4}}", app_id):
        raise GenError(f"{app_id}: not an id of application 0x{mn['number']:04X} version {mn['version']}")
    emb = embedded_template(xml_text)
    tpl = None if emb is None else {"id": emb[0]["id"], "version": emb[0]["version"], "digest": emb[1]}
    return {"number": mn["number"], "version": mn["version"], "id": app_id, "template": tpl,
            "application_sha256": mn["sha256"], "objects": mn["objects"], "parameters": mn["parameters"],
            "parameter_types": mn["parameter_types"]}


def dpst(dpt: str) -> str:
    """"1.001" -> "DPST-1-1"."""
    main, _, sub = dpt.partition(".")
    if not (main.isdigit() and sub.isdigit()):
        raise GenError(f"datapoint type {dpt!r}: expected main.sub, e.g. 1.001")
    return f"DPST-{int(main)}-{int(sub)}"


def sub(parent, tag, **attrs):
    return ET.SubElement(parent, tag, {k: str(v) for k, v in attrs.items()})


def catalog_sections(cat: dict, mfr: str) -> list[tuple[str, str, str]]:
    """(id, name, number) of the catalog sections, outermost first: knx.catalog.sections, or the one section that
    knx.catalog.section and section_number name in manifests from before nested sections."""
    secs = cat.get("sections") or [{"number": cat["section_number"], "name": cat["section"]}]
    out, sid = [], None
    for s in secs:
        num = str(s["number"])
        if not SECTION_NUMBER.fullmatch(num):
            raise GenError(f"catalog section number {num!r}: 1 to 20 letters and digits")
        if not s.get("name"):
            raise GenError(f"catalog section {num}: no name")
        sid = f"{mfr}_CS-{num}" if sid is None else f"{sid}-{num}"
        out.append((sid, s["name"], num))
    return out


def check_manifest(m: dict):
    knx = m["knx"]
    order = knx["hardware"]["order_number"]
    if not order or len(order) > ORDER_MAX or order != order.strip() or "%" in order:
        raise GenError(f"order number {order!r}: 1 to {ORDER_MAX} characters, no blanks at the ends, no '%'")
    catalog_sections(knx["catalog"], knx["manufacturer"])
    numbers = [o["number"] for o in knx["objects"]]
    if len(set(numbers)) != len(numbers):
        raise GenError("object numbers repeat")
    for o in knx["objects"]:
        if set(o["flags"]) - set("CRWTUI"):
            raise GenError(f"object {o['number']}: unknown flag in {o['flags']!r}")
    params = knx["parameters"]
    size = params["segment_size"]
    types = knx["parameter_types"]
    used = set()
    for p in params["list"]:
        t = types.get(p["type"])
        if t is None:
            raise GenError(f"parameter {p['name']}: unknown type {p['type']!r}")
        octets = (t["bits"] + 7) // 8
        if not (0 <= p["offset"] and p["offset"] + octets <= size):
            raise GenError(f"parameter {p['name']}: offset {p['offset']} + {octets} outside the {size}-byte segment")
        span = set(range(p["offset"], p["offset"] + octets))
        if span & used:
            raise GenError(f"parameter {p['name']}: overlaps another parameter")
        used |= span
        if "enum" in t and p["default"] not in [v for v, _ in t["enum"]]:
            raise GenError(f"parameter {p['name']}: default {p['default']} not in its enumeration")
        if "number" in t and not t["number"]["min"] <= p["default"] <= t["number"]["max"]:
            raise GenError(f"parameter {p['name']}: default {p['default']} outside its range")


def embedding(m: dict, behaviour_src: str | None, access: str | None, signer=None,
              carrier: str | None = None) -> dict | None:
    """What carries the template in the product, or None if the manifest embeds nothing."""
    e = m["knx"].get("embed")
    if not e:
        return None
    if behaviour_src is None:
        raise GenError("knx.embed needs the behaviour file")
    chunk = int(e.get("chunk_chars", 30))
    access = access or e.get("access", "none")
    if access not in ("none", "unreferenced", "read"):
        raise GenError(f"knx.embed.access {access!r}: none, unreferenced or read")
    carrier = carrier or e.get("carrier", "text")
    if carrier not in CARRIERS:
        raise GenError(f"knx.embed.carrier {carrier!r}: one of {', '.join(CARRIERS)}")
    params_len = m["knx"]["parameters"]["segment_size"]
    blob = kgt.pack(m, behaviour_src, signer)
    text = kgt.text_of(blob)
    lay = kgt.layout(params_len, len(text), chunk)
    if carrier == "rawdata":                      # the value starts behind the 4 bytes ETS puts in front
        lay = dict(lay, text_off=lay["text_off"] + RAW_HEAD, trailer_off=lay["trailer_off"] + RAW_HEAD,
                   segment_size=lay["segment_size"] + RAW_HEAD)
    if lay["segment_size"] > 0xFFFF:
        raise GenError(f"segment of {lay['segment_size']} bytes: the trailer counts in 16 bits")
    chunks = [text[i:i + chunk] for i in range(0, len(text), chunk)]
    trailer = kgt.trailer(lay["text_off"], len(text), params_len)
    # text, padding and trailer as they lie in the device, from text_off to the end of the segment
    tail = text.encode("ascii").ljust(lay["trailer_off"] - lay["text_off"], b"\x00") + trailer.encode("ascii")
    return dict(chunk=chunk, access=access, carrier=carrier, blob=blob, text=text, chunks=chunks, layout=lay,
                trailer=trailer, tail=tail)


def default_bytes(p: dict, t: dict) -> bytes:
    """A parameter's default as it lies in the segment (whole bytes, big endian)."""
    return int(p["default"]).to_bytes((t["bits"] + 7) // 8, "big")


def generate(m: dict, behaviour_src: str | None = None, access: str | None = None,
             app_number: int | None = None, app_name: str | None = None, signer=None,
             carrier: str | None = None) -> tuple[ET.Element, dict | None]:
    check_manifest(m)
    knx = m["knx"]
    app, hw, cat = dict(knx["application"]), knx["hardware"], knx["catalog"]
    if app_number is not None:
        app["number"] = app_number
    if app_name:
        app["name"] = app_name
    lang = knx.get("language", "en")
    mfr = knx["manufacturer"]
    params = knx["parameters"]
    emb = embedding(m, behaviour_src, access, signer, carrier)
    seg_size = emb["layout"]["segment_size"] if emb else params["segment_size"]
    how = emb["carrier"] if emb else None

    root = ET.Element("KNX", {"xmlns": NS, "CreatedBy": "KNX MT", "ToolVersion": "5.1.255.16695"})
    man = sub(sub(root, "ManufacturerData"), "Manufacturer", RefId=mfr)

    section = sub(man, "Catalog")
    for sid, name, number in catalog_sections(cat, mfr):
        section = sub(section, "CatalogSection", Id=sid, Name=name, Number=number, DefaultLanguage=lang)
    sub(section, "CatalogItem", Id="%CatalogItemId%", Name=app_name or hw["name"], Number=cat["item_number"],
        ProductRefId="%ProductId%", Hardware2ProgramRefId="%Hardware2ProgramId%", DefaultLanguage=lang)

    prog = sub(sub(man, "ApplicationPrograms"), "ApplicationProgram", Id="%AID%", ApplicationNumber=app["number"],
               ApplicationVersion=app["version"], ReplacesVersions="0", ProgramType="ApplicationProgram",
               MaskVersion=app["mask"], Name=app["name"], LoadProcedureStyle="MergedProcedure", PeiType="0",
               DefaultLanguage=lang, DynamicTableManagement="false", Linkable="true", MinEtsVersion="5.0")
    static = sub(prog, "Static")
    seg = sub(sub(static, "Code"), "RelativeSegment", Id=SEG_ID, Name="Parameters", Offset="0", Size=seg_size,
              LoadStateMachine="4")
    if how == "data":                             # the whole segment as a static image; ETS lays the parameters over it
        img = bytearray(seg_size)
        for p in params["list"]:
            raw = default_bytes(p, knx["parameter_types"][p["type"]])
            img[p["offset"]:p["offset"] + len(raw)] = raw
        img[emb["layout"]["text_off"]:] = emb["tail"]
        seg.set("Name", "Parameters+Template")
        sub(seg, "Data").text = base64.b64encode(bytes(img)).decode("ascii")
        sub(seg, "Mask").text = base64.b64encode(b"\xff" * seg_size).decode("ascii")

    ptypes = sub(static, "ParameterTypes")
    for name, t in knx["parameter_types"].items():
        pt = sub(ptypes, "ParameterType", Id=f"%AID%_PT-{name}", Name=name)
        if "number" in t:
            n = t["number"]
            sub(pt, "TypeNumber", SizeInBit=t["bits"], Type=n["type"], minInclusive=n["min"], maxInclusive=n["max"])
        elif "enum" in t:
            r = sub(pt, "TypeRestriction", Base="Value", SizeInBit=t["bits"])
            for value, text in t["enum"]:
                sub(r, "Enumeration", Text=text, Value=value, Id=f"%AID%_PT-{name}_EN-{value}")
        else:
            raise GenError(f"parameter type {name}: neither number nor enum")
    if how == "text":
        sub(sub(ptypes, "ParameterType", Id="%AID%_PT-KgtChunk", Name="KgtChunk"), "TypeText",
            SizeInBit=emb["chunk"] * 8)
        sub(sub(ptypes, "ParameterType", Id="%AID%_PT-KgtTrailer", Name="KgtTrailer"), "TypeText",
            SizeInBit=TRAILER_CHARS * 8)
    elif how == "rawdata":
        sub(sub(ptypes, "ParameterType", Id="%AID%_PT-KgtBlob", Name="KgtBlob"), "TypeRawData",
            MaxSize=len(emb["tail"]))

    plist = [dict(p, _type=p["type"]) for p in params["list"]]
    hidden = []                                   # positions (1-based) of the embedding's parameters
    if how == "rawdata":                          # one parameter, hidden by its own Access, referenced like any other
        plist.append(dict(name="KgtBlob", text="Template", _type="KgtBlob", access="None",
                          default=base64.b64encode(emb["tail"]).decode("ascii"),
                          offset=emb["layout"]["text_off"] - RAW_HEAD))
    if how == "text":
        lay = emb["layout"]
        for i, c in enumerate(emb["chunks"]):
            plist.append(dict(name=f"KgtPart{i + 1:03d}", text=f"Template part {i + 1}", _type="KgtChunk",
                              default=c, offset=lay["text_off"] + i * emb["chunk"]))
        plist.append(dict(name="KgtTrailer", text="Template trailer", _type="KgtTrailer", default=emb["trailer"],
                          offset=lay["trailer_off"]))
        hidden = list(range(len(params["list"]) + 1, len(plist) + 1))
    pnode = sub(static, "Parameters")
    for i, p in enumerate(plist, 1):
        attrs = dict(Id=f"%AID%_P-{i}", Name=p["name"], ParameterType=f"%AID%_PT-{p['_type']}", Text=p["text"])
        if p.get("access"):
            attrs["Access"] = p["access"]
        e = sub(pnode, "Parameter", **attrs, Value=p["default"])
        sub(e, "Memory", CodeSegment=SEG_ID, Offset=p["offset"], BitOffset="0")
    prefs = sub(static, "ParameterRefs")
    for i, _ in enumerate(plist, 1):
        attrs = dict(Id=f"%AID%_P-{i}_R-{i}", RefId=f"%AID%_P-{i}")
        if i in hidden:
            attrs["Access"] = "Read" if emb["access"] == "read" else "None"
        sub(prefs, "ParameterRef", **attrs)

    objs = knx["objects"]
    table = sub(static, "ComObjectTable")
    for o in objs:
        n = o["number"]
        attrs = dict(Id=f"%AID%_O-{n}", Name=o["name"], Text=o["text"], Number=n, FunctionText=o["function"],
                     ObjectSize=o["size"])
        attrs.update({attr: "Enabled" if f in o["flags"] else "Disabled" for f, attr in FLAGS})
        attrs["DatapointType"] = dpst(o["dpt"])
        sub(table, "ComObject", **attrs)
    orefs = sub(static, "ComObjectRefs")
    for o in objs:
        n = o["number"]
        sub(orefs, "ComObjectRef", Id=f"%AID%_O-{n}_R-{n}", RefId=f"%AID%_O-{n}")

    sub(static, "AddressTable", MaxEntries="65535")
    sub(static, "AssociationTable", MaxEntries="65535")
    lps = sub(static, "LoadProcedures")
    lp = sub(lps, "LoadProcedure", MergeId="2")
    sub(lp, "LdCtrlRelSegment", LsmIdx="4", Size=seg_size, Mode="1", Fill="0", AppliesTo="full")
    sub(lp, "LdCtrlRelSegment", LsmIdx="4", Size=seg_size, Mode="0", Fill="0", AppliesTo="par")
    lp = sub(lps, "LoadProcedure", MergeId="4")
    if how == "inline":                           # parameters from the segment, the template from the procedure
        sub(lp, "LdCtrlWriteRelMem", ObjIdx="4", Offset="0", Size=params["segment_size"], Verify="true",
            AppliesTo="full,par")
        sub(lp, "LdCtrlWriteRelMem", ObjIdx="4", Offset=emb["layout"]["text_off"], Size=len(emb["tail"]),
            Verify="true", AppliesTo="full", InlineData=emb["tail"].hex().upper())
    else:
        sub(lp, "LdCtrlWriteRelMem", ObjIdx="4", Offset="0", Size=seg_size, Verify="true", AppliesTo="full,par")
    lp = sub(lps, "LoadProcedure", MergeId="7")
    for obj in (1, 2, 3, 4):
        sub(lp, "LdCtrlLoadImageProp", ObjIdx=obj, PropId="27")
    sub(static, "Options")

    block = params.get("block", {"name": "General", "text": app["name"]})
    cib = sub(sub(prog, "Dynamic"), "ChannelIndependentBlock")
    pb = sub(cib, "ParameterBlock", Id="%AID%_PB-1", Name=block["name"], Text=block["text"])
    for i, _ in enumerate(plist, 1):
        if i not in hidden:
            sub(pb, "ParameterRefRef", RefId=f"%AID%_P-{i}_R-{i}")
    for o in objs:
        n = o["number"]
        sub(pb, "ComObjectRefRef", RefId=f"%AID%_O-{n}_R-{n}")
    if hidden and emb["access"] != "unreferenced":
        tb = sub(cib, "ParameterBlock", Id="%AID%_PB-2", Name="Template", Text="Template (embedded)")
        if emb["access"] == "none":
            tb.set("Access", "None")
        for i in hidden:
            sub(tb, "ParameterRefRef", RefId=f"%AID%_P-{i}_R-{i}")

    hwe = sub(sub(man, "Hardware"), "Hardware", Id="%HardwareId%", Name=app_name or hw["name"],
              SerialNumber=hw["serial_number"], VersionNumber=hw["version"], BusCurrent=hw["bus_current_ma"],
              HasIndividualAddress="true", HasApplicationProgram="true")
    if hw.get("nonreg_version", 0):              # data of the hardware that the registration does not cover changed
        hwe.set("NonRegRelevantDataVersion", str(int(hw["nonreg_version"])))
    prod = sub(sub(hwe, "Products"), "Product", Id="%ProductId%", Text=app_name or hw["name"],
               OrderNumber=hw["order_number"], IsRailMounted="false", DefaultLanguage=lang)
    sub(prod, "RegistrationInfo", RegistrationStatus="Registered")
    h2p = sub(sub(hwe, "Hardware2Programs"), "Hardware2Program", Id="%Hardware2ProgramId%", MediumTypes="MT-0")
    sub(h2p, "ApplicationProgramRef", RefId="%AID%")
    sub(h2p, "RegistrationInfo", RegistrationStatus="Registered",       # 0001/<hardware version><application version>
        RegistrationNumber=f"0001/%HardwareVersionEncoded%{app['version']}")
    return root, emb


def render(root: ET.Element, m: dict, src: str, emb: dict | None) -> str:
    ET.indent(root, space="  ")
    extra = ""
    if emb:
        lay = emb["layout"]
        where = {"text": f"{lay['chunks']} hidden text parameters of {emb['chunk']} (access {emb['access']})",
                 "rawdata": f"one hidden TypeRawData parameter at {lay['text_off'] - RAW_HEAD}",
                 "data": "the static image of the segment (Data, Mask)",
                 "inline": "InlineData of a second LdCtrlWriteRelMem"}[emb["carrier"]]
        extra = (f"\n     Embedded template: {len(emb['blob'])} bytes packed, {len(emb['text'])} base64 characters in "
                 f"{where},\n     text at {lay['text_off']}, trailer at {lay['trailer_off']}, parameter segment "
                 f"{lay['segment_size']} bytes (tools/kgt.py).")
    head = ('<?xml version="1.0" encoding="utf-8"?>\n'
            f"<!-- Generated by tools/knxprod_gen.py from {src}\n"
            f"     (template {m['id']} version {m['version']}); edit the manifest, not this file.\n"
            f"     {m['knx']['manufacturer']} (KNX Association) as open-source KNX projects use it; not registered.{extra}\n"
            f"     Build: OpenKNXproducer create <name> on a Windows PC with ETS -->\n")
    return head + ET.tostring(root, encoding="unicode") + "\n"


def image(xml_text: str) -> bytes:
    """The parameter segment (LSM 4) as ETS writes it from the product's defaults: the static image of the
    segment if it has one, over it every parameter with a Memory element in its byte place (numbers big endian,
    text as Latin-1 padded with NUL, raw data behind its 4-byte length), then the inline data of the load
    procedure. Only whole bytes."""
    root = ET.fromstring(xml_text.split("\n", 1)[1])
    q = lambda tag: f"{{{NS}}}{tag}"                                       # noqa: E731
    seg = next(root.iter(q("RelativeSegment")))
    size = int(seg.get("Size"))
    data = seg.find(q("Data"))
    out = bytearray(base64.b64decode(data.text, validate=True)) if data is not None else bytearray(size)
    if len(out) != size:
        raise GenError(f"segment image of {len(out)} bytes in a segment of {size}")
    types = {}
    for pt in root.iter(q("ParameterType")):
        kind = pt[0]
        tag = kind.tag.split("}")[1]
        types[pt.get("Id")] = (tag, int(kind.get("MaxSize")) * 8 if tag == "TypeRawData" else int(kind.get("SizeInBit")))
    for p in root.iter(q("Parameter")):
        mem = p.find(q("Memory"))
        if mem is None:
            continue
        kind, bits = types[p.get("ParameterType")]
        off = int(mem.get("Offset"))
        if bits % 8 or int(mem.get("BitOffset")) != 0:
            raise GenError(f"{p.get('Name')}: only whole-byte parameters are computed")
        n = bits // 8
        if kind == "TypeText":
            raw = p.get("Value").encode("latin-1")
            if len(raw) > n:
                raise GenError(f"{p.get('Name')}: text longer than {n} bytes")
            raw = raw.ljust(n, b"\x00")
        elif kind == "TypeRawData":
            value = base64.b64decode(p.get("Value"), validate=True)
            if len(value) > n:
                raise GenError(f"{p.get('Name')}: {len(value)} bytes of raw data, MaxSize {n}")
            raw = len(value).to_bytes(RAW_HEAD, "big") + value.ljust(n, b"\x00")
        else:
            raw = int(p.get("Value")).to_bytes(n, "big")
        if off + len(raw) > size:
            raise GenError(f"{p.get('Name')}: {len(raw)} bytes at {off} leave the segment of {size}")
        out[off:off + len(raw)] = raw
    for w in root.iter(q("LdCtrlWriteRelMem")):
        if w.get("InlineData") is None or w.get("ObjIdx") != "4":
            continue
        raw, off = bytes.fromhex(w.get("InlineData")), int(w.get("Offset"))
        if len(raw) != int(w.get("Size")) or off + len(raw) > size:
            raise GenError(f"inline data of {len(raw)} bytes at {off}: Size {w.get('Size')}, segment {size}")
        out[off:off + len(raw)] = raw
    return bytes(out)


def canon(e: ET.Element, path: str = "") -> list[str]:
    """Flatten a tree to comparable lines: path, sorted attributes, stripped text. Comments are not parsed."""
    here = f"{path}/{e.tag.split('}')[-1]}"
    out = [f"{here} {sorted(e.attrib.items())} {(e.text or '').strip()!r}"]
    for i, c in enumerate(e):
        out += canon(c, f"{here}[{i}]")
    return out


def compare(generated: str, ref_path: str) -> list[str]:
    a = canon(ET.fromstring(generated.split("\n", 1)[1]))      # skip the XML declaration line
    b = canon(ET.parse(ref_path).getroot())
    diffs = []
    for i in range(max(len(a), len(b))):
        x = a[i] if i < len(a) else "<missing>"
        y = b[i] if i < len(b) else "<missing>"
        if x != y:
            diffs.append(f"generated: {x}\nreference: {y}")
    return diffs


def check_embedding(text: str, m: dict, behaviour_src: str, emb: dict, signer=None) -> str:
    """Round trip: the segment ETS would write must unpack to exactly the packed template (and its signature
    must verify with the signer's public key)."""
    seg = image(text)
    trust = kgt.load_trust(points=[signer.point]) if signer else {}
    p, d, params, err = kgt.extract(seg, trust, allow_unsigned=signer is None)
    if err or p is None:
        raise GenError(f"embedding round trip failed: {err or 'no trailer found'}")
    want_payload = kgt.payload_json(m, behaviour_src)
    p = {k: v for k, v in p.items() if k != "_signed_by"}
    if d != kgt.digest(want_payload) or json.dumps(p, sort_keys=True) != json.dumps(json.loads(want_payload), sort_keys=True):
        raise GenError("embedding round trip gives another template")
    if len(params) != m["knx"]["parameters"]["segment_size"]:
        raise GenError(f"embedding round trip: {len(params)} parameter bytes")
    return d


def write_atomic(path: str, data: bytes):
    d = os.path.dirname(os.path.abspath(path))
    for _ in range(3):
        fd, tmp = tempfile.mkstemp(dir=d, prefix=".knxprod_gen.", suffix=".tmp")
        os.fchmod(fd, 0o644)                                      # mkstemp gives 0600; the file is for others too
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
        dfd = os.open(d, os.O_DIRECTORY)
        os.fsync(dfd)
        os.close(dfd)
        if open(path, "rb").read() == data:                       # NFS: read back before anyone uses it
            return
    raise GenError(f"{path}: read-back differs after 3 writes")


def build(template_dir: str, manifest_path: str | None = None, signer=None,
          **kw) -> tuple[str, dict, dict | None, str | None]:
    """Manifest (+ behaviour) -> (producer XML, manifest, embedding, digest of the embedded template)."""
    src = manifest_path or os.path.join(template_dir, "manifest.json")
    m = json.load(open(src))
    be_path = os.path.join(template_dir, m["behaviour"]["file"])
    behaviour_src = open(be_path).read() if os.path.exists(be_path) else None
    rel = os.path.relpath(os.path.abspath(src), ROOT)
    root, emb = generate(m, behaviour_src, signer=signer, **kw)
    text = render(root, m, rel, emb)
    d = check_embedding(text, m, behaviour_src, emb, signer) if emb else None
    return text, m, emb, d


def make_signer(args):
    """The signer the options name: a PEM key (--sign-key), or an external command (--sign-cmd) together with its
    public key (--sign-pub, or --sign-fpr looked up in tools/kgt_trust.json)."""
    if args.sign_cmd:
        if args.sign_key:
            raise GenError("a signing key and a signer command are both given (options or environment); choose one")
        if args.sign_pub:
            point = bytes.fromhex(args.sign_pub)
        elif args.sign_fpr:
            with open(TRUST_FILE, encoding="utf-8") as f:
                keys = {k["fpr"]: bytes.fromhex(k["p256"]) for k in json.load(f)["keys"]}
            if args.sign_fpr not in keys:
                raise GenError(f"key {args.sign_fpr} is not in {TRUST_FILE}")
            point = keys[args.sign_fpr]
        else:
            raise GenError("--sign-cmd needs the signer's public key: --sign-pub or --sign-fpr")
        return kgt.load_command_signer(args.sign_cmd, point)
    if not args.sign_key:
        raise GenError("no signing key: --sign-key PEM or --sign-cmd CMD (or --unsigned for tests)")
    return kgt.load_signer(args.sign_key)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("template", nargs="?", help="template directory with manifest.json")
    ap.add_argument("--issued-record", nargs=2, metavar=("PRODUCER_XML", "APP_ID"),
                    help="print the producer/issued.json entry of the application in PRODUCER_XML, imported into ETS "
                         "as APP_ID (e.g. M-00FA_A-C002-04-1A2B), and exit")
    ap.add_argument("--manifest", help="another manifest file (e.g. a frozen version) with the template's behaviour")
    ap.add_argument("-o", "--out", help="write here (atomic) instead of stdout")
    ap.add_argument("--check", metavar="REF.xml", help="compare with a reference producer input")
    ap.add_argument("--image", metavar="OUT.bin", help="write the parameter segment ETS would write from the defaults")
    ap.add_argument("--embed-carrier", choices=CARRIERS, help="what carries the template (default: manifest, else text)")
    ap.add_argument("--embed-access", choices=("none", "unreferenced", "read"),
                    help="test variant of the hidden text parameters (carrier text)")
    ap.add_argument("--app-number", type=lambda s: int(s, 0), help="application number (test variants side by side)")
    ap.add_argument("--app-name", help="application, hardware and catalog name (test variants)")
    ap.add_argument("--sign-key", default=SIGN_KEY,
                    help="PEM private key (P-256) that signs the embedded template (default: $ZIGBEE2KNX_SIGN_KEY)")
    ap.add_argument("--sign-cmd", default=SIGN_CMD,
                    help="external signer instead of --sign-key: gets the SHA-256 digest as hex on stdin, prints r||s "
                         "as hex (default: $ZIGBEE2KNX_SIGN_CMD)")
    ap.add_argument("--sign-pub", help="with --sign-cmd: the signer's public point, 65 bytes uncompressed, as hex")
    ap.add_argument("--sign-fpr", default=SIGN_FPR,
                    help="with --sign-cmd: the signer's key by fingerprint from tools/kgt_trust.json "
                         "(default: $ZIGBEE2KNX_SIGN_FPR)")
    ap.add_argument("--unsigned", action="store_true", help="embed without signature (tests only; the gateway refuses it)")
    ap.add_argument("--overwrite-version", action="store_true",
                    help="with -o: replace an application of the same number and version that has other content")
    args = ap.parse_args()
    if args.issued_record:
        try:
            rec = issued_record(open(args.issued_record[0], encoding="utf-8").read(), args.issued_record[1])
        except (GenError, OSError, ET.ParseError, kgt.KgtError) as e:
            sys.exit(f"knxprod_gen: {args.issued_record[0]}: {e}")
        print(json.dumps(rec, indent=2, ensure_ascii=False))
        return
    if not args.template:
        ap.error("TEMPLATE_DIR is needed")
    try:
        signer = None if args.unsigned else make_signer(args)
        text, m, emb, d = build(args.template, args.manifest, signer=signer, access=args.embed_access,
                                carrier=args.embed_carrier, app_number=args.app_number, app_name=args.app_name)
    except (GenError, KeyError, OSError, ValueError, kgt.KgtError) as e:
        sys.exit(f"knxprod_gen: {args.template}: {e}")
    if emb:
        lay = emb["layout"]
        how = f"signed by {kgt.fingerprint(signer.point)}" if signer else "UNSIGNED"
        what = f"{lay['chunks']} parts, access {emb['access']}" if emb["carrier"] == "text" else "one piece"
        print(f"embedded template {d}: {len(emb['blob'])} bytes packed, carrier {emb['carrier']} ({what}), segment "
              f"{lay['segment_size']} bytes, {how}; round trip ok", file=sys.stderr)
    if args.check:
        diffs = compare(text, args.check)
        if diffs:
            print(f"{len(diffs)} difference(s) to {args.check}, first ones:")
            print("\n".join(diffs[:5]))
            sys.exit(1)
        print(f"identical to {args.check} (elements, attributes and text; comments and layout ignored)")
    if args.image:
        write_atomic(args.image, image(text))
        print(f"written {args.image}")
    if args.out:
        # a version given out (producer/issued.json) is never written with other content, whatever the switches
        # and the output file. ReplacesVersions is left out of the comparison: this generator writes "0", and
        # distill/knxprod_products.py and producer/gateway_build.py set it afterwards (and check the rest of the
        # list). Test variants with a number of their own (--app-number) are not in that list and pass.
        issued = load_issued()
        mn = application_meaning(text)
        same = [e for e in issued.get("applications", []) if e["number"] == mn["number"] and e["version"] == mn["version"]]
        if same:
            want = " ".join(str(v) for v in issued_below(issued, mn["number"], mn["version"])) or "0"
            as_given = re.sub(r'ReplacesVersions="[^"]*"', f'ReplacesVersions="{want}"', text, count=1)
            if application_meaning(as_given)["sha256"] != same[0]["application_sha256"]:
                sys.exit(f"knxprod_gen: {args.out}: not written: application 0x{mn['number']:04X} version "
                         f"{mn['version']} has been given out ({same[0]['id']}) with other content")
        if os.path.exists(args.out) and not args.overwrite_version:
            clash = version_clash(open(args.out, encoding="utf-8").read(), text)
            if clash:
                sys.exit(f"knxprod_gen: {args.out}: {clash}")
        write_atomic(args.out, text.encode("utf-8"))
        print(f"written {args.out}")
    elif not (args.check or args.image):
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
