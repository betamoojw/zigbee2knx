#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dirk Tostmann
"""z2m_to_knx — gateway templates (knxgw-template/0) per device CLASS, from distilled Zigbee2MQTT
definitions (distill/dump_z2m.cjs).

  z2m_to_knx.py DUMP.jsonl [--vendor IKEA] [--out-dir templates] [--report]

One KNX application per class, not per model: every model of a class shares the
application number, the objects and the parameters; what differs between models lives in a model table
inside the class's Berry, looked up at run time by the device's zigbeeModel. Ranges the lamp can report
itself (ZCL colorTempPhysicalMin/Max, 0x400B/0x400C) take precedence over the table.

Classes (KNX Standard 07_20_02 Lighting Actuators):
  zigbee-dimmable-light       FB Dimming Actuator Basic (p. 37, 55)                        app 0xC001
  zigbee-white-spectrum-light FB Dimming Actuator Basic + FB Colour Temperature Actuator (p. 98-99)  app 0xC002
  zigbee-switch               one switched output (plug, relay): Switch, Info On Off, Identify  app 0xC003
  zigbee-remote               a remote with on/off and dimming buttons as a KNX push button          app 0xC004
A model whose definition does not fit a class exactly (colour, other value sets, several endpoints, a
switch with more than switching) is reported and left out. Every class gets a product per model and per
white-label name, so that ETS finds a plug under the number on its box (IKEA E2204, Zigbee2MQTT model E22x4).
Every class has the object "Available": whether the gateway reaches the Zigbee device (dp_event "availability" on
endpoint 0, firmware main/avail.h); its number is fixed per class (CLASSES).

Versions: the template version is the application version. A class keeps its object numbers and parameter ids
(P-n = position in the parameter list, so new parameters only ever go behind the existing ones) with their meaning
across versions, and a new version replaces every lower version given out: knx.application.replaces comes from
producer/issued.json, and tests/test_issued.py holds the generated producer inputs against that list.
Catalog: every product lies in the section Zigbee2KNX, in the subsection of its class (CLASSES "section").

Radio side in Zigbee2MQTT terms: datapoint "<endpoint>.<property path>"; endpoint 0 carries the device
identity ("0.zigbeeModel") and its availability ("0.availability"). The Berry is written as it is embedded:
without comments, indentation and blank lines (shorten_berry); the commented source is the text in this file.
Generated files are written atomically and read back (NFS).
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import json
import os
import sys

GEN = "distill/z2m_to_knx.py v4"
Z2M_VERSION = "26.57.0"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISSUED = os.path.join(ROOT, "producer", "issued.json")
BUS_CURRENT_MA = 0                       # a virtual device draws nothing from the bus; the gateway's hardware does
# BusCurrent is not registration-relevant, so the hardware ids stay; versions given out before carried 10 mA under
# the same ids. NonRegRelevantDataVersion 1 marks the changed data, so that an import takes it over.
HW_NONREG_VERSION = 1
# The catalog: one section for the products of this project, a subsection per kind of device (ids
# M-00FA_CS-<number>, M-00FA_CS-<number>-<number>; ETS merges a section of the same id across product files).
CATALOG_TOP = {"number": "Zigbee2KNX", "name": "Zigbee2KNX"}
SECTIONS = {"gateway": {"number": "Gateway", "name": "Gateway"}, "lights": {"number": "Lights", "name": "Lights"},
            "plugs": {"number": "PlugsSwitches", "name": "Plugs and switches"},
            "remotes": {"number": "Remotes", "name": "Remotes"}}
# Steps of DPT 3.007 (KNX 03_07_02): step code 1..7 = 1/2^(code-1) of the range, 0 = stop. A remote's Zigbee Step
# command becomes the step code nearest to its step size on a logarithmic scale: the geometric means of
# 254/2^(code-1) and the next one are the limits (step size >= 180: code 1, >= 90: code 2, ...).
STEP_LIMITS = [180, 90, 45, 23, 12, 6]
POWER_ON = ["off", "on", "toggle", "previous"]
LEVEL_START = ["minimum", "previous"]
CT_PREVIOUS = 65535                      # color_temp_startup preset "previous" (Zigbee2MQTT definition)
MIRED_DEFAULT = (153, 500)               # class fallback when neither the lamp nor the table gives a range
K_RANGE = (1000, 10000)                  # ETS range of the Kelvin parameters (the lamp limits at run time)
# model table flags
F_LEVEL_START, F_IDENTIFY, F_CT_START, F_FREEZE = 1, 2, 4, 8
# switch class: model table entry [flags, child lock write]; flags 1 power-on behaviour, 2 identify, 16 child lock
S_POB, S_IDENTIFY, S_CHILD_LOCK = 1, 2, 16
SWITCH_EXTRAS = {"power_on_behavior", "identify", "child_lock", "led_enable", "linkquality"}
# Where a child lock lives, by the cluster the definition decodes (facts from zigbee-herdsman-converters
# lib/ikea.js 26.57.0: manuSpecificIkeaSmartPlug 0xFC85, childLock 0x0000 BOOLEAN, manufacturer IKEA_OF_SWEDEN
# 4476 = 0x117C). The value follows as the last field of the "zcl.write" datapoint (gateway zlink.c).
CHILD_LOCK = {"manuSpecificIkeaSmartPlug": "fc85,0000,10,117c"}
# remote class: the actions a remote may have so that its buttons are On/Off and Level commands (ZCL clusters 0x0006,
# 0x0008), which the gateway hands to the template as "zcl.cmd". Multi-click, arrows and scene buttons are left out.
REMOTE_ACTIONS = {"on", "off", "toggle", "brightness_move_up", "brightness_move_down", "brightness_stop",
                  "brightness_step_up", "brightness_step_down", "brightness_move_to_level"}
REMOTE_EXTRAS = {"action", "battery", "battery_low", "voltage", "identify", "linkquality"}
# remote class: model table entry [flags, firmware version]; flags 1 battery, 2 reports BatteryPercentageRemaining in
# percent instead of half percent, 4 does so below the firmware version of the entry (semver, as Zigbee2MQTT compares).
R_BATTERY, R_PERCENT, R_PERCENT_BELOW = 1, 2, 4
# Battery in percent, by Zigbee2MQTT model (facts from zigbee-herdsman-converters 26.57.0): devices/ikea.ts gives
# ICTC-G-1 m.battery({dontDividePercentage: true}); the definitions with ikeaBattery() (lib/ikea.ts) report percent
# below firmware 2.4.0, E2103 below 24.4.13; every other definition reports half percent (ZCL).
BATTERY_PERCENT = {"ICTC-G-1"}
IKEA_BATTERY = {"E1524/E1810", "E1525/E1745", "E1743", "E1744", "E1757", "E1766", "E1812", "E1841", "E1926",
                "E2001/E2002/E2313", "E2102", "E2103"}
IKEA_BATTERY_FIXED = {"E2103": [24, 4, 13]}
IKEA_BATTERY_FIXED_DEFAULT = [2, 4, 0]

# version: the application's and the template's; replaces (knx.application.replaces, put into ReplacesVersions by
# distill/knxprod_products.py): every lower version given out (producer/issued.json). available: the number of the
# object "Available", fixed once given out. Light classes: version 1 carried the template in text parameters
# (FE20/FE21), version 2 as segment data (C001/C002), version 3 adds the object "Available"; switch and remote:
# version 2 adds it.
CLASSES = {
    "dimmable": {"dir": "zigbee-dimmable-light", "app": 0xC001, "name": "Zigbee dimmable light",
                 "order": "ZB-LIGHT-DIM", "version": 3, "available": 7, "section": "lights"},
    "switch": {"dir": "zigbee-switch", "app": 0xC003, "name": "Zigbee switch", "order": "ZB-SWITCH",
               "version": 3, "available": 4, "section": "plugs"},
    "remote": {"dir": "zigbee-remote", "app": 0xC004, "name": "Zigbee remote", "order": "ZB-REMOTE",
               "version": 2, "available": 6, "section": "remotes"},
    "white_spectrum": {"dir": "zigbee-white-spectrum-light", "app": 0xC002, "name": "Zigbee white spectrum light",
                       "order": "ZB-LIGHT-WS", "version": 4, "available": 11, "section": "lights"},
}


def replaces(app: int, version: int, path: str = ISSUED) -> list:
    """Every version of the application given out below `version` (producer/issued.json)."""
    with open(path, encoding="utf-8") as f:
        issued = json.load(f)
    return sorted(e["version"] for e in issued["applications"] if e["number"] == app and e["version"] < version)


class GenError(Exception):
    pass


def shorten_berry(src: str) -> str:
    """The Berry as it is embedded: without comments, indentation, blank lines and runs of blanks (shorter
    downloads). One statement per line stays, so the line of an error message is still a line of this text. String
    literals are copied as they are; Berry's block comments (#- -#) and strings left open are refused rather than
    guessed at. kgt.minify_berry (tools/kgt.py), which the embedding applies on top, finds nothing more to drop."""
    out = []
    for n, line in enumerate(src.splitlines(), 1):
        buf, i, quote = [], 0, None
        while i < len(line):
            c = line[i]
            if quote:
                buf.append(c)
                if c == "\\" and i + 1 < len(line):
                    buf.append(line[i + 1])
                    i += 1
                elif c == quote:
                    quote = None
            elif c in "'\"":
                quote = c
                buf.append(c)
            elif c == "#":
                if line[i + 1:i + 2] == "-":
                    raise GenError(f"Berry line {n}: block comment, not shortened")
                break
            elif c in " \t":
                if buf and buf[-1] != " ":
                    buf.append(" ")
            else:
                buf.append(c)
            i += 1
        if quote:
            raise GenError(f"Berry line {n}: string not closed on its line")
        s = "".join(buf).strip()
        if s:
            out.append(s)
    return "\n".join(out) + "\n"


def avail_object(spec: dict, sleepy: bool = False) -> dict:
    """Object "Available", with the number the class keeps. A sleepy device (battery, receiver off when idle) is never
    asked: the object says whether a frame came in the last 25 h (firmware avail.h)."""
    if sleepy:
        return {"number": spec["available"], "name": "Available", "text": "Zigbee device heard in the last 25 h",
                "function": "Status", "size": "1 Bit", "flags": "CRT", "dpt": "1.002",
                "zigbee": "0.availability <- gateway (firmware avail.h): a battery device sleeps and is never asked; "
                          "true from its first frame, false after 25 h without one (also a remote that is only not "
                          "used); no value after a start of the gateway until its first frame"}
    return {"number": spec["available"], "name": "Available", "text": "Zigbee device reachable", "function": "Status",
            "size": "1 Bit", "flags": "CRT", "dpt": "1.002",
            "zigbee": "0.availability <- gateway (firmware avail.h): true while the device answers, false when not; "
                      "the first true after linking only fills the shadow"}


def knx_part(spec: dict, objects: list, ptypes: dict, seg: int, plist: list) -> dict:
    """manifest["knx"] of a class: application (with the versions it replaces), hardware, catalog, objects,
    parameters, embedding as segment data."""
    numbers = [o["number"] for o in objects]
    if len(set(numbers)) != len(numbers) or [o["number"] for o in objects if o["name"] == "Available"] != [spec["available"]]:
        raise GenError(f"{spec['dir']}: object numbers {numbers}, 'Available' must be {spec['available']}")
    return {
        "manufacturer": "M-00FA",
        "language": "en",
        "application": {"name": spec["name"], "number": spec["app"], "version": spec["version"],
                        "replaces": replaces(spec["app"], spec["version"]), "mask": "MV-07B0",
                        "order_info": spec["order"][:10]},
        "hardware": {"name": spec["name"], "serial_number": str(spec["app"]), "version": 1,
                     "bus_current_ma": BUS_CURRENT_MA, "nonreg_version": HW_NONREG_VERSION,
                     "order_number": spec["order"]},
        "catalog": {"sections": [CATALOG_TOP, SECTIONS[spec["section"]]], "item_number": "1"},
        "objects": sorted(objects, key=lambda o: o["number"]),
        "parameter_types": ptypes,
        "parameters": {"segment_size": seg, "block": {"name": "General", "text": spec["name"]}, "list": plist},
        "embed": {"carrier": "data"},
    }


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
    raise GenError(f"{path}: written three times, read back differently")


def find(exposes: list, name: str):
    for e in exposes:
        if e.get("name") == name or e.get("property") == name:
            return e
        hit = find(e.get("features", []), name)
        if hit:
            return hit
    return None


def classify(r: dict) -> tuple[str, list]:
    """(class, model table entry [min_on, mired_min, mired_max, flags]) or GenError with the reason."""
    ex = r.get("exposes", [])
    lights = [e for e in ex if e.get("type") == "light"]
    if len(lights) != 1:
        raise GenError(f"{len(lights)} light exposes (one expected)")
    feats = lights[0].get("features", [])
    names = {f.get("name") for f in feats}
    if names & {"color_xy", "color_hs"}:
        raise GenError("colour (color_xy/color_hs): FB Colour Actuator not mapped yet")
    by = {f.get("name"): f for f in feats}
    state, bri = by.get("state"), by.get("brightness")
    if not (state and bri):
        raise GenError("light without state and brightness")
    if "endpoint" in state or "endpoint" in bri:
        raise GenError("endpoint-specific light")
    if int(bri.get("value_max", 254)) != 254:
        raise GenError(f"brightness 0..{bri.get('value_max')} (254 expected)")
    pob = find(ex, "power_on_behavior")
    if pob and list(pob.get("values", [])) != POWER_ON:
        raise GenError(f"power_on_behavior values {pob.get('values')}")
    flags = 0
    start = find(ex, "current_level_startup")
    if start:
        if [p["value"] for p in start.get("presets", [])] != LEVEL_START:
            raise GenError("current_level_startup presets differ")
        flags |= F_LEVEL_START
    ident = find(ex, "identify")
    if ident and "identify" in ident.get("values", []):
        flags |= F_IDENTIFY
    if "unfreeze_support" in [o.get("name") for o in r.get("options", [])]:
        flags |= F_FREEZE
    min_on = 2 if r.get("meta", {}).get("turnsOffAtBrightness1") else 1
    ct = by.get("color_temp")
    if not ct:
        return "dimmable", [min_on, 0, 0, flags]
    if ct.get("unit") != "mired":
        raise GenError(f"color_temp in {ct.get('unit')!r}")
    cts = by.get("color_temp_startup")
    if cts:
        prev = [p for p in cts.get("presets", []) if p.get("name") == "previous"]
        if not prev or prev[0].get("value") != CT_PREVIOUS:
            raise GenError("color_temp_startup without previous = 65535")
        flags |= F_CT_START
    return "white_spectrum", [min_on, int(ct["value_min"]), int(ct["value_max"]), flags]


def classify_switch(r: dict) -> tuple[str, list]:
    """("switch", [flags, child lock write]) for one switched output and nothing else, or GenError."""
    ex = r.get("exposes", [])
    sw = [e for e in ex if e.get("type") == "switch"]
    if len(sw) != 1:
        raise GenError(f"{len(sw)} switch exposes (one expected)")
    feats = sw[0].get("features", [])
    if {f.get("name") for f in feats} != {"state"} or "endpoint" in sw[0] or any("endpoint" in f for f in feats):
        raise GenError("switch with more than one state or on another endpoint")
    more = sorted({str(e.get("name")) for e in ex if e is not sw[0] and e.get("name") not in SWITCH_EXTRAS})
    if more:
        raise GenError(f"more than a switch: {', '.join(more)}")
    flags, lock = 0, ""
    pob = find(ex, "power_on_behavior")
    if pob:
        if list(pob.get("values", [])) != POWER_ON:
            raise GenError(f"power_on_behavior values {pob.get('values')}")
        flags |= S_POB
    ident = find(ex, "identify")
    if ident and "identify" in ident.get("values", []):
        flags |= S_IDENTIFY
    if find(ex, "child_lock"):
        hits = [CHILD_LOCK[c] for c in r.get("fromZigbee_clusters", []) if c in CHILD_LOCK]
        if hits:                                   # a child lock elsewhere is left out, the switch stays
            flags |= S_CHILD_LOCK
            lock = hits[0]
    return "switch", [flags, lock]


def classify_remote(r: dict) -> tuple[str, list]:
    """("remote", [flags]) for a remote whose buttons are On/Off and Level commands only, or GenError."""
    ex = r.get("exposes", [])
    act = [e for e in ex if e.get("name") == "action" and e.get("type") == "enum"]
    if len(act) != 1:
        raise GenError(f"{len(act)} action exposes (one expected)")
    values = set(act[0].get("values", []))
    if not values or values - REMOTE_ACTIONS:
        raise GenError(f"buttons other than on/off/dim: {', '.join(sorted(values - REMOTE_ACTIONS))}")
    more = sorted({str(e.get("name")) for e in ex if e.get("name") not in REMOTE_EXTRAS})
    if more:
        raise GenError(f"more than a remote: {', '.join(more)}")
    flags, below = 0, []
    if find(ex, "battery"):
        flags |= R_BATTERY
        if r["model"] in BATTERY_PERCENT:
            flags |= R_PERCENT
        elif r["model"] in IKEA_BATTERY:
            flags |= R_PERCENT_BELOW
            below = IKEA_BATTERY_FIXED.get(r["model"], IKEA_BATTERY_FIXED_DEFAULT)
    return "remote", [flags, below]


BERRY_REMOTE = r'''# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dirk Tostmann
#
# GENERATED by @GEN@: class @CLASS@ (@NMODELS@ models) from Zigbee2MQTT definitions
# (zigbee-herdsman-converters @Z2M@, MIT, facts only). Change the generator, not this file.
#
# KNX: a push button. Switch (KO 1, on/off/toggle), Dimming (KO 2, DPT 3.007: start brighter/darker, stop, a step
# with the step code nearest to the remote's step size), Value (KO 3, DPT 5.001, from "move to level"), Battery
# (KO 4, %), Switch status (KO 5: the state of what the button switches, so toggle sends the opposite), Available
# (KO 6: the gateway heard the remote in the last 25 h). Parameter: invert the switch telegrams.
# Zigbee: the remote's buttons arrive as commands (gateway: datapoint "zcl.cmd" = "cluster,command,payload" in
# hex): On/Off 0x0006 (0 off, 1 on, 2 toggle, 0x40 off with effect, 0x41/0x42 on), Level 0x0008 (0/4 move to
# level, 1/5 move, 2/6 step: mode, step size, transition; 3/7 stop). Battery: BatteryPercentageRemaining as the
# remote reports it ("batteryPercentageRemaining", half percent by ZCL); the model table says which models report
# percent instead, as Zigbee2MQTT reads them, some only below a firmware version (endpoint 0 "swBuildId", Basic
# SWBuildID, compared as a semantic version the way Zigbee2MQTT does). "battery" in percent comes from gateways that
# deliver it already converted.
# Endpoint 0: the identity (zigbeeModel, swBuildId) and the gateway's availability (true/false; a sleeping remote
# counts as gone after 25 h without a frame, firmware avail.h).
import string

class zb_remote
    static KO_SWITCH = 1
    static KO_DIM = 2
    static KO_VALUE = 3
    static KO_BATTERY = 4
    static KO_SWITCH_STATUS = 5
    static KO_AVAILABLE = @KO_AVAIL@
    static F_PERCENT = 2
    static F_PERCENT_BELOW = 4
    static STEP_LIMITS = @STEP_LIMITS@
    static MODELS = @MODELS@
@POFF@
    static def cfg()
        var c = host.state_get('cfg')
        return c == nil ? {'inv': false} : c
    end
    static def model()                          # entry of this device in MODELS, or the class defaults
        var zm = host.dp_get(0, 'zigbeeModel')
        var e = zm != nil ? zb_remote.MODELS.find(zm) : nil
        return e != nil ? e : @DEFAULT_ENTRY@
    end
    static def switch(on)
        host.state_set('sw', on)
        var out = zb_remote.cfg()['inv'] ? !on : on
        host.ko_send(zb_remote.KO_SWITCH, out ? bytes('01') : bytes('00'))
    end
    static def dim(code)
        var b = bytes()
        b.add(code, 1)
        host.ko_send(zb_remote.KO_DIM, b)
    end
    static def step_code(size)                  # Zigbee step size (of 254) -> the nearest DPT 3.007 step code
        var code = 1
        for lim : zb_remote.STEP_LIMITS
            if size < lim  code += 1 end
        end
        return code
    end
    static def ident(s, i, j)                   # s[i..j) is one identifier of a semantic version: letters, digits, '-'
        if i >= j  return false end
        while i < j
            var c = string.byte(s[i])
            if !((c >= 48 && c <= 57) || (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || c == 45)  return false end
            i += 1
        end
        return true
    end
    static def idents(s, i, j)                  # s[i..j): identifiers separated by '.'
        var k = i
        while k <= j
            if k == j || s[k] == '.'
                if !zb_remote.ident(s, i, k)  return false end
                i = k + 1
            end
            k += 1
        end
        return true
    end
    static def version(s)                       # semantic version, loose ("v2.3.075", "2.4.0-beta"): [major, minor, patch,
        if type(s) != 'string'  return nil end  # prerelease] or nil
        var i = 0
        var n = size(s)
        while i < n && string.find(' \t\r\n', s[i]) >= 0  i += 1 end
        while n > i && string.find(' \t\r\n', s[n - 1]) >= 0  n -= 1 end
        while i < n && string.find('v= \t\r\n', s[i]) >= 0  i += 1 end
        var v = []
        for f : 0 .. 2
            var d = 0
            var k = i
            while k < n && string.byte(s[k]) >= 48 && string.byte(s[k]) <= 57
                d = d * 10 + string.byte(s[k]) - 48
                k += 1
            end
            if k == i  return nil end
            v.push(d)
            i = k
            if f < 2
                if i >= n || s[i] != '.'  return nil end
                i += 1
            end
        end
        var plus = string.find(s, '+', i)
        var e = plus >= 0 && plus < n ? plus : n
        if plus >= 0 && plus < n && !zb_remote.idents(s, plus + 1, n)  return nil end
        var pre = false
        if i < e
            if s[i] == '-' && i + 1 == e
                pre = true
            else
                if s[i] == '-'  i += 1 end
                if s[i] == '.' || !zb_remote.idents(s, i, e)  return nil end
                pre = true
            end
        end
        v.push(pre)
        return v
    end
    static def below(v, w)                      # version v lower than w (w without prerelease)
        for f : 0 .. 2
            if v[f] != w[f]  return v[f] < w[f] end
        end
        return v[3]
    end
    static def battery(raw)                     # BatteryPercentageRemaining as reported -> DPT 5.001
        var Z = zb_remote
        var e = Z.model()
        var half = raw
        if (e[0] & Z.F_PERCENT) != 0
            half = raw * 2
        elif (e[0] & Z.F_PERCENT_BELOW) != 0
            var v = Z.version(host.dp_get(0, 'swBuildId'))
            if v != nil && Z.below(v, e[1])  half = raw * 2 end
        end
        var b = bytes()
        b.add(half >= 200 ? 255 : (half * 255 + 100) / 200, 1)
        return b
    end
end

# Group write from KNX: the state of what the button switches
def knx_write(ko, raw)
    var Z = zb_remote
    if ko == Z.KO_SWITCH_STATUS && raw.size() == 1
        host.state_set('sw', (raw[0] & 1) != 0)
    else
        host.log('ko_write_ignored', {'ko': ko, 'value': raw.tohex()})
    end
end

# A command from a button, the battery, or (endpoint 0) whether the gateway hears the remote and what it is
def dp_event(ch, key, value, initial)
    var Z = zb_remote
    if ch == 0
        if key == 'availability'
            host.ko_status(Z.KO_AVAILABLE, value == true ? bytes('01') : bytes('00'), initial)
        elif key == 'zigbeeModel' || key == 'swBuildId'
            var bat = host.state_get('bat')         # the model or its firmware decides how the battery reads
            if bat != nil  host.ko_status(Z.KO_BATTERY, Z.battery(bat[0]), bat[1]) end
        end
        return
    end
    if key == 'batteryPercentageRemaining'
        if type(value) == 'int' && value >= 0 && value < 255
            host.state_set('bat', [value, initial])
            host.ko_status(Z.KO_BATTERY, Z.battery(value), initial)
        end
        return
    end
    if key == 'battery'
        if type(value) == 'int' || type(value) == 'real'
            var b = bytes()
            b.add(value >= 100 ? 255 : value <= 0 ? 0 : int((value * 255 + 50) / 100), 1)
            host.ko_status(Z.KO_BATTERY, b, initial)
        end
        return
    end
    if key != 'zcl.cmd' || value == nil  return end
    var f = string.split(value, ',')
    if size(f) < 2  return end
    var cl = bytes(f[0])
    var cmd = bytes(f[1])
    var p = size(f) > 2 ? bytes(f[2]) : bytes()
    if size(cl) != 2 || size(cmd) != 1  return end
    var cluster = (cl[0] << 8) | cl[1]
    var c = cmd[0]
    if cluster == 0x0006
        if c == 0x00 || c == 0x40
            Z.switch(false)
        elif c == 0x01 || c == 0x41 || c == 0x42
            Z.switch(true)
        elif c == 0x02
            var last = host.state_get('sw')
            Z.switch(last == nil ? true : !last)
        end
    elif cluster == 0x0008
        var up = size(p) > 0 && p[0] == 0
        if (c == 0x00 || c == 0x04) && size(p) > 0
            var b = bytes()
            b.add((p[0] * 255 + 127) / 254, 1)
            host.ko_send(Z.KO_VALUE, b)
        elif c == 0x01 || c == 0x05
            Z.dim(up ? 0x09 : 0x01)
        elif (c == 0x02 || c == 0x06) && size(p) > 1
            if p[1] > 0  Z.dim((up ? 0x08 : 0x00) | Z.step_code(p[1])) end
        elif c == 0x03 || c == 0x07
            Z.dim(0x00)
        end
    else
        host.log('command_ignored', {'cmd': value})
    end
end

# Parameter segment (@SEG@ bytes, manifest knx.parameters)
def params(p)
    if p.size() < @SEG@  return end
    host.state_set('cfg', {'inv': p[zb_remote.P_INV] != 0})
end
'''


def build_remote(members: list) -> tuple[dict, str]:
    """The remote class (a push button): members [(record, [flags])] -> (manifest, berry)."""
    spec = CLASSES["remote"]
    ep = 1
    ptypes = {"YesNo": {"bits": 8, "enum": [[0, "no"], [1, "yes"]]}}
    plist = [{"offset": 0, "name": "InvertSwitch", "text": "Invert the switch telegrams (on button sends 0)",
              "type": "YesNo", "default": 0, "zigbee": "gateway: KO 1 sends the opposite"}]
    poff, seg = {"inv": 0}, 1
    objects = [
        {"number": 1, "name": "Switch", "text": "Switch", "function": "On/Off", "size": "1 Bit", "flags": "CT",
         "dpt": "1.001", "zigbee": "On/Off commands of the remote: on, off, toggle (opposite of KO 5)"},
        {"number": 2, "name": "Dimming", "text": "Relative dimming", "function": "Brighter/Darker", "size": "4 Bit",
         "flags": "CT", "dpt": "3.007",
         "zigbee": "Level move -> start brighter/darker (step code 1, 100 %), stop -> stop (0), step -> the step code "
                   "nearest to the step size (1/2^(code-1) of 254)"},
        {"number": 3, "name": "Value", "text": "Brightness value", "function": "Value", "size": "1 Byte",
         "flags": "CT", "dpt": "5.001", "zigbee": "Level move to level"},
        {"number": 4, "name": "Battery", "text": "Battery", "function": "Percent", "size": "1 Byte", "flags": "CRT",
         "dpt": "5.001",
         "zigbee": "batteryPercentageRemaining (half percent by ZCL; percent for the models with flag 2, and with flag 4 "
                   "below the firmware version of the entry, read from swBuildId)"},
        {"number": 5, "name": "SwitchStatus", "text": "Switch status (for toggle)", "function": "On/Off",
         "size": "1 Bit", "flags": "CWU", "dpt": "1.001", "zigbee": "state of the switched load, so toggle sends the opposite"},
        avail_object(spec, sleepy=True),
    ]
    table = {}
    for r, entry in sorted(members, key=lambda m: m[0]["model"]):
        for zm in r.get("zigbeeModel", []):
            table[zm] = entry
    default_entry = [R_BATTERY, []]
    be = (BERRY_REMOTE.replace("@GEN@", GEN).replace("@CLASS@", spec["dir"]).replace("@NMODELS@", str(len(members)))
          .replace("@Z2M@", Z2M_VERSION).replace("@MODELS@", berry_value(table))
          .replace("@DEFAULT_ENTRY@", berry_value(default_entry)).replace("@STEP_LIMITS@", berry_value(STEP_LIMITS))
          .replace("@POFF@", "".join(f"    static P_{k.upper()} = {v}\n" for k, v in poff.items()))
          .replace("@SEG@", str(seg)).replace("@KO_AVAIL@", str(spec["available"])))
    left = sorted({w.strip("()[],") for w in be.split() if w.strip("()[],").startswith("@") and w.strip("()[],").endswith("@")})
    if left:
        raise GenError(f"remote: placeholders left in the Berry: {left}")
    manifest = {
        "schema": "knxgw-template/0",
        "id": "zigbee.class.remote",
        "version": spec["version"],
        "title": spec["name"] + " as a KNX push button",
        "radio": "zigbee",
        "match": {"class": "remote", "zigbeeModel": sorted(table)},
        "behaviour": {"language": "berry", "file": f"{spec['dir']}.be"},
        "datapoints": {
            "_doc": "<endpoint>.<property>; endpoint 0 = device identity (zigbeeModel, swBuildId: Basic SWBuildID) and "
                    "availability (gateway: true/false). zcl.cmd: 'cluster,command,payload' in hex, a command the remote "
                    "sent to the gateway (gateway zlink.c). batteryPercentageRemaining: the ZCL attribute as reported, "
                    "0..254 (nothing for 0xFF); battery: percent, from gateways that convert it themselves.",
            "write": [],
            "events": ["0.zigbeeModel", "0.swBuildId", "0.availability", f"{ep}.zcl.cmd",
                       f"{ep}.batteryPercentageRemaining", f"{ep}.battery"],
            "master": []},
        "products": products_of(members),
        "generated": {"by": GEN, "from": f"zigbee-herdsman-converters {Z2M_VERSION}", "models": len(members)},
        "sources": [
            f"zigbee-herdsman-converters {Z2M_VERSION} (MIT, (c) Koen Kanters), distilled by distill/dump_z2m.cjs (facts only)",
            f"zigbee-herdsman-converters {Z2M_VERSION} lib/ikea.ts ikeaBattery() and devices/ikea.ts: which models report "
            "BatteryPercentageRemaining in percent (model table flags 2 and 4)",
            "ZCL: On/Off cluster commands 0x00-0x02, 0x40-0x42; Level Control commands 0x00-0x07",
            "KNX DPT 3.007 (dimming control: step code 1..7 = 1/2^(code-1) of the range, 0 = break), 5.001, 1.001",
        ],
        "open": [
            "The gateway binds the remote's On/Off and Level clusters to itself when it links the device; a sleeping "
            "remote may miss that and needs a button press while the gateway asks",
            "Long press and release of the IKEA remotes map to move and stop as Zigbee2MQTT reads them; not seen on air yet",
            "Remotes with arrows, scenes or double clicks are not in this class",
            "Which models report the battery in percent is a table from the Zigbee2MQTT definitions; not checked on a device",
        ],
        "knx": knx_part(spec, objects, ptypes, seg, plist),
    }
    return manifest, be


BERRY_SWITCH = r'''# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dirk Tostmann
#
# GENERATED by @GEN@: class @CLASS@ (@NMODELS@ models) from Zigbee2MQTT definitions
# (zigbee-herdsman-converters @Z2M@, MIT, facts only). Change the generator, not this file.
#
# KNX: one switched output, like FB Light Switching Actuator Basic (KNX Standard 07_20_02): Switch On Off
# (KO 1), Info On Off (KO 2); KO 3 (DPT 1.017 trigger: 0 and 1 alike) starts the device's identify effect if it has
# one; KO 4 Available: the gateway reaches the device. Parameters: behaviour after mains power returns (models with
# flag 1), child lock (models with flag 16).
# Zigbee: Zigbee2MQTT properties on endpoint 1; endpoint 0 carries the identity (zigbeeModel) and the gateway's
# availability (true/false). What differs between models is in MODELS, looked up by zigbeeModel: [flags, child
# lock write]; flags 1 power-on behaviour, 2 identify, 16 child lock. The child lock is written as "zcl.write"
# (cluster, attribute, type, manufacturer, value in hex), so the knowledge about the device stays in this template.
# These model settings wait for the model: params() keeps the parameters, and they are written as soon as the
# device has told its zigbeeModel, once per model (a gateway asks the model after it hands over the parameters).
class zb_switch
    static KO_SWITCH = 1
    static KO_SWITCH_STATUS = 2
    static KO_IDENTIFY = 3
    static KO_AVAILABLE = @KO_AVAIL@
    static EP = 1
    static POWER_ON = ['off', 'on', 'toggle', 'previous']
    static F_POB = 1
    static F_IDENTIFY = 2
    static F_CHILD_LOCK = 16
    static MODELS = @MODELS@
@POFF@
    static def model()                          # entry of this device in MODELS, or the class defaults
        var zm = host.dp_get(0, 'zigbeeModel')
        var e = zm != nil ? zb_switch.MODELS.find(zm) : nil
        return e != nil ? e : @DEFAULT_ENTRY@
    end
    static def has(flag)
        return (zb_switch.model()[0] & flag) != 0
    end
    static def clamp(v, lo, hi)
        if v < lo  return lo end
        if v > hi  return hi end
        return v
    end
    static def apply()                          # model settings from the last parameters, once per known model
        var Z = zb_switch
        var p = host.state_get('p')
        var zm = host.dp_get(0, 'zigbeeModel')
        if p == nil || zm == nil || host.state_get('pm') == zm  return end
        host.state_set('pm', zm)
        if Z.has(Z.F_POB)
            host.dp_set(Z.EP, 'power_on_behavior', Z.POWER_ON[Z.clamp(p[Z.P_POB], 0, 3)])
        end
        var e = Z.model()
        if Z.has(Z.F_CHILD_LOCK) && e[1] != ''
            host.dp_set(Z.EP, 'zcl.write', e[1] + (p[Z.P_LOCK] != 0 ? ',1' : ',0'))
        end
    end
end

# Group write from KNX (raw: the object value as bytes)
def knx_write(ko, raw)
    var Z = zb_switch
    if ko == Z.KO_SWITCH && raw.size() == 1
        host.dp_set(Z.EP, 'state', (raw[0] & 1) != 0 ? 'ON' : 'OFF')
    elif ko == Z.KO_IDENTIFY && raw.size() == 1
        if Z.has(Z.F_IDENTIFY)  host.dp_set(Z.EP, 'identify', 'identify') end
    else
        host.log('ko_write_ignored', {'ko': ko, 'value': raw.tohex()})
    end
end

# A property value from the device (an event, or read at start: initial); endpoint 0: identity, availability
def dp_event(ch, key, value, initial)
    var Z = zb_switch
    if ch == 0
        if key == 'availability'
            host.ko_status(Z.KO_AVAILABLE, value == true ? bytes('01') : bytes('00'), initial)
        elif key == 'zigbeeModel'
            Z.apply()
        end
        return
    end
    if ch != Z.EP  return end
    if key == 'state'
        host.ko_status(Z.KO_SWITCH_STATUS, value == 'ON' ? bytes('01') : bytes('00'), initial)
    end
end

# Parameter segment (@SEG@ bytes, manifest knx.parameters)
def params(p)
    var Z = zb_switch
    if p.size() < @SEG@  return end
    host.state_set('p', p)
    host.state_set('pm', nil)                   # new parameters: written again for the model the device has
    Z.apply()
    host.log('zb_params', {'pob': p[Z.P_POB], 'lock': p[Z.P_LOCK], 'wait_model': host.dp_get(0, 'zigbeeModel') == nil})
end
'''


BERRY = r'''# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dirk Tostmann
#
# GENERATED by @GEN@: class @CLASS@ (@NMODELS@ models) from Zigbee2MQTT definitions
# (zigbee-herdsman-converters @Z2M@, MIT, facts only). Change the generator, not this file.
#
# KNX: FB Dimming Actuator Basic (KNX Standard 07_20_02, p. 37 and 55). Inputs Switch On Off (KO 1),
# Relative Setvalue Control (KO 2), Absolute Setvalue Control (KO 3); outputs Info On Off (KO 4) and
# Actual Dimming Value (KO 5, 0 while off); KO 6 (DPT 1.017 trigger: 0 and 1 alike) starts the lamp's identify
# effect if it has one. The parameters switch-on value (OSV), memory function (MF), minimum/maximum value (MIN_SV,
# MAX_SV) and relative off (ROE) are carried out here. KO @KO_AVAIL@ is Available: the gateway reaches the lamp.
# Relative dimming (DPT 3.007) as in the state tables of FB Dimming Actuator Basic (p. 44-46): step code 1..7 moves
# the set value by 1/2^(code-1) of 0..100 % within MIN_SV..MAX_SV at the dimming rate, 0 stops where the lamp is.
# While a command runs, the next step counts from its target; then from what the lamp reports. Up from off switches
# on at MIN_SV; down from off does nothing; with ROE a step down below MIN_SV dims to off.
# Zigbee: Zigbee2MQTT properties on endpoint 1; endpoint 0 carries the identity (zigbeeModel) and the
# gateway's availability (true/false).
# One application serves every model of the class. What differs between models is in MODELS,
# looked up by zigbeeModel: [min_on, mired_min, mired_max, flags]; flags 1 startup level, 2 identify,
# 4 startup colour temperature, 8 freezing bug. An unknown model gets the class defaults.
@CT_DOC@
class zb_light
    static KO_SWITCH = 1
    static KO_DIM = 2
    static KO_VALUE = 3
    static KO_SWITCH_STATUS = 4
    static KO_VALUE_STATUS = 5
    static KO_IDENTIFY = 6
    static KO_AVAILABLE = @KO_AVAIL@
    static EP = 1
    static BRI_MAX = 254
    static SETTLE_MS = 1000                     # the gateway reads the lamp back 0.7 s after a command has run
    static POWER_ON = ['off', 'on', 'toggle', 'previous']
    static START = ['minimum', 'previous']
    static F_LEVEL_START = 1
    static F_IDENTIFY = 2
    static F_CT_START = 4
    static F_FREEZE = 8
    static MODELS = @MODELS@
@CT_CONST@@POFF@
    static def defaults()
        return @DEFAULTS@
    end
    static def cfg()
        var c = host.state_get('cfg')
        return c == nil ? zb_light.defaults() : c
    end
    static def model()                          # entry of this device in zb_models, or the class defaults
        var zm = host.dp_get(0, 'zigbeeModel')
        var e = zm != nil ? zb_light.MODELS.find(zm) : nil
        return e != nil ? e : @DEFAULT_ENTRY@
    end
    static def has(flag)
        return (zb_light.model()[3] & flag) != 0
    end
    static def clamp(v, lo, hi)
        if v < lo  return lo end
        if v > hi  return hi end
        return v
    end
    static def pct_to_bri(pct)                  # 1..100 % -> brightness, never below the model's minimum on
        var Z = zb_light
        return Z.clamp((pct * Z.BRI_MAX + 50) / 100, Z.model()[0], Z.BRI_MAX)
    end
    static def bri_byte(bri)                    # brightness -> DPT 5.001 (0..255)
        var b = bytes()
        b.add((bri * 255 + zb_light.BRI_MAX / 2) / zb_light.BRI_MAX, 1)
        return b
    end
    static def transition(c)
        if c['trans'] > 0
            host.dp_set(zb_light.EP, 'transition', real(c['trans']) / 10)
        end
    end
    static def level()                          # set value in brightness steps, 0 = off: the target of the last command
        var Z = zb_light                        # while it runs and until the lamp has reported after it, else the report
        var m = host.state_get('lv')
        if m != nil && host.now_ms() < m[2] + m[3] + Z.SETTLE_MS  return m[1] end
        if host.dp_get(Z.EP, 'state') != 'ON'  return 0 end
        var b = host.dp_get(Z.EP, 'brightness')
        return b == nil ? 0 : b
    end
    static def moving(key, at, to, tenths)      # remember a command: from, target, start, duration in ms
        host.state_set(key, [at, to, host.now_ms(), tenths * 100])
    end
    static def stopped(key)                     # a stop: where the running command has got to counts as the set value
        var m = host.state_get(key)
        var now = host.now_ms()
        if m != nil && now < m[2] + m[3]
            var at = m[0] + (m[1] - m[0]) * (now - m[2]) / m[3]
            host.state_set(key, [at, at, now, 0])
        end
    end
    static def set_bri(c, bri)                  # within MIN_SV..MAX_SV, then on
        var Z = zb_light
        var lo = Z.pct_to_bri(c['min'])
        var hi = Z.pct_to_bri(c['max'])
        var to = Z.clamp(bri, lo, hi)
        Z.moving('lv', Z.level(), to, c['trans'])
        Z.transition(c)
        host.dp_set(Z.EP, 'brightness', to)
    end
    static def off(c)
        var Z = zb_light
        Z.moving('lv', Z.level(), 0, c['trans'])
        Z.transition(c)
        host.dp_set(Z.EP, 'state', 'OFF')
    end
    static def step(c, raw)                     # KO 2, DPT 3.007
        var Z = zb_light
        var code = raw & 0x07
        if code == 0
            Z.stopped('lv')
            host.dp_set(Z.EP, c['roe'] ? 'brightness_move_onoff' : 'brightness_move', 'stop')
            return
        end
        var lo = Z.pct_to_bri(c['min'])
        var hi = Z.pct_to_bri(c['max'])
        var d = (Z.BRI_MAX * 2 / (1 << (code - 1)) + 1) / 2
        var at = Z.level()
        var to = 0
        if (raw & 0x08) != 0
            if at == 0                          # off: on at MIN_SV, then up by the step
                host.dp_set(Z.EP, 'brightness', lo)
                at = lo
            end
            to = Z.clamp(at + d, lo, hi)
        elif at == 0
            return
        elif c['roe'] && at - d < lo            # ROE: below MIN_SV the lamp dims to off
            Z.moving('lv', at, 0, (at * 10 + c['rate'] / 2) / c['rate'])
            host.dp_set(Z.EP, 'brightness_move_onoff', -c['rate'])
            return
        else
            to = Z.clamp(at - d, lo, hi)
        end
        if to == at
            Z.moving('lv', at, to, 0)
            return
        end
        var t = ((to > at ? to - at : at - to) * 10 + c['rate'] / 2) / c['rate']
        Z.moving('lv', at, to, t)
        if t > 0  host.dp_set(Z.EP, 'transition', real(t) / 10) end
        host.dp_set(Z.EP, 'brightness', to)
    end
@CT_HELPERS@end

# Group write from KNX (raw: the object value as bytes)
def knx_write(ko, raw)
    var Z = zb_light
    var c = Z.cfg()
    if ko == Z.KO_SWITCH && raw.size() == 1
        if (raw[0] & 1) != 0
            var bri = Z.pct_to_bri(c['osv'])
            var last = host.dp_get(Z.EP, 'brightness')
            if c['mf'] && last != nil && last > 0  bri = last end
@CT_ON@            Z.set_bri(c, bri)
        else
            Z.off(c)
        end
    elif ko == Z.KO_VALUE && raw.size() == 1
        if raw[0] == 0
            Z.off(c)
        else
            Z.set_bri(c, Z.clamp((raw[0] * Z.BRI_MAX + 127) / 255, Z.model()[0], Z.BRI_MAX))
        end
    elif ko == Z.KO_DIM && raw.size() == 1
        Z.step(c, raw[0])
    elif ko == Z.KO_IDENTIFY && raw.size() == 1
        if Z.has(Z.F_IDENTIFY)  host.dp_set(Z.EP, 'identify', 'identify') end
@CT_WRITE@    else
        host.log('ko_write_ignored', {'ko': ko, 'value': raw.tohex()})
    end
end

# A property value from the device (an event, or read at start: initial); endpoint 0: identity, availability
def dp_event(ch, key, value, initial)
    var Z = zb_light
    if ch == 0
        if key == 'availability'
            host.ko_status(Z.KO_AVAILABLE, value == true ? bytes('01') : bytes('00'), initial)
        end
        return
    end
    if ch != Z.EP  return end
    if key == 'state'
        var on = value == 'ON'
        host.ko_status(Z.KO_SWITCH_STATUS, on ? bytes('01') : bytes('00'), initial)
        var b = host.dp_get(Z.EP, 'brightness')
        if !on
            host.ko_status(Z.KO_VALUE_STATUS, bytes('00'), initial)
        elif b != nil
            host.ko_status(Z.KO_VALUE_STATUS, Z.bri_byte(b), initial)
        end
    elif key == 'brightness'
        if host.dp_get(Z.EP, 'state') != 'OFF'
            host.ko_status(Z.KO_VALUE_STATUS, Z.bri_byte(value), initial)
        end
@CT_EVENT@    end
end

# Parameter segment (@SEG@ bytes, manifest knx.parameters)
def params(p)
    var Z = zb_light
    if p.size() < @SEG@  return end
    var c = {'osv': Z.clamp(p[Z.P_OSV], 1, 100), 'mf': p[Z.P_MF] != 0, 'min': Z.clamp(p[Z.P_MIN], 1, 100),
             'max': Z.clamp(p[Z.P_MAX], 1, 100), 'roe': p[Z.P_ROE] != 0, 'trans': p[Z.P_TRANS],
             'rate': Z.clamp(p[Z.P_RATE], 1, 255)}
    if c['min'] > c['max']  c['min'] = c['max'] end
@CT_CFG@    host.state_set('cfg', c)
    host.dp_set(Z.EP, 'power_on_behavior', Z.POWER_ON[Z.clamp(p[Z.P_POB], 0, 3)])
    if Z.has(Z.F_LEVEL_START)
        if p[Z.P_START] < Z.START.size()
            host.dp_set(Z.EP, 'level_config.current_level_startup', Z.START[p[Z.P_START]])
        else
            host.dp_set(Z.EP, 'level_config.current_level_startup', Z.pct_to_bri(Z.clamp(p[Z.P_STARTVAL], 1, 100)))
        end
    end
@CT_PARAMS@    host.log('zb_params', c)
end
'''

CT_DOC = """# Colour temperature: FB Colour Temperature Actuator (07_20_02 p. 98-99). Absolute (KO 7, K),
# relative (KO 8), current (KO 9, K), out of range (KO 10); parameters MINCT/MAXCT, CTSOB (20.608),
# CTSOSV, CTFMS. The usable range is the lamp's: colorTempPhysicalMin/Max as the lamp reports them
# (ZCL 0x400B/0x400C), else the model table, else 153..500 mired. Zigbee2MQTT works in mired
# (K = 1000000 / mired); a KNX increase (more K, cooler) is a negative color_temp_move.
# Relative (p. 94-95): step code 1 fades until a stop (color_temp_move, up to the lamp's own limits), 2..7 move the
# set value by 1/2^(code-1) of MINCT..MAXCT at the colour temperature rate, 0 stops where the lamp is.
# Models with flag 8 freeze on a colour change with a transition (Zigbee2MQTT issue 18574): no transition, a step
# jumps."""

