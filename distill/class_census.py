#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dirk Tostmann
"""class_census.py - how many KNX applications (classes) would the Zigbee2MQTT definitions need, depending on
what the distiller treats as "the same ETS surface"?

  class_census.py DUMP.jsonl [--vendor IKEA] [--members] [--top N] [--merge [--show K]]

DUMP.jsonl is the output of dump_z2m.cjs. Every definition gets a signature at four levels:

  L0 exact     every expose and option as dumped (ranges, presets, endpoint names), labels and descriptions aside
  L1 surface   what ETS would show: objects (type, name, set/state, unit, enum values) and parameters (name, type,
               enum values). Numeric ranges, presets and endpoint names are left to the model table in the Berry.
  L2 runtime   the objects a bus user works with. Settings (options, category config), service points (battery,
               identify, start-up behaviour, effects, diagnostics) are not part of the identity: the class carries
               their superset. Lights are reduced to state/brightness/colour temperature/colour.
  L3 family    function family and channel count: light by capability, switch, cover, climate, lock, fan, remote,
               metering yes/no, set of measured values. Settable extras are not part of the identity.

A class is one signature. The census counts signatures, how many definitions sit in classes of one, and how many
classes it takes to cover a share of the definitions when the largest classes come first.

--merge adds superset classes: every definition becomes a set of function blocks (per light channel state,
brightness, colour temperature, colour; switch and cover channels; climate, lock, fan, remote, metering, every
measured value), and a class covers a definition when it has all of its blocks and at most k more. The cover is
greedy and takes its candidates from the sets that occur in the dump, so the counts are an upper bound for a
hand-made catalogue.

Not evaluated: definitions whose exposes need a paired device and come out empty in the dump (listed in the first
line of the output).
"""
import argparse
import collections
import json
import sys

SPECIFIC = ("light", "switch", "cover", "climate", "lock", "fan")
SERVICE = {"linkquality", "battery", "battery_low", "battery_state", "battery_voltage", "identify",
           "power_on_behavior", "effect", "effect_speed", "effect_color", "device_temperature",
           "power_outage_count", "tamper"}
LIGHT_CORE = {"state", "brightness", "color_temp", "color_xy", "color_hs"}
METER = ("power", "energy", "current", "voltage", "ac_frequency", "produced_energy")
DROP0 = ("label", "description")


def strip(e):
    o = {k: v for k, v in e.items() if k not in DROP0 and k not in ("features", "item_type")}
    if e.get("features"):
        o["features"] = [strip(f) for f in e["features"]]
    if isinstance(e.get("item_type"), dict):
        o["item_type"] = strip(e["item_type"])
    return o


def t1(e):
    o = {"t": e.get("type"), "n": e.get("name")}
    if e.get("category"):
        o["c"] = e["category"]
    if e.get("access") is not None:
        o["a"] = e["access"] & 3
    if e.get("unit"):
        o["u"] = e["unit"]
    if e.get("type") == "enum":
        o["v"] = sorted(str(v) for v in e.get("values", []))
    if e.get("features"):
        o["f"] = sorted(json.dumps(t1(f), sort_keys=True) for f in e["features"])
    if isinstance(e.get("item_type"), dict):
        o["i"] = t1(e["item_type"])
    return o


def light_core(e):
    core = {f.get("name") for f in e.get("features", [])} & LIGHT_CORE
    if core & {"color_xy", "color_hs"}:
        core = (core - {"color_xy", "color_hs"}) | {"color"}
    return core


def t2(e):
    t, n, c = e.get("type"), e.get("name"), e.get("category")
    if t in SPECIFIC:
        feats = {f.get("name") for f in e.get("features", [])}
        if t == "light":
            return "light(" + ",".join(sorted(light_core(e))) + ")"
        if t == "switch":
            return "switch"
        if t == "cover":
            return "cover(" + ",".join(sorted(feats & {"state", "position", "tilt"})) + ")"
        return t + "(" + ",".join(sorted(str(f) for f in feats)) + ")"
    if n in SERVICE:
        return None
    if n == "action":
        return "action{" + ",".join(sorted(str(v) for v in e.get("values", []))) + "}"
    if c in ("config", "diagnostic"):
        return None
    unit = "[" + e["unit"] + "]" if e.get("unit") else ""
    return ("set:" if e.get("access", 0) & 2 else "val:") + str(n) + unit


def t3(e):
    t, n, c = e.get("type"), e.get("name"), e.get("category")
    if t in SPECIFIC:
        if t == "light":
            core = light_core(e)
            cap = "colour" if "color" in core else "ct" if "color_temp" in core else "dim" if "brightness" in core \
                else "onoff"
            return "light:" + cap
        if t == "cover":
            return "cover+tilt" if any(f.get("name") == "tilt" for f in e.get("features", [])) else "cover"
        return t
    if n in SERVICE:
        return None
    if n == "action":
        return "remote"
    if c in ("config", "diagnostic") or e.get("access", 0) & 2:
        return None
    if str(n).startswith(METER):
        return "metering"
    return "val:" + str(n)


