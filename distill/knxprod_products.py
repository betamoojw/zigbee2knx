#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dirk Tostmann
"""knxprod_products — the producer input of a class application with one catalog product per Zigbee model.

  knxprod_products.py TEMPLATE_DIR [-o producer/<class>.xml] [--knxprod-gen PATH] [--sign-key PEM]
  knxprod_products.py --append producer/<class>.xml (TEMPLATE_DIR | --products FILE.json) [-o OUT.xml]

The full run (first form):
Step 1 runs tools/knxprod_gen.py: one product, the application with the embedded signed template, the
round-trip check of the embedding.
Step 2 adds a product and a catalog item for every model in manifest "products". OpenKNXproducer expands
the placeholders of ONE product only (ProcessInclude.cs: SelectSingleNode on Product/@OrderNumber and on
CatalogItem; checked at OpenKNXproducer 7f23a63, 2026-08-14), so the further ones get explicit ids, built
from the placeholders it does expand, the way it builds its own:
    Product      %HardwareId%_P-<enc(order number)>
    CatalogItem  %Hardware2ProgramId%_CI-<enc(order number)>-<Number>
enc() is OpenKNXproducer's GetEncoded (Program.cs): the characters . % space ! " # $ & ( ) + - / : ; < > = ?
@ [ \\ ] ^ _ { | } become ".XX" (hex), '.' first. The id suffix equals the catalog item's Number, as in
every catalog item of an ETS product file checked here (151 of 151 in a Merten knxprod). All products
hang on the class's one hardware and its one Hardware2Program, so they share the application.
Step 3 puts the application versions the class replaces (manifest knx.application.replaces) into
ReplacesVersions, where knxprod_gen writes "0", as producer/gateway_build.py does for the gateway, so that ETS
can update a device loaded with an earlier version instead of having it deleted and added again. ETS 6.4.1 did
that on 2026-11-15 once a device of the new version had been inserted into the project; whether ReplacesVersions is
needed for it is not tested. The head comment names the template as it lies in this tree.
A full run refuses to overwrite an output that holds the same application number and version with other content
(--overwrite-version: only for a version never given out).

Every full run signs the template anew, and with another signature the ETS library gives the application another
id: whoever has the product in a project then has to load its devices again.

--append (second form) adds catalog products to a producer input that exists, and nothing else. It signs nothing
and needs no key. The application with its embedded template stays byte for byte, so its id stays and no device
has to be loaded again. A product the file holds already (same order number) is left as it is. The result is the
file a full run would have written with these products at the end.
  with TEMPLATE_DIR      the products are those of the manifest. The template of that directory must be the one the
                         application carries (same digest, tools/kgt.py). If it is not,
                         the template has changed: that needs a new application version and a full run.
  with --products FILE   a JSON list: [{"vendor": "..", "model": "..", "description": "..", "zigbeeModel": [..]}],
                         "zigbeeModel" may be left out. Needs nothing but this file and the producer input.
A product whose Zigbee model the embedded template does not name works all the same: the class runs it with its
defaults, and the gateway reports MODEL for it.

Seen: OpenKNXproducer accepts the explicit ids (inputs of 2026-11-12), and ETS 6.4.1 accepted a second import that
brings the same application with more products (2026-11-15). A device from an added product has not been loaded yet.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import importlib.util
import json
import os
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zlib
from xml.sax.saxutils import quoteattr

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOME = ROOT
KNXPROD_GEN = os.path.join(ROOT, "tools", "knxprod_gen.py")
KGT = os.path.join(ROOT, "tools", "kgt.py")
ENCODED = ['.', '%', ' ', '!', '"', '#', '$', '&', '(', ')', '+', '-', '/', ':', ';', '<', '>', '=', '?', '@', '[',
           '\\', ']', '^', '_', '{', '|', '}']


class ProductError(Exception):
    pass


def get_encoded(s: str) -> str:
    """OpenKNXproducer Program.GetEncoded, same characters, same order."""
    for ch in ENCODED:
        s = s.replace(ch, f".{ord(ch):02X}")
    return s


def atomic_write(path: str, data: bytes):
    want = hashlib.sha256(data).hexdigest()
    for _ in range(3):
        tmp = path + ".tmp"
        with open(tmp, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
        fd = os.open(os.path.dirname(path) or ".", os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
        with open(path, "rb") as f:
            back = f.read()
        if b"\x00" not in back and hashlib.sha256(back).hexdigest() == want:
            return
    raise ProductError(f"{path}: written three times, read back differently")


def product_texts(p: dict, lang: str, item_ind: str, prod_ind: str, orders: list) -> tuple[str, str]:
    """The catalog item and the product of one model, as lines of the producer input. `orders` holds the order
    numbers there are and gets this one."""
    for key in ("vendor", "model", "description"):
        if not isinstance(p, dict) or not isinstance(p.get(key), str) or not p[key]:
            raise ProductError(f"product {p!r}: needs the texts vendor, model and description")
    order = p["model"]
    if order in orders:
        raise ProductError(f"order number {order!r} twice")
    orders.append(order)
    enc = get_encoded(order)
    if not re.fullmatch(r"[A-Za-z0-9.]+", enc):
        raise ProductError(f"order number {order!r} leaves characters an id may not hold: {enc!r}")
    name = f"{p['vendor']} {p['model']} ({p['description']})"[:200]   # ETS search finds the description too
    pid = f"%HardwareId%_P-{enc}"
    item = (f'{item_ind}<CatalogItem Id="%Hardware2ProgramId%_CI-{enc}-1" Name={quoteattr(name)} Number="1" '
            f'ProductRefId="{pid}" Hardware2ProgramRefId="%Hardware2ProgramId%" DefaultLanguage="{lang}" />\n')
    prod = (f'{prod_ind}<Product Id="{pid}" Text={quoteattr(name)} OrderNumber={quoteattr(order)} IsRailMounted="false" '
            f'DefaultLanguage="{lang}">\n{prod_ind}  <RegistrationInfo RegistrationStatus="Registered" />\n'
            f'{prod_ind}</Product>\n')
    return item, prod


def add_products(xml: str, products: list, lang: str) -> tuple[str, int]:
    item = re.search(r'^([ \t]*)<CatalogItem Id="%CatalogItemId%"[^>]*/>\n', xml, re.M)
    prod = re.search(r'^([ \t]*)<Product Id="%ProductId%".*?</Product>\n', xml, re.M | re.S)
    if not (item and prod) or xml.count("<CatalogItem ") != 1 or xml.count("<Product ") != 1:
        raise ProductError("expected exactly one CatalogItem %CatalogItemId% and one Product %ProductId% (knxprod_gen output)")
    first_order = re.search(r'OrderNumber="([^"]*)"', prod.group(0)).group(1)
    orders = [first_order]
    items, prods = [], []
    for p in products:
        i, q = product_texts(p, lang, item.group(1), prod.group(1), orders)
        items.append(i)
        prods.append(q)
    out = xml[:prod.end()] + "".join(prods) + xml[prod.end():]
    item2 = re.search(r'^[ \t]*<CatalogItem Id="%CatalogItemId%"[^>]*/>\n', out, re.M)
    out = out[:item2.end()] + "".join(items) + out[item2.end():]
    return out, len(products)


def embedded_template(xml: str) -> tuple[dict, str]:
    """(payload, digest) of the template the application carries in its parameter segment (format: tools/kgt.py).
    The signature is not checked here: the generator did, and the gateway does."""
    data = re.findall(r'<RelativeSegment [^>]*LoadStateMachine="4"[^>]*>\s*<Data>([^<]*)</Data>', xml)
    if len(data) != 1:
        raise ProductError(f"embedded template: {len(data)} parameter segments with <Data>, expected one")
    try:
        seg = base64.b64decode(data[0], validate=True)
        t = seg[-16:].decode("ascii", "replace")
        if not re.fullmatch(r"KGTE[0-9A-F]{12}", t):
            raise ValueError(f"no trailer of an embedded template at the end of the segment ({t!r})")
        off, length = int(t[4:8], 16), int(t[8:12], 16)
        blob = base64.b64decode(seg[off:off + length], validate=True)
        z = blob[88:]
        if blob[:4] != b"KGT2" or int.from_bytes(blob[4:8], "big") != len(z) or hashlib.sha256(z).digest()[:8] != blob[8:16]:
            raise ValueError("the container is not KGT2, or its length or checksum is wrong")
        raw = zlib.decompress(z)
        payload = json.loads(raw)
        if not isinstance(payload, dict) or not isinstance(payload.get("manifest"), dict):
            raise ValueError("the payload has no manifest")
    except (ValueError, zlib.error) as e:
        raise ProductError(f"embedded template: {e}") from None
    return payload, hashlib.sha256(raw).hexdigest()[:16]


def template_digest(template_dir: str, kgt_path: str = KGT) -> tuple[str, dict]:
    """(digest, manifest) of the template in a directory, packed the way knxprod_gen packs it."""
    spec = importlib.util.spec_from_file_location("kgt_shared", kgt_path)
    kgt = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(kgt)
    with open(os.path.join(template_dir, "manifest.json"), encoding="utf-8") as f:
        m = json.load(f)
    try:
        with open(os.path.join(template_dir, m["behaviour"]["file"]), encoding="utf-8") as f:
            src = f.read()
        m["knx"]["application"]["number"], m["knx"]["application"]["version"]
        return kgt.digest(kgt.payload_json(m, src)), m
    except (KeyError, TypeError) as e:
        raise ProductError(f"{template_dir}/manifest.json: not the manifest of a class template ({e!r})") from None


def same_template(xml: str, template_dir: str, kgt_path: str = KGT) -> tuple[dict, str]:
    """The manifest of the directory and the digest, if its template is the one the application carries."""
    _, emb = embedded_template(xml)
    here, m = template_digest(template_dir, kgt_path)
    root = ET.fromstring(xml)
    ns = root.tag.split("}")[0] + "}"
    prog = next(root.iter(ns + "ApplicationProgram"))
    have = (int(prog.get("ApplicationNumber")), int(prog.get("ApplicationVersion")))
    app = m["knx"]["application"]
    if (int(app["number"]), int(app["version"])) != have:
        raise ProductError(f"the manifest is of application 0x{int(app['number']):04X} version {app['version']}, the file "
                           f"holds 0x{have[0]:04X} version {have[1]}")
    if here != emb:
        raise ProductError(f"the template in {template_dir} (digest {here}) is not the one the application carries "
                           f"({emb}): a changed template needs a new application version and a full run")
    return m, emb


def block(xml: str, tag: str) -> str:
    a, b = xml.find(f"<{tag}>"), xml.find(f"</{tag}>")
    if a < 0 or b < a or xml.count(f"<{tag}>") != 1:
        raise ProductError(f"expected one <{tag}>")
    return xml[a:b]


def append_products(xml: str, products: list) -> tuple[str, list, list, list]:
    """The products the file does not hold yet, behind those it holds; everything else as it is.
    -> (xml, order numbers added, order numbers that were there, notes on models the template does not name)."""
    if not isinstance(products, list):
        raise ProductError("the products are a list")
    payload, _ = embedded_template(xml)
    root = ET.fromstring(xml)
    ns = root.tag.split("}")[0] + "}"
    have = list(root.iter(ns + "Product"))
    prod_ms = list(re.finditer(r'^([ \t]*)<Product [^>]*>\n.*?^[ \t]*</Product>\n', xml, re.M | re.S))
    item_ms = list(re.finditer(r'^([ \t]*)<CatalogItem [^>]*/>\n', xml, re.M))
    if not have or len(prod_ms) != len(have) or len(item_ms) != len(list(root.iter(ns + "CatalogItem"))):
        raise ProductError("products and catalog items are not laid out as this tool writes them")
    lang = have[0].get("DefaultLanguage", "en")
    orders = [p.get("OrderNumber") for p in have]
    listed = payload["manifest"].get("match", {}).get("zigbeeModel")
    items, prods, added, present, notes = [], [], [], [], []
    for p in products:
        if isinstance(p, dict) and p.get("model") in orders and p.get("model") not in added:
            present.append(p["model"])
            continue
        i, q = product_texts(p, lang, item_ms[-1].group(1), prod_ms[-1].group(1), orders)
        items.append(i)
        prods.append(q)
        added.append(p["model"])
        names = p.get("zigbeeModel", [])
        if not isinstance(names, list) or not all(isinstance(z, str) for z in names):
            raise ProductError(f"product {p['model']!r}: zigbeeModel is a list of texts")
        unknown = [z for z in names if isinstance(listed, list) and z not in listed]
        if unknown:
            notes.append(f"{p['model']}: the template does not name {', '.join(repr(z) for z in unknown)}; the class "
                         f"runs it with its defaults and the gateway reports MODEL")
    out = xml
    for at, text in sorted(((prod_ms[-1].end(), "".join(prods)), (item_ms[-1].end(), "".join(items))), reverse=True):
        out = out[:at] + text + out[at:]
    for tag in ("ApplicationPrograms", "Hardware2Programs"):
        if block(out, tag) != block(xml, tag):
            raise ProductError(f"<{tag}> changed: not written")
    return out, added, present, notes


def replaces_versions(xml: str, app: dict) -> tuple[str, str]:
    """ReplacesVersions from knx.application.replaces instead of the "0" knxprod_gen writes."""
    replaces = [int(v) for v in app.get("replaces", [])]
    if any(v <= 0 or v >= int(app["version"]) for v in replaces):
        raise ProductError(f"replaces {replaces}: only versions below {app['version']}")
    old = 'ReplacesVersions="0"'
    if xml.count(old) != 1:
        raise ProductError(f"knxprod_gen wrote {xml.count(old)} times {old}, expected once")
    want = " ".join(str(v) for v in replaces) or "0"
    xml = xml.replace(old, f'ReplacesVersions="{want}"')
    note = "     ReplacesVersions set by distill/knxprod_products.py from knx.application.replaces.\n"
    head = "edit the manifest, not this file.\n"
    if head in xml:
        xml = xml.replace(head, head + note, 1)
    return xml, want


def home_path(xml: str, template: str, gen: str) -> str:
    """The head comment of knxprod_gen names the manifest relative to the generator's tree; name it as it lies in
    HOME."""
    gen_root = os.path.dirname(os.path.dirname(os.path.abspath(gen)))
    here = os.path.join(os.path.abspath(template), "manifest.json")
    rel = os.path.relpath(here, ROOT)
    if rel.startswith(".."):                       # a template outside this project: leave the comment alone
        return xml
    return xml.replace(f" from {os.path.relpath(here, gen_root)}\n",
                       f" from {os.path.relpath(os.path.join(HOME, rel), gen_root)}\n", 1)


def check(xml: str, n_extra: int, replaces: str):
    root = ET.fromstring(xml)
    ns = root.tag.split("}")[0] + "}"
    progs = list(root.iter(ns + "ApplicationProgram"))
    if [p.get("ReplacesVersions") for p in progs] != [replaces]:
        raise ProductError(f"ReplacesVersions {[p.get('ReplacesVersions') for p in progs]}, {replaces!r} expected")
    items = list(root.iter(ns + "CatalogItem"))
    prods = list(root.iter(ns + "Product"))
    if len(items) != 1 + n_extra or len(prods) != 1 + n_extra:
        raise ProductError(f"{len(items)} catalog items, {len(prods)} products, {1 + n_extra} expected")
    pids = [p.get("Id") for p in prods]
    if len(set(pids)) != len(pids) or len({i.get("Id") for i in items}) != len(items):
        raise ProductError("ids repeat")
    for i in items:
        if i.get("ProductRefId") not in pids:
            raise ProductError(f"catalog item {i.get('Id')} points to no product")
        if "_CI-" in i.get("Id") and i.get("Id").rsplit("-", 1)[1] != i.get("Number"):   # the first one is a placeholder
            raise ProductError(f"catalog item {i.get('Id')}: suffix is not its Number")
    if len(list(root.iter(ns + "Hardware2Program"))) != 1 or len(list(root.iter(ns + "ApplicationProgram"))) != 1:
        raise ProductError("one Hardware2Program and one ApplicationProgram expected")


def main_append(a):
    if bool(a.template) == bool(a.products):
        raise ProductError("--append takes the products from TEMPLATE_DIR or from --products FILE.json, one of them")
    with open(a.append, encoding="utf-8") as f:
        xml = f.read()
    if a.template:
        m, emb = same_template(xml, a.template, a.kgt)
        products = m.get("products", [])
    else:
        _, emb = embedded_template(xml)
        with open(a.products, encoding="utf-8") as f:
            products = json.load(f)
    out_xml, added, present, notes = append_products(xml, products)
    if not added:
        print(f"{a.append}: nothing to add, all {len(present)} products are there")
        return
    replaces = re.search(r'<ApplicationProgram [^>]*ReplacesVersions="([^"]*)"', out_xml).group(1)
    check(out_xml, out_xml.count("<Product ") - 1, replaces)
    out = a.out or a.append
    atomic_write(out, out_xml.encode("utf-8"))
    print(f"written {out}: {len(added)} products added ({', '.join(added)}), {len(present)} were there; the "
          f"application and its template (digest {emb}) are untouched")
    for n in notes:
        print("note: " + n)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("template", nargs="?")
    ap.add_argument("-o", "--out")
    ap.add_argument("--knxprod-gen", default=KNXPROD_GEN)
    ap.add_argument("--sign-key", help="passed to knxprod_gen (default there: $ZIGBEE2KNX_SIGN_KEY)")
    ap.add_argument("--append", metavar="XML", help="add products to this producer input; nothing is signed")
    ap.add_argument("--products", metavar="FILE.json", help="with --append: the products, instead of a template directory")
    ap.add_argument("--kgt", default=KGT, help="with --append TEMPLATE_DIR: kgt.py that packs the template for the comparison")
    ap.add_argument("--overwrite-version", action="store_true",
                    help="full run: replace an application of the same number and version that has other content")
    a = ap.parse_args()
    if a.append:
        return main_append(a)
    if not a.template or a.products:
        ap.error("TEMPLATE_DIR for a full run; --products goes with --append")
    m = json.load(open(os.path.join(a.template, "manifest.json"), encoding="utf-8"))
    products = m.get("products", [])
    lang = m["knx"].get("language", "en")
    out = a.out or os.path.join(ROOT, "producer", os.path.basename(os.path.normpath(a.template)) + ".xml")
    with tempfile.TemporaryDirectory() as td:
        single = os.path.join(td, "single.xml")
        cmd = [sys.executable, a.knxprod_gen, a.template, "-o", single] + (["--sign-key", a.sign_key] if a.sign_key else [])
        r = subprocess.run(cmd, capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)                 # the embedding: size, signer, round trip
        if r.returncode != 0:
            raise ProductError(f"knxprod_gen failed ({r.returncode})")
        xml = open(single, encoding="utf-8").read()
    xml = home_path(xml, a.template, a.knxprod_gen)
    xml, replaces = replaces_versions(xml, m["knx"]["application"])
    xml, n = add_products(xml, products, lang)
    check(xml, n, replaces)
    if os.path.exists(out) and not a.overwrite_version:
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        from knxprod_gen import version_clash
        clash = version_clash(open(out, encoding="utf-8").read(), xml)
        if clash:
            raise ProductError(f"{out}: {clash}")
    atomic_write(out, xml.encode("utf-8"))
    print(f"written {out}: 1 + {n} products on one hardware and one application, version "
          f"{m['knx']['application']['version']}, ReplacesVersions {replaces}")


if __name__ == "__main__":
    try:
        main()
    except (ProductError, OSError, json.JSONDecodeError, ET.ParseError) as e:
        sys.exit(f"knxprod_products: {e}")