CT_CONST = """    static KO_CT = 7
    static KO_CT_REL = 8
    static KO_CT_STATUS = 9
    static KO_CT_RANGE = 10
    static MIRED_DEFAULT_MIN = @MD_MIN@
    static MIRED_DEFAULT_MAX = @MD_MAX@
    static CT_PREVIOUS = 65535
"""

CT_HELPERS = """    static def k_word(p, o)                     # 16-bit parameter, big endian
        return (p[o] << 8) | p[o + 1]
    end
    static def mired_to_k(m)
        return (1000000 + m / 2) / m
    end
    static def ct_range()                       # mired: lamp's report > model table > class default
        var Z = zb_light
        var lo = host.dp_get(Z.EP, 'colorTempPhysicalMin')
        var hi = host.dp_get(Z.EP, 'colorTempPhysicalMax')
        if lo != nil && hi != nil && lo > 0 && hi < 0xFEFF && lo < hi  return [lo, hi] end
        var e = Z.model()
        if e[1] > 0  return [e[1], e[2]] end
        return [Z.MIRED_DEFAULT_MIN, Z.MIRED_DEFAULT_MAX]
    end
    static def k_bytes(k)                       # DPT 7.600: 2 bytes, big endian
        var b = bytes()
        b.add((k >> 8) & 0xFF, 1)
        b.add(k & 0xFF, 1)
        return b
    end
    static def ct_limits(c)                     # K: MINCT..MAXCT within the lamp's range
        var Z = zb_light
        var r = Z.ct_range()
        var lo = Z.clamp(c['ctmin'], Z.mired_to_k(r[1]), Z.mired_to_k(r[0]))
        var hi = Z.clamp(c['ctmax'], Z.mired_to_k(r[1]), Z.mired_to_k(r[0]))
        if lo > hi  lo = hi end
        return [lo, hi]
    end
    static def ct_now()                         # set value in K: the target of the last command while it runs and until
        var Z = zb_light                        # the lamp has reported after it, else the report; nil: not known
        var m = host.state_get('ctm')
        if m != nil && host.now_ms() < m[2] + m[3] + Z.SETTLE_MS  return m[1] end
        var v = host.dp_get(Z.EP, 'color_temp')
        return v != nil && v > 0 ? Z.mired_to_k(v) : nil
    end
    static def set_ct(c, k)                     # within MINCT..MAXCT and the lamp; out of range on KO 10
        var Z = zb_light
        var r = Z.ct_range()
        var lim = Z.ct_limits(c)
        var kk = Z.clamp(k, lim[0], lim[1])
        host.ko_status(Z.KO_CT_RANGE, kk != k ? bytes('01') : bytes('00'), false)
        var fade = c['ctfade'] && !Z.has(Z.F_FREEZE)
        Z.moving('ctm', kk, kk, fade ? c['trans'] : 0)
        if fade  Z.transition(c) end
        host.dp_set(Z.EP, 'color_temp', Z.clamp((1000000 + kk / 2) / kk, r[0], r[1]))
    end
    static def ct_step(c, raw)                  # KO 8, DPT 3.007: up = more K, fewer mired
        var Z = zb_light
        var code = raw & 0x07
        var up = (raw & 0x08) != 0
        var r = Z.ct_range()
        if code == 0
            Z.stopped('ctm')
            host.dp_set(Z.EP, 'color_temp_move', 'stop')
            return
        end
        var k = Z.ct_now()
        if code == 1                            # until a stop or the lamp's limit
            if k != nil
                var a = Z.clamp((1000000 + k / 2) / k, r[0], r[1])
                var b = up ? r[0] : r[1]
                Z.moving('ctm', k, Z.mired_to_k(b), ((a > b ? a - b : b - a) * 10 + c['ctrate'] / 2) / c['ctrate'])
            end
            host.dp_set(Z.EP, 'color_temp_move', up ? -c['ctrate'] : c['ctrate'])
            return
        end
        var lim = Z.ct_limits(c)
        if k == nil  k = (lim[0] + lim[1]) / 2 end
        var d = ((lim[1] - lim[0]) * 2 / (1 << (code - 1)) + 1) / 2
        var kk = Z.clamp(up ? k + d : k - d, lim[0], lim[1])
        if kk == k
            Z.moving('ctm', k, k, 0)
            return
        end
        var a = Z.clamp((1000000 + k / 2) / k, r[0], r[1])
        var b = Z.clamp((1000000 + kk / 2) / kk, r[0], r[1])
        var t = Z.has(Z.F_FREEZE) ? 0 : ((a > b ? a - b : b - a) * 10 + c['ctrate'] / 2) / c['ctrate']
        Z.moving('ctm', k, kk, t)
        if t > 0  host.dp_set(Z.EP, 'transition', real(t) / 10) end
        host.dp_set(Z.EP, 'color_temp', b)
    end
"""