CHANNEL = ("light:", "switch", "cover")


def signatures(d):
    ex = [e for e in d["exposes"] if e.get("name") != "linkquality"]
    s0 = json.dumps([sorted(json.dumps(strip(e), sort_keys=True) for e in ex),
                     sorted(json.dumps(strip(o), sort_keys=True) for o in d["options"])])
    c1 = collections.Counter(json.dumps(t1(e), sort_keys=True) for e in ex)
    o1 = sorted(json.dumps({k: v for k, v in t1(o).items() if k != "a"}, sort_keys=True) for o in d["options"])
    s1 = json.dumps([sorted(c1.items()), o1])
    c2 = collections.Counter(x for x in (t2(e) for e in ex) if x)
    s2 = " + ".join((k if v == 1 else "%dx %s" % (v, k)) for k, v in sorted(c2.items()))
    c3 = collections.Counter(x for x in (t3(e) for e in ex) if x)
    parts = []
    for k, v in sorted(c3.items()):
        parts.append("%dx %s" % (v, k) if v > 1 and k.startswith(CHANNEL) else k)
    s3 = " + ".join(parts)
    return s0, s1, s2 or "(nothing at run time)", s3 or "(nothing at run time)"


def family(s3):
    for key, name in (("climate", "climate"), ("cover", "cover"), ("lock", "lock"), ("fan", "fan"),
                      ("light:", "light"), ("switch", "switch"), ("remote", "remote"), ("val:", "sensor"),
                      ("metering", "sensor")):
        if key in s3:
            return name
    return "none"


def cover(counter, share):
    need, got, total = 0, 0, sum(counter.values())
    for _, n in counter.most_common():
        need += 1
        got += n
        if got >= share * total:
            return need
    return need


def report(name, rows, top, members):
    print("\n=== %s: %d definitions ===" % (name, len(rows)))
    sig = [signatures(r) for r in rows]
    print("%-12s %8s %10s %22s   classes needed for 50/80/90/95/99 %%" % ("level", "classes", "of one", "definitions in those"))
    for i, lv in enumerate(("L0 exact", "L1 surface", "L2 runtime", "L3 family")):
        c = collections.Counter(s[i] for s in sig)
        ones = sum(1 for v in c.values() if v == 1)
        print("%-12s %8d %10d %21.1f%%   %s" % (lv, len(c), ones, 100.0 * ones / len(rows),
                                                 " / ".join(str(cover(c, p)) for p in (.5, .8, .9, .95, .99))))
    fam = collections.Counter(family(s[3]) for s in sig)
    print("families:", ", ".join("%s %d" % kv for kv in fam.most_common()))
    for i, lv in ((2, "L2 runtime"), (3, "L3 family")):
        c = collections.Counter(s[i] for s in sig)
        print("\n-- %s, largest %d of %d classes" % (lv, min(top, len(c)), len(c)))
        for k, v in c.most_common(top):
            line = "%5d  %s" % (v, k if len(k) < 150 else k[:147] + "...")
            if members:
                line += "   <- " + ", ".join(sorted(r["model"] for r, s in zip(rows, sig) if s[i] == k))
            print(line)
    # per family: classes at L2 and L3
    print("\n-- classes per family (L1 / L2 / L3) and definitions")
    for f, n in fam.most_common():
        idx = [j for j, s in enumerate(sig) if family(s[3]) == f]
        print("%-8s %5d definitions   %4d / %4d / %4d" % (f, n, len({sig[j][1] for j in idx}),
                                                         len({sig[j][2] for j in idx}), len({sig[j][3] for j in idx})))
    rem = [(r, s) for r, s in zip(rows, sig) if "remote" in s[3]]
    if rem:
        sets = collections.Counter()
        for r, _ in rem:
            for e in r["exposes"]:
                if e.get("name") == "action":
                    sets[tuple(sorted(str(v) for v in e.get("values", [])))] += 1
        pure = sum(1 for _, s in rem if s[3] == "remote")
        print("\nremotes: %d definitions with an action expose, %d of them with nothing else at run time; "
              "%d different sets of action values, %d used by one definition only"
              % (len(rem), pure, len(sets), sum(1 for v in sets.values() if v == 1)))


