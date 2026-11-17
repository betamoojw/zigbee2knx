#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dirk Tostmann
"""Class zigbee-white-spectrum-light (FB Colour Temperature Actuator on top of FB Dimming Actuator Basic) in the
firmware's Berry VM: one application, models told apart at run time (LED1736G9 with the freezing bug,
KAJPLATS without), the lamp's own range winning over the table, the class default for an unknown model."""
import sys

from vmtest import Checks, Template, Vm, availability_checks, effects, rebuild_checks, sets, status

T = Template("zigbee-white-spectrum-light")
LED1736G9 = "TRADFRI bulb E27 WS clear 806lm"      # table: [1, 250, 454, 15] (flag 8: freezing bug)
KAJPLATS = "KAJPLATS E27 WS globe 470lm"           # table: [2, 153, 454, 7]


def main():
    check = Checks()
    rebuild_checks(check, T, app_version=3, replaces=[2], objects=11)
    prods = {p["model"]: p for p in T.manifest["products"]}
    check("products per white-label name: LED2406G4 and LED2401G5 (KAJPLATS)",
          prods.get("LED2406G4", {}).get("white_label_of") == "KAJPLATS_WS" and
          prods.get("LED2401G5", {}).get("white_label_of") == "KAJPLATS_WS_clear", sorted(prods))
    vm = Vm(T)
    vm.call("dp_event", ch=0, key="zigbeeModel", value=LED1736G9, initial=True)
    ops = vm.call("params", p=T.segment())
    check("params: power-on on, level previous, colour temperature startup previous (65535)",
          sets(ops) == [("power_on_behavior", "on"), ("level_config.current_level_startup", "previous"),
                        ("color_temp_startup", 65535)], sets(ops))
    ops = vm.call("knx_write", ko=7, raw="0a8c")                     # 2700 K
    check("KO7 2700 K -> 370 mired, in range", sets(ops) == [("color_temp", 370)] and status(ops) == [(10, "00", False)],
          (sets(ops), status(ops)))
    ops = vm.call("knx_write", ko=7, raw="1964")                     # 6500 K, table range ends at 4000 K
    check("KO7 6500 K -> 4000 K = 250 mired, out of range", sets(ops) == [("color_temp", 250)] and
          status(ops) == [(10, "01", False)], (sets(ops), status(ops)))
    check("KO8 increase (more K) -> color_temp_move -55", sets(vm.call("knx_write", ko=8, raw="0b")) == [("color_temp_move", -55)], None)
    check("KO8 decrease -> +55", sets(vm.call("knx_write", ko=8, raw="03")) == [("color_temp_move", 55)], None)
    check("KO8 stop", sets(vm.call("knx_write", ko=8, raw="08")) == [("color_temp_move", "stop")], None)
    ops = vm.call("dp_event", ch=1, key="color_temp", value=370, initial=False)
    check("color_temp 370 -> KO9 2703 K", status(ops) == [(9, "0a8f", False)], status(ops))
    vm.call("params", p=T.segment(CtFade=1, TransitionTime=15, CtSwitchOnMode=1, CtSwitchOnValue=3000))
    ops = vm.call("knx_write", ko=1, raw="01")
    check("LED1736G9 (freezing bug): colour temperature jumps, brightness fades",
          sets(ops) == [("color_temp", 333), ("transition", 1.5), ("brightness", 254)], sets(ops))
    # same instance, now the identity of a KAJPLATS (no freezing bug)
    vm.call("dp_event", ch=0, key="zigbeeModel", value=KAJPLATS, initial=False)
    ops = vm.call("knx_write", ko=1, raw="01")
    check("KAJPLATS: colour temperature fades too",
          sets(ops) == [("transition", 1.5), ("color_temp", 333), ("transition", 1.5), ("brightness", 254)], sets(ops))
    # the lamp reports its physical range (ZCL 0x400B/0x400C): 153..370 mired = 2703..6536 K
    vm.call("dp_event", ch=1, key="colorTempPhysicalMin", value=153, initial=True)
    vm.call("dp_event", ch=1, key="colorTempPhysicalMax", value=370, initial=True)
    ops = vm.call("knx_write", ko=7, raw="07d0")                     # 2000 K
    check("lamp range wins: 2000 K -> 2703 K = 370 mired, out of range",
          sets(ops) == [("transition", 1.5), ("color_temp", 370)] and status(ops) == [(10, "01", False)], (sets(ops), status(ops)))
    ops = vm.call("knx_write", ko=7, raw="1964")                     # 6500 K, inside 2703..6536
    check("lamp range wins: 6500 K -> 154 mired, in range",
          sets(ops) == [("transition", 1.5), ("color_temp", 154)] and status(ops) == [(10, "00", False)], (sets(ops), status(ops)))
    ops = vm.call("params", p=T.segment(CtStartup=1, CtStartupValue=3500))
    check("params: fixed startup 3500 K -> 286 mired", ("color_temp_startup", 286) in sets(ops), sets(ops))
    # instance 2: unknown model, no reported range -> class default 153..500 mired (2000..6536 K)
    vm.call("dp_event", inst=2, ch=0, key="zigbeeModel", value="UNKNOWN-WS", initial=True)
    ops = vm.call("knx_write", inst=2, ko=7, raw="05dc")             # 1500 K
    check("unknown model: class default range, 1500 K -> 2000 K = 500 mired",
          sets(ops) == [("color_temp", 500)] and status(ops) == [(10, "01", False)], (sets(ops), status(ops)))
    availability_checks(check, T, vm, inst=2)
    ops = vm.call("dp_event", inst=2, ch=1, key="color_temp", value=370, initial=False)
    check("after availability: color_temp 370 -> KO9 only", status(ops) == [(9, "0a8f", False)] and
          len(effects(ops)) == 1, effects(ops))
    vm.close()
    w = set(T.manifest["datapoints"]["write"])
    e = set(T.manifest["datapoints"]["events"])
    check("datapoints declared", {"1.color_temp", "1.color_temp_move", "1.color_temp_startup"} <= w and
          {"0.zigbeeModel", "1.colorTempPhysicalMin", "1.colorTempPhysicalMax", "1.color_temp"} <= e, (w, e))
    return check.finish()


if __name__ == "__main__":
    sys.exit(main())