CT_ON = """            if c['ctsob'] == 1
                Z.set_ct(c, c['ctsosv'])
            elif c['ctsob'] == 2 && host.state_get('ct_abs') != nil
                Z.set_ct(c, host.state_get('ct_abs'))
            end
"""

CT_WRITE = """    elif ko == Z.KO_CT && raw.size() == 2
        var k = (raw[0] << 8) | raw[1]
        if k > 0
            host.state_set('ct_abs', k)
            Z.set_ct(c, k)
        end
    elif ko == Z.KO_CT_REL && raw.size() == 1
        Z.ct_step(c, raw[0])
"""

CT_EVENT = """    elif key == 'color_temp'
        if value != nil && value > 0
            host.ko_status(Z.KO_CT_STATUS, Z.k_bytes(Z.mired_to_k(value)), initial)
        end
"""

CT_CFG = """    c['ctmin'] = Z.clamp(Z.k_word(p, Z.P_CTMIN), @KMIN@, @KMAX@)
    c['ctmax'] = Z.clamp(Z.k_word(p, Z.P_CTMAX), @KMIN@, @KMAX@)
    if c['ctmin'] > c['ctmax']  c['ctmin'] = c['ctmax'] end
    c['ctsob'] = Z.clamp(p[Z.P_CTSOB], 0, 2)
    c['ctsosv'] = Z.clamp(Z.k_word(p, Z.P_CTSOSV), @KMIN@, @KMAX@)
    c['ctrate'] = Z.clamp(p[Z.P_CTRATE], 1, 255)
    c['ctfade'] = p[Z.P_CTFADE] != 0
"""