def blocks(d):
    lights, covers, switches, out = [], [], 0, set()
    for e in d["exposes"]:
        t, n, c = e.get("type"), e.get("name"), e.get("category")
        f = {x.get("name") for x in e.get("features", [])}
        if t == "light":
            caps = ["state"] + [x for x in ("brightness", "color_temp") if x in f]
            if f & {"color_xy", "color_hs"}:
                caps.append("color")
            lights.append(caps)
        elif t == "switch":
            switches += 1
        elif t == "cover":
            covers.append(["move"] + [x for x in ("position", "tilt") if x in f])
        elif t in SPECIFIC:
            out.add(t)
        elif n in SERVICE:
            continue
        elif n == "action":
            out.add("remote")
        elif c in ("config", "diagnostic") or e.get("access", 0) & 2:
            continue
        elif str(n).startswith(METER):
            out.add("metering")
        else:
            out.add("val:" + str(n))
    for i, caps in enumerate(sorted(lights, key=len, reverse=True), 1):
        out |= {"light%d.%s" % (i, x) for x in caps}
    for i, caps in enumerate(sorted(covers, key=len, reverse=True), 1):
        out |= {"cover%d.%s" % (i, x) for x in caps}
    out |= {"switch%d" % i for i in range(1, switches + 1)}
    return frozenset(out)


def greedy(weights, k):
    """weights: {frozenset: n}. Returns [(class set, definitions newly covered, dead blocks summed)]."""
    sets = [s for s in weights if s]
    todo = dict((s, weights[s]) for s in sets)
    covers = {}
    for c in sets:
        covers[c] = [d for d in sets if d <= c and len(c) - len(d) <= k]
    chosen = []
    while todo:
        best, gain = None, -1
        for c, ds in covers.items():
            g = sum(todo.get(d, 0) for d in ds)
            if g > gain or (g == gain and best is not None and len(c) < len(best)):
                best, gain = c, g
        dead = sum(todo[d] * (len(best) - len(d)) for d in covers[best] if d in todo)
        chosen.append((best, gain, dead))
        for d in covers[best]:
            todo.pop(d, None)
    return chosen


def short(c):
    return " ".join(sorted(c))



def merge_report(rows, show, top):
    w = collections.Counter(blocks(r) for r in rows)
    none = w.pop(frozenset(), 0)
    total = sum(w.values())
    print("\n-- superset classes: a class may carry up to k function blocks the device does not have")
    print("%d definitions, %d of them without a function at run time, %d different function sets, %d different blocks"
          % (len(rows), none, len(w), len(set().union(*w))))
    print("%-3s %8s   %-28s %s" % ("k", "classes", "classes for 80/90/95/99 %", "unused blocks per device (mean)"))
    for k in (0, 1, 2, 3, 4, 6):
        ch = greedy(w, k)
        need, got, i = [], 0, 0
        marks = [.8, .9, .95, .99]
        for j, (_, g, _) in enumerate(ch, 1):
            got += g
            while i < len(marks) and got >= marks[i] * total:
                need.append(j)
                i += 1
        print("%-3d %8d   %-28s %.2f" % (k, len(ch), " / ".join(map(str, need)), sum(x[2] for x in ch) / total))
        if k == show:
            got = 0
            for j, (c, g, dead) in enumerate(ch[:top], 1):
                got += g
                print("      %3d. %5d (%5.1f%%)  unused %.2f  %s"
                      % (j, g, 100.0 * got / total, dead / g if g else 0, short(c)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--vendor")
    ap.add_argument("--members", action="store_true")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--merge", action="store_true", help="also count superset classes (greedy cover)")
    ap.add_argument("--show", type=int, default=-1, help="with --merge: list the classes chosen for this k")
    a = ap.parse_args()
    rows = [json.loads(line) for line in open(a.dump, encoding="utf-8")]
    bad = [r for r in rows if "error" in r]
    rows = [r for r in rows if "error" not in r]
    failed = [r for r in rows if str(r.get("exposes_note", "")).startswith("function failed")]
    partial = [r for r in rows if r.get("exposes_note") == "function, called without device"]
    print("dump: %d definitions, %d not prepared, %d whose exposes need a paired device and came out empty, "
          "%d whose exposes were built without a device (may be incomplete), %d white-label names on top"
          % (len(rows) + len(bad), len(bad), len(failed), len(partial), sum(len(r["whiteLabel"]) for r in rows)))
    print("empty by vendor:", collections.Counter(r["vendor"] for r in failed).most_common(12))
    tuya = [r for r in rows if any(c.startswith("manuSpecificTuya") for c in r["fromZigbee_clusters"])]
    manu = [r for r in rows if any(c.startswith(("manuSpecific", "custom")) or "Cluster" in c
                                   for c in r["fromZigbee_clusters"])]
    print("definitions that decode a Tuya datapoint cluster: %d; any manufacturer-specific cluster: %d"
          % (len(tuya), len(manu)))
    usable = [r for r in rows if r not in failed]
    if a.vendor:
        sel = [r for r in usable if r["vendor"] == a.vendor]
        report(a.vendor, sel, a.top, a.members)
        if a.merge:
            merge_report(sel, a.show, a.top)
    else:
        report("all vendors", usable, a.top, a.members)
        std = [r for r in usable if r not in tuya]
        report("all vendors without Tuya datapoint devices", std, 0, False)
        if a.merge:
            merge_report(usable, a.show, a.top)


if __name__ == "__main__":
    sys.exit(main())
