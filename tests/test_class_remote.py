#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dirk Tostmann
"""Class zigbee-remote (a KNX push button) in the firmware's Berry VM, as the IKEA E1743 TRADFRI on/off switch: its
button commands as the gateway hands them over ("zcl.cmd"). No device, no network.

Steps: a Zigbee Step command becomes the DPT 3.007 step code nearest to its step size (of 254, on a logarithmic
scale). Battery: BatteryPercentageRemaining as the remote reports it, in half percent by ZCL; percent for the models
Zigbee2MQTT reads as percent (ICTC-G-1 always, the models with ikeaBattery() below firmware 2.4.0, compared as a
loose semantic version the way Zigbee2MQTT does)."""
import sys

from vmtest import Checks, Template, Vm, availability_checks, effects, rebuild_checks, status

T = Template("zigbee-remote")
E1743 = "TRADFRI on/off switch"
ICTC = "TRADFRI wireless dimmer"                   # ICTC-G-1: reports percent
RODRET = "RODRET Dimmer"                           # E2201: reports half percent


def sends(ops):
    return [(o["ko"], o["raw"].lower()) for o in ops if o["op"] == "ko_send"]


def main():
    check = Checks()
    m = T.manifest
    rebuild_checks(check, T, app_version=2, replaces=[1], available=6, section=("Remotes", "Remotes"))
    check("E1743 is in the class table", E1743 in m["match"]["zigbeeModel"], m["match"]["zigbeeModel"])
    check("a product E1743", "E1743" in [p["model"] for p in m["products"]], None)
    avail = [o for o in m["knx"]["objects"] if o["name"] == "Available"]
    check("'Available' says what it means for a battery device", avail and avail[0]["text"] ==
          "Zigbee device heard in the last 25 h", avail)
    entries = {p["model"]: p["entry"] for p in m["products"]}
    check("battery facts per model: E1743 E1744 E1812 E1841 percent below 2.4.0, ICTC-G-1 percent, E2201 half percent",
          all(entries.get(x) == [5, [2, 4, 0]] for x in ("E1743", "E1744", "E1812", "E1841")) and
          entries.get("ICTC-G-1") == [3, []] and entries.get("E2201") == [1, []], entries)
    ev = m["datapoints"]["events"]
    check("datapoints: swBuildId on endpoint 0, batteryPercentageRemaining", "0.swBuildId" in ev and
          "1.batteryPercentageRemaining" in ev, ev)
    vm = Vm(T)
    vm.call("dp_event", ch=0, key="zigbeeModel", value=E1743, initial=True)
    vm.call("params", p=T.segment())
    cmd = lambda v, inst=1: vm.call("dp_event", inst=inst, ch=1, key="zcl.cmd", value=v, initial=False)   # noqa: E731
    check("I: On -> KO1 1", sends(cmd("0006,01,")) == [(1, "01")], None)
    check("O: Off -> KO1 0", sends(cmd("0006,00,")) == [(1, "00")], None)
    check("I held: Move with On/Off up -> KO2 brighter (09)", sends(cmd("0008,05,0053")) == [(2, "09")], None)
    check("released: Stop with On/Off -> KO2 stop (00)", sends(cmd("0008,07,")) == [(2, "00")], None)
    check("O held: Move down -> KO2 darker (01)", sends(cmd("0008,01,0153")) == [(2, "01")], None)
    check("step up 43 of 254 -> KO2 12.5 % brighter (0c)", sends(cmd("0008,02,002b0500")) == [(2, "0c")], None)
    check("step down 255 with On/Off -> KO2 darker by the whole range (01)", sends(cmd("0008,06,01ff0a00")) == [(2, "01")], None)
    check("step up 127 -> KO2 50 % (0a)", sends(cmd("0008,02,007f0500")) == [(2, "0a")], None)
    check("step up 64 -> KO2 25 % (0b)", sends(cmd("0008,02,00400a00")) == [(2, "0b")], None)
    check("step down 16 -> KO2 6.25 % (05)", sends(cmd("0008,02,01100a00")) == [(2, "05")], None)
    check("step up 1 -> KO2 the smallest step (0f)", sends(cmd("0008,02,00010a00")) == [(2, "0f")], None)
    check("step of size 0 -> nothing", sends(cmd("0008,02,00000a00")) == [], None)
    check("step without a size -> nothing", sends(cmd("0008,02,00")) == [], None)
    check("move to level 127 -> KO3 128", sends(cmd("0008,04,7f0000")) == [(3, "80")], None)
    vm.call("knx_write", ko=5, raw="01")
    check("toggle with status on -> KO1 0", sends(cmd("0006,02,")) == [(1, "00")], None)
    check("toggle again -> KO1 1", sends(cmd("0006,02,")) == [(1, "01")], None)
    check("off with effect -> KO1 0", sends(cmd("0006,40,0000")) == [(1, "00")], None)
    check("a scene command is left alone", sends(cmd("0005,05,00000000")) == [], None)

    def battery(raw, inst=1):
        return status(vm.call("dp_event", inst=inst, ch=1, key="batteryPercentageRemaining", value=raw, initial=False))

    def build(value, inst=1):
        return status(vm.call("dp_event", inst=inst, ch=0, key="swBuildId", value=value, initial=True))

    check("E1743, firmware not known yet: 87 half percent -> 43.5 % (0x6f)", battery(87) == [(4, "6f", False)], None)
    check("its firmware 2.3.075 arrives: the same report is percent, 87 % (0xde), sent", build("2.3.075") ==
          [(4, "de", False)], None)
    check("E1743 2.3.075: 50 -> 50 % (0x80)", battery(50) == [(4, "80", False)], None)
    check("firmware 2.4.5: half percent again, 50 -> 25 % (0x40)", build("2.4.5") == [(4, "40", False)], None)
    check("100 half percent at 2.4.5 -> 50 % (0x80)", battery(100) == [(4, "80", False)], None)
    for v, pct in (("v2.3.075", True), (" 2.3.075 ", True), ("=v2.3.9", True), ("2.4.0-beta", True), ("2.3.075a", True),
                   ("2.4.0", False), ("2.4.0+5", False), ("24.4.13", False), ("1.1.1.0-5.7.2.0", False), ("2.3", False),
                   ("", False), ("2.3.x", False)):
        got = build(v)
        check(f"firmware {v!r}: {'below 2.4.0, percent' if pct else 'not below 2.4.0 or no version, half percent'}",
              got == [(4, "ff" if pct else "80", False)], got)
    check("200 half percent -> 100 % (0xff)", battery(200) == [(4, "ff", False)], None)
    check("0xFF (not known) is no value", battery(255) == [], None)
    vm.call("dp_event", inst=2, ch=0, key="zigbeeModel", value=ICTC, initial=True)
    check("ICTC-G-1 reports percent: 87 -> 0xde", battery(87, inst=2) == [(4, "de", False)], None)
    vm.call("dp_event", inst=3, ch=0, key="zigbeeModel", value=RODRET, initial=True)
    check("E2201 reports half percent: 174 -> 0xde", battery(174, inst=3) == [(4, "de", False)], None)
    check("E2201 with firmware 1.0.0 stays half percent", build("1.0.0", inst=3) == [(4, "de", False)], None)
    vm.call("dp_event", inst=4, ch=0, key="zigbeeModel", value="UNKNOWN-REMOTE", initial=True)
    check("a model the table does not know: half percent, 174 -> 0xde", battery(174, inst=4) == [(4, "de", False)], None)
    ops = vm.call("dp_event", inst=5, ch=1, key="battery", value=87, initial=False)
    check("battery in percent (gateways that convert it): 87 % -> 0xde", status(ops) == [(4, "de", False)], status(ops))
    vm.call("params", p=T.segment(InvertSwitch=1))
    check("inverted: On -> KO1 0", sends(cmd("0006,01,")) == [(1, "00")], None)
    availability_checks(check, T, vm, inst=1)
    ops = cmd("0006,01,")
    check("after availability: On -> KO1 only", sends(ops) == [(1, "00")] and len(effects(ops)) == 1, effects(ops))
    vm.close()
    return check.finish()


if __name__ == "__main__":
    sys.exit(main())