CT_PARAMS = """    if Z.has(Z.F_CT_START)
        if p[Z.P_CTSTART] == 0
            host.dp_set(Z.EP, 'color_temp_startup', Z.CT_PREVIOUS)
        else
            var r = Z.ct_range()
            var k = Z.clamp(Z.k_word(p, Z.P_CTSTARTVAL), Z.mired_to_k(r[1]), Z.mired_to_k(r[0]))
            host.dp_set(Z.EP, 'color_temp_startup', Z.clamp((1000000 + k / 2) / k, r[0], r[1]))
        end
    end
"""


def berry_value(v) -> str:
    if v is True:
        return "true"
    if v is False:
        return "false"
    if isinstance(v, str):                       # control characters (e.g. a 0x1A in a zigbeeModel) as \xHH
        out = v.replace("\\", "\\\\").replace("'", "\\'")
        return "'" + "".join(f"\\x{ord(ch):02x}" if ord(ch) < 0x20 or ord(ch) == 0x7F else ch for ch in out) + "'"
    if isinstance(v, list):
        return "[" + ", ".join(berry_value(x) for x in v) + "]"
    if isinstance(v, dict):
        return "{" + ", ".join(f"{berry_value(k)}: {berry_value(x)}" for k, x in v.items()) + "}"
    return repr(v)


