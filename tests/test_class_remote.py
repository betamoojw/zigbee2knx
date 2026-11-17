#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dirk Tostmann
"""Class zigbee-remote (a KNX push button) in the firmware's Berry VM, as the IKEA E1743 TRADFRI on/off switch: its
button commands as the gateway hands them over ("zcl.cmd"). No device, no network."""
import sys

from vmtest import Checks, Template, Vm, availability_checks, effects, rebuild_checks, status

T = Template("zigbee-remote")
E1743 = "TRADFRI on/off switch"


def sends(ops):
    return [(o["ko"], o["raw"].lower()) for o in ops if o["op"] == "ko_send"]


def main():
    check = Checks()
    m = T.manifest
    rebuild_checks(check, T, app_version=2, replaces=[1], objects=6)
    check("E1743 is in the class table", E1743 in m["match"]["zigbeeModel"], m["match"]["zigbeeModel"])
    check("a product E1743", "E1743" in [p["model"] for p in m["products"]], None)
    vm = Vm(T)
    vm.call("dp_event", ch=0, key="zigbeeModel", value=E1743, initial=True)
    vm.call("params", p=T.segment())
    cmd = lambda v: vm.call("dp_event", ch=1, key="zcl.cmd", value=v, initial=False)
    check("I: On -> KO1 1", sends(cmd("0006,01,")) == [(1, "01")], None)
    check("O: Off -> KO1 0", sends(cmd("0006,00,")) == [(1, "00")], None)
    check("I held: Move with On/Off up -> KO2 brighter (09)", sends(cmd("0008,05,0053")) == [(2, "09")], None)
    check("released: Stop with On/Off -> KO2 stop (00)", sends(cmd("0008,07,")) == [(2, "00")], None)
    check("O held: Move down -> KO2 darker (01)", sends(cmd("0008,01,0153")) == [(2, "01")], None)
    check("step up -> KO2 12.5 % brighter (0c)", sends(cmd("0008,02,002b0500")) == [(2, "0c")], None)
    check("move to level 127 -> KO3 128", sends(cmd("0008,04,7f0000")) == [(3, "80")], None)
    vm.call("knx_write", ko=5, raw="01")
    check("toggle with status on -> KO1 0", sends(cmd("0006,02,")) == [(1, "00")], None)
    check("toggle again -> KO1 1", sends(cmd("0006,02,")) == [(1, "01")], None)
    check("off with effect -> KO1 0", sends(cmd("0006,40,0000")) == [(1, "00")], None)
    check("a scene command is left alone", sends(cmd("0005,05,00000000")) == [], None)
    ops = vm.call("dp_event", ch=1, key="battery", value=87, initial=False)
    check("battery 87 % -> KO4 0xde", status(ops) == [(4, "de", False)], status(ops))
    vm.call("params", p=T.segment(InvertSwitch=1))
    check("inverted: On -> KO1 0", sends(cmd("0006,01,")) == [(1, "00")], None)
    availability_checks(check, T, vm, inst=1)
    ops = cmd("0006,01,")
    check("after availability: On -> KO1 only", sends(ops) == [(1, "00")] and len(effects(ops)) == 1, effects(ops))
    vm.close()
    return check.finish()


if __name__ == "__main__":
    sys.exit(main())
