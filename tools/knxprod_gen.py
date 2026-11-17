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
n = object number, one ref per parameter and object: P-n_R-n, O-n_R-n).
"""
from __future__ import annotations

import argparse
import base64
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
FLAGS = (("R", "ReadFlag"), ("W", "WriteFlag"), ("C", "CommunicationFlag"), ("T", "TransmitFlag"),
         ("U", "UpdateFlag"), ("I", "ReadOnInitFlag"))
SEG_ID = "%AID%_RS-04-00000"
TRAILER_CHARS = kgt.TRAILER_LEN
CARRIERS = ("text", "rawdata", "data", "inline")
RAW_HEAD = 4        # bytes ETS keeps in front of a TypeRawData value: in the ABB i-bus product of the corpus the
                    # parameters lie MaxSize + 4 apart. Taken as the length, big endian; not seen on a device yet.
SIGN_KEY = os.environ.get("ZIGBEE2KNX_SIGN_KEY", "")   # PEM private key (P-256) that signs the embedded templates


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


def dpst(dpt: str) -> str:
    """"1.001" -> "DPST-1-1"."""
    main, _, sub = dpt.partition(".")
    if not (main.isdigit() and sub.isdigit()):
        raise GenError(f"datapoint type {dpt!r}: expected main.sub, e.g. 1.001")
    return f"DPST-{int(main)}-{int(sub)}"


def sub(parent, tag, **attrs):
    return ET.SubElement(parent, tag, {k: str(v) for k, v in attrs.items()})


def check_manifest(m: dict):
    knx = m["knx"]
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

    section = sub(sub(man, "Catalog"), "CatalogSection", Id=f"{mfr}_CS-{cat['section_number']}", Name=cat["section"],
                  Number=cat["section_number"], DefaultLanguage=lang)
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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("template", help="template directory with manifest.json")
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
    ap.add_argument("--unsigned", action="store_true", help="embed without signature (tests only; the gateway refuses it)")
    ap.add_argument("--overwrite-version", action="store_true",
                    help="with -o: replace an application of the same number and version that has other content")
    args = ap.parse_args()
    try:
        signer = None if args.unsigned else kgt.load_signer(args.sign_key)
        text, m, emb, d = build(args.template, args.manifest, signer=signer, access=args.embed_access,
                                carrier=args.embed_carrier, app_number=args.app_number, app_name=args.app_name)
    except (GenError, KeyError, OSError, kgt.KgtError) as e:
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