def products_of(members: list) -> list:
    """One product per model and per white-label name (its order number is the model, so each appears once)."""
    out, seen = [], set()
    for r, entry in sorted(members, key=lambda m: (m[0]["vendor"], m[0]["model"])):
        base = {"vendor": r["vendor"], "description": r["description"], "zigbeeModel": r.get("zigbeeModel", []),
                "entry": entry}
        names = [(r["vendor"], r["model"], None)] + [(w.get("vendor") or r["vendor"], w.get("model"), r["model"])
                                                     for w in r.get("whiteLabel", []) if w.get("model")]
        for vendor, model, of in names:
            if model in seen:
                continue
            seen.add(model)
            p = dict(base, vendor=vendor, model=model)
            if of:
                p["white_label_of"] = of
            out.append(p)
    return out


def build_switch(members: list) -> tuple[dict, str]:
    """The switch class (one switched output): members [(record, [flags, child lock write])] -> (manifest, berry)."""
    spec = CLASSES["switch"]
    ep = 1
    ptypes = {
        "PowerOn": {"bits": 8, "enum": [[i, v] for i, v in enumerate(POWER_ON)]},
        "YesNo": {"bits": 8, "enum": [[0, "no"], [1, "yes"]]},
    }
    plist = [
        {"offset": 0, "name": "PowerOnBehaviour", "text": "Behaviour after mains power returns", "type": "PowerOn",
         "default": 1, "zigbee": f"{ep}.power_on_behavior (Zigbee attribute; models with flag 1; written once the "
                                 "device has told its model)"},
        {"offset": 1, "name": "ChildLock", "text": "Child lock (for devices that have one)", "type": "YesNo",
         "default": 0, "zigbee": f"{ep}.zcl.write of the model's child lock attribute (models with flag 16; written "
                                 "once the device has told its model)"},
    ]
    poff = {"pob": 0, "lock": 1}
    seg = 2
    objects = [
        {"number": 1, "name": "Switch", "text": "Switch", "function": "On/Off", "size": "1 Bit", "flags": "CW",
         "dpt": "1.001", "zigbee": "Switch On Off -> state"},
        {"number": 2, "name": "SwitchStatus", "text": "Switch status", "function": "On/Off", "size": "1 Bit",
         "flags": "CRT", "dpt": "1.001", "zigbee": "Info On Off <- state"},
        {"number": 3, "name": "Identify", "text": "Identify", "function": "Trigger", "size": "1 Bit", "flags": "CW",
         "dpt": "1.017", "zigbee": "identify on 0 and 1 alike (DPT 1.017 trigger), models with flag 2"},
        avail_object(spec),
    ]
    table = {}
    for r, entry in sorted(members, key=lambda m: m[0]["model"]):
        for zm in r.get("zigbeeModel", []):
            table[zm] = entry
    default_entry = [S_POB | S_IDENTIFY, ""]
    be = (BERRY_SWITCH.replace("@GEN@", GEN).replace("@CLASS@", spec["dir"]).replace("@NMODELS@", str(len(members)))
          .replace("@Z2M@", Z2M_VERSION).replace("@MODELS@", berry_value(table))
          .replace("@DEFAULT_ENTRY@", berry_value(default_entry))
          .replace("@POFF@", "".join(f"    static P_{k.upper()} = {v}\n" for k, v in poff.items()))
          .replace("@SEG@", str(seg)).replace("@KO_AVAIL@", str(spec["available"])))
    left = sorted({w.strip("()[],") for w in be.split() if w.strip("()[],").startswith("@") and w.strip("()[],").endswith("@")})
    if left:
        raise GenError(f"switch: placeholders left in the Berry: {left}")
    manifest = {
        "schema": "knxgw-template/0",
        "id": "zigbee.class.switch",
        "version": spec["version"],
        "title": spec["name"] + " as a KNX switching actuator",
        "radio": "zigbee",
        "match": {"class": "switch", "zigbeeModel": sorted(table)},
        "behaviour": {"language": "berry", "file": f"{spec['dir']}.be"},
        "datapoints": {
            "_doc": "<endpoint>.<Zigbee2MQTT property path>; endpoint 0 = device identity and availability (gateway: "
                    "true/false). zcl.write: 'cluster,attribute,type,manufacturer,value' in hex, written as it is "
                    "(gateway zlink.c).",
            "write": [f"{ep}.state", f"{ep}.power_on_behavior", f"{ep}.identify", f"{ep}.zcl.write"],
            "events": ["0.zigbeeModel", "0.availability", f"{ep}.state"], "master": []},
        "products": products_of(members),
        "generated": {"by": GEN, "from": f"zigbee-herdsman-converters {Z2M_VERSION}", "models": len(members)},
        "sources": [
            f"zigbee-herdsman-converters {Z2M_VERSION} (MIT, (c) Koen Kanters), distilled by distill/dump_z2m.cjs (facts only)",
            "KNX Standard v3.0.0, 07_20_02 Lighting Actuators: FB Light Switching Actuator Basic (objects Switch On Off, "
            "Info On Off)",
            "zigbee-herdsman-converters lib/ikea.js: manuSpecificIkeaSmartPlug 0xFC85, childLock 0x0000, IKEA_OF_SWEDEN 0x117C",
        ],
        "open": [
            "LED setting of the IKEA plugs left out: Zigbee2MQTT reads the same attribute as 'led enable' from firmware "
            "2.4.25 and as 'led disable' before; not checked on a device",
            "Plugs with metering (IKEA INSPELNING) are not in this class",
        ],
        "knx": knx_part(spec, objects, ptypes, seg, plist),
    }
    return manifest, be


def build(cls: str, members: list) -> tuple[dict, str]:
    """members: [(record, entry)] of one class -> (manifest, berry)."""
    if cls == "switch":
        return build_switch(members)
    if cls == "remote":
        return build_remote(members)
    spec = CLASSES[cls]
    ct = cls == "white_spectrum"
    ep = 1
    ptypes = {
        "PowerOn": {"bits": 8, "enum": [[i, v] for i, v in enumerate(POWER_ON)]},
        "StartLevel": {"bits": 8, "enum": [[i, v] for i, v in enumerate(LEVEL_START + ["fixed value"])]},
        "Percent": {"bits": 8, "number": {"type": "unsignedInt", "min": 1, "max": 100}},
        "YesNo": {"bits": 8, "enum": [[0, "no"], [1, "yes"]]},
        "Tenths": {"bits": 8, "number": {"type": "unsignedInt", "min": 0, "max": 255}},
        "Rate": {"bits": 8, "number": {"type": "unsignedInt", "min": 1, "max": 255}},
    }
    plist, poff, at = [], {}, [0]

    def add(key, pname, text, ptype, default, doc):
        poff[key] = at[0]
        plist.append({"offset": at[0], "name": pname, "text": text, "type": ptype, "default": default, "zigbee": doc})
        at[0] += (ptypes[ptype]["bits"] + 7) // 8

    add("pob", "PowerOnBehaviour", "Behaviour after mains power returns", "PowerOn", 1,
        f"{ep}.power_on_behavior (Zigbee attribute)")
    add("start", "StartupLevel", "Brightness after mains power returns", "StartLevel", 1,
        f"{ep}.level_config.current_level_startup: preset or value (models with flag 1)")
    add("startval", "StartupLevelValue", "Fixed brightness after mains power returns (%)", "Percent", 100,
        f"{ep}.level_config.current_level_startup as brightness")
    add("osv", "SwitchOnValue", "Switch-on brightness (%)", "Percent", 100, "gateway: FB DAB OSV")
    add("mf", "MemoryFunction", "Switch on with the last brightness", "YesNo", 1, "gateway: FB DAB MF")
    add("min", "MinimumValue", "Minimum brightness (%)", "Percent", 1, "gateway: FB DAB MIN_SV")
    add("max", "MaximumValue", "Maximum brightness (%)", "Percent", 100, "gateway: FB DAB MAX_SV")
    add("roe", "RelativeOffEnable", "Relative dimming may switch off", "YesNo", 0,
        "gateway: FB DAB ROE (brightness_move_onoff instead of brightness_move)")
    add("trans", "TransitionTime", "Transition for switching and values (0.1 s)", "Tenths", 0,
        "Zigbee2MQTT option transition (seconds), sent before each command")
    add("rate", "DimmingRate", "Relative dimming rate (brightness steps per second)", "Rate", 60,
        f"{ep}.brightness_move rate")
    objects = [
        {"number": 1, "name": "Switch", "text": "Switch", "function": "On/Off", "size": "1 Bit", "flags": "CW",
         "dpt": "1.001", "zigbee": "FB DAB Switch On Off -> state / brightness"},
        {"number": 2, "name": "Dimming", "text": "Relative dimming", "function": "Brighter/Darker", "size": "4 Bit",
         "flags": "CW", "dpt": "3.007",
         "zigbee": "FB DAB Relative Setvalue Control: step code 1..7 -> brightness by 1/2^(code-1) of 0..100 % at the "
                   "dimming rate, 0 -> brightness_move stop; ROE below MIN_SV -> brightness_move_onoff"},
        {"number": 3, "name": "Value", "text": "Brightness value", "function": "Value", "size": "1 Byte",
         "flags": "CW", "dpt": "5.001", "zigbee": "FB DAB Absolute Setvalue Control -> brightness, 0 = off"},
        {"number": 4, "name": "SwitchStatus", "text": "Switch status", "function": "On/Off", "size": "1 Bit",
         "flags": "CRT", "dpt": "1.001", "zigbee": "FB DAB Info On Off <- state"},
        {"number": 5, "name": "ValueStatus", "text": "Brightness status", "function": "Value", "size": "1 Byte",
         "flags": "CRT", "dpt": "5.001", "zigbee": "FB DAB Actual Dimming Value <- brightness, 0 while off"},
        {"number": 6, "name": "Identify", "text": "Identify", "function": "Trigger", "size": "1 Bit", "flags": "CW",
         "dpt": "1.017",
         "zigbee": "identify on 0 and 1 alike (DPT 1.017 trigger), models with flag 2 (manufacturer-specific object, not in "
                   "FB DAB)"},
    ]
    write = [f"{ep}.state", f"{ep}.brightness", f"{ep}.brightness_move", f"{ep}.brightness_move_onoff",
             f"{ep}.transition", f"{ep}.power_on_behavior", f"{ep}.level_config.current_level_startup",
             f"{ep}.identify"]
    events = ["0.zigbeeModel", f"{ep}.state", f"{ep}.brightness"]
    defaults = {"osv": 100, "mf": True, "min": 1, "max": 100, "roe": False, "trans": 0, "rate": 60}
    parts = {k: "" for k in ("CT_DOC", "CT_CONST", "CT_HELPERS", "CT_ON", "CT_WRITE", "CT_EVENT", "CT_CFG",
                             "CT_PARAMS")}
    if ct:
        ptypes["Kelvin"] = {"bits": 16, "number": {"type": "unsignedInt", "min": K_RANGE[0], "max": K_RANGE[1]}}
        ptypes["CtSwitchOn"] = {"bits": 8, "enum": [[0, "last actual value"], [1, "value of the next parameter"],
                                                    [2, "last received absolute value"]]}
        ptypes["CtStartup"] = {"bits": 8, "enum": [[0, "previous"], [1, "fixed value"]]}
        add("ctmin", "CtMinimum", "Minimum colour temperature (K)", "Kelvin", K_RANGE[0],
            "gateway: FB CTA MINCT; the lamp's own range applies on top")
        add("ctmax", "CtMaximum", "Maximum colour temperature (K)", "Kelvin", K_RANGE[1],
            "gateway: FB CTA MAXCT; the lamp's own range applies on top")
        add("ctsob", "CtSwitchOnMode", "Colour temperature when switched on", "CtSwitchOn", 0,
            "gateway: FB CTA CTSOB, DPT 20.608 (0 last actual, 1 parameter, 2 last received absolute)")
        add("ctsosv", "CtSwitchOnValue", "Colour temperature when switched on (K)", "Kelvin", 2700,
            "gateway: FB CTA CTSOSV")
        add("ctrate", "CtRate", "Relative colour temperature rate (mired per second)", "Rate", 55,
            f"{ep}.color_temp_move rate")
        add("ctfade", "CtFade", "Use the transition time for colour temperature", "YesNo", 0,
            "gateway: FB CTA CTFMS (no = jump); never for models with flag 8")
        add("ctstart", "CtStartup", "Colour temperature after mains power returns", "CtStartup", 0,
            f"{ep}.color_temp_startup: previous (65535) or value (models with flag 4)")
        add("ctstartval", "CtStartupValue", "Fixed colour temperature after mains power returns (K)", "Kelvin", 2700,
            f"{ep}.color_temp_startup as mired")
        objects += [
            {"number": 7, "name": "ColourTemperature", "text": "Colour temperature", "function": "Kelvin",
             "size": "2 Bytes", "flags": "CW", "dpt": "7.600",
             "zigbee": "FB CTA Absolute Colour Temperature Control -> color_temp (mired = 1000000 / K)"},
            {"number": 8, "name": "ColourTemperatureRelative", "text": "Relative colour temperature",
             "function": "Cooler/Warmer", "size": "4 Bit", "flags": "CW", "dpt": "3.007",
             "zigbee": "FB CTA Relative Setvalue Control Colour Temperature: step code 1 -> color_temp_move (increase = "
                       "negative), 2..7 -> color_temp by 1/2^(code-1) of MINCT..MAXCT, 0 -> stop"},
            {"number": 9, "name": "ColourTemperatureStatus", "text": "Colour temperature status", "function": "Kelvin",
             "size": "2 Bytes", "flags": "CRT", "dpt": "7.600", "zigbee": "FB CTA Current Colour Temperature <- color_temp"},
            {"number": 10, "name": "ColourTemperatureOutOfRange", "text": "Colour temperature out of range",
             "function": "Alarm", "size": "1 Bit", "flags": "CRT", "dpt": "1.002",
             "zigbee": "FB CTA Current Colour Temperature Out of Range: a request outside the usable range was clamped"},
        ]
        write += [f"{ep}.color_temp", f"{ep}.color_temp_move", f"{ep}.color_temp_startup"]
        events += [f"{ep}.color_temp", f"{ep}.colorTempPhysicalMin", f"{ep}.colorTempPhysicalMax"]
        defaults.update({"ctmin": K_RANGE[0], "ctmax": K_RANGE[1], "ctsob": 0, "ctsosv": 2700, "ctrate": 55,
                         "ctfade": False})
        parts.update({"CT_DOC": CT_DOC, "CT_CONST": CT_CONST, "CT_HELPERS": CT_HELPERS, "CT_ON": CT_ON,
                      "CT_WRITE": CT_WRITE, "CT_EVENT": CT_EVENT, "CT_CFG": CT_CFG, "CT_PARAMS": CT_PARAMS})
    objects.append(avail_object(spec))
    events.insert(1, "0.availability")
    seg = at[0]
    table = {}
    for r, entry in sorted(members, key=lambda m: m[0]["model"]):
        for zm in r.get("zigbeeModel", []):
            table[zm] = entry
    default_entry = [1, 0, 0, F_LEVEL_START | F_IDENTIFY | (F_CT_START if ct else 0)]
    be = BERRY
    for k, v in parts.items():
        be = be.replace(f"@{k}@", v)
    be = (be.replace("@GEN@", GEN).replace("@CLASS@", spec["dir"]).replace("@NMODELS@", str(len(members)))
          .replace("@Z2M@", Z2M_VERSION).replace("@MODELS@", berry_value(table))
          .replace("@DEFAULT_ENTRY@", berry_value(default_entry)).replace("@DEFAULTS@", berry_value(defaults))
          .replace("@POFF@", "".join(f"    static P_{k.upper()} = {v}\n" for k, v in poff.items()))
          .replace("@SEG@", str(seg)).replace("@MD_MIN@", str(MIRED_DEFAULT[0])).replace("@MD_MAX@", str(MIRED_DEFAULT[1]))
          .replace("@KMIN@", str(K_RANGE[0])).replace("@KMAX@", str(K_RANGE[1]))
          .replace("@KO_AVAIL@", str(spec["available"])))
    left = sorted({w.strip("()[],") for w in be.split() if w.strip("()[],").startswith("@") and w.strip("()[],").endswith("@")})
    if left:
        raise GenError(f"{cls}: placeholders left in the Berry: {left}")
    products = products_of(members)
    manifest = {
        "schema": "knxgw-template/0",
        "id": f"zigbee.class.{cls}",
        "version": spec["version"],
        "title": spec["name"] + " as a KNX " + ("dimming actuator with colour temperature" if ct else "dimming actuator"),
        "radio": "zigbee",
        "match": {"class": cls, "zigbeeModel": sorted(table)},
        "behaviour": {"language": "berry", "file": f"{spec['dir']}.be"},
        "datapoints": {
            "_doc": "<endpoint>.<Zigbee2MQTT property path>; endpoint 0 = device identity and availability (gateway: "
                    "true/false). 'transition' applies to the next state/brightness command only. "
                    "colorTempPhysicalMin/Max: ZCL 0x400B/0x400C as the lamp reports them (the radio adapter reads them).",
            "write": write, "events": events, "master": []},
        "products": products,
        "generated": {"by": GEN, "from": f"zigbee-herdsman-converters {Z2M_VERSION}", "models": len(members)},
        "sources": [
            f"zigbee-herdsman-converters {Z2M_VERSION} (MIT, (c) Koen Kanters), distilled by distill/dump_z2m.cjs (facts only)",
            "KNX Standard v3.0.0, 07_20_02 Lighting Actuators: FB Dimming Actuator Basic p. 37, 55, state tables p. 44-46" +
            ("; FB Colour Temperature Actuator p. 94-95, 98-99; 03_07_02 p. 177 (DPT 20.608)" if ct else "") +
            "; 03_07_02 p. 29 (DPT 3.007)",
            "zigbee-herdsman cluster lightingColorCtrl: colorTempPhysicalMin 0x400B, colorTempPhysicalMax 0x400C",
        ],
        "open": ([
            "MINCT/MAXCT do not limit relative colour temperature with step code 1: color_temp_move runs to the lamp's "
            "own limits or a stop",
            "A stop of relative colour temperature (Move Color Temperature, stop) ends a Move to Color Temperature still "
            "running on IKEA white spectrum lamps (Zigbee2MQTT lib/ikea.ts relies on it); other lamps not checked",
        ] if ct else []) + [
            "Parameters live in host.state; after a VM restart the defaults apply until the next params call",
            "The radio adapter must deliver 0.zigbeeModel (and colorTempPhysicalMin/Max) as datapoints",
        ],
        "knx": knx_part(spec, objects, ptypes, seg, plist),
    }
    return manifest, be


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump")
    ap.add_argument("--vendor", help="only this vendor's definitions (e.g. IKEA)")
    ap.add_argument("--out-dir", default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "templates"))
    ap.add_argument("--report", action="store_true", help="only print the classification")
    a = ap.parse_args()
    groups, refused = collections.defaultdict(list), collections.Counter()
    with open(a.dump, encoding="utf-8") as f:
        for line in f:
            r = json.loads(line)
            if a.vendor and r.get("vendor") != a.vendor:
                continue
            types = {e.get("type") for e in r.get("exposes", [])}
            names = {e.get("name") for e in r.get("exposes", [])}
            if "light" in types:
                how = classify
            elif "switch" in types:
                how = classify_switch
            elif "action" in names:
                how = classify_remote
            else:
                continue
            try:
                cls, entry = how(r)
                groups[cls].append((r, entry))
            except GenError as e:
                refused[str(e)] += 1
    for cls, members in sorted(groups.items()):
        print(f"{cls}: {len(members)} models")
    for why, n in refused.most_common():
        print(f"  refused {n:4d}: {why}")
    if a.report:
        return
    for cls, members in sorted(groups.items()):
        m, be = build(cls, members)
        short = shorten_berry(be)
        if shorten_berry(short) != short:
            raise GenError(f"{cls}: shortening the Berry twice changes it")
        d = os.path.join(a.out_dir, CLASSES[cls]["dir"])
        os.makedirs(d, exist_ok=True)
        atomic_write(os.path.join(d, "manifest.json"), (json.dumps(m, indent=2, ensure_ascii=False) + "\n").encode())
        atomic_write(os.path.join(d, m["behaviour"]["file"]), short.encode())
        app = m["knx"]["application"]
        print(f"{d}: app {CLASSES[cls]['app']:#06x} version {app['version']} (replaces {app['replaces']}), "
              f"{len(m['knx']['objects'])} objects, {m['knx']['parameters']['segment_size']} parameter bytes, "
              f"{len(m['match']['zigbeeModel'])} zigbeeModel keys, {len(m['products'])} products, Berry {len(be)} -> "
              f"{len(short)} characters")


if __name__ == "__main__":
    try:
        main()
    except GenError as e:
        sys.exit(f"z2m_to_knx: {e}")
