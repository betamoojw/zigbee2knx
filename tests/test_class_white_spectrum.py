#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dirk Tostmann
"""Class zigbee-white-spectrum-light (FB Colour Temperature Actuator on top of FB Dimming Actuator Basic) in the
firmware's Berry VM: one application, models told apart at run time (LED1736G9 with the freezing bug,
KAJPLATS without), the lamp's own range winning over the table, the class default for an unknown model.

Relative colour temperature (DPT 3.007, KNX 07_20_02 p. 94-95): step code 1 fades until a stop (color_temp_move),
2..7 move the set value by 1/2^(code-1) of MINCT..MAXCT at the colour temperature rate (mired per second), 0 stops.
A model with the freezing bug jumps instead of fading. Each step check has an instance of its own.

The LED2201G8 (model flag 16) restarts on Move to Level with On/Off while it is off: from off the class sets the
level while off (brightness_no_onoff, ExecuteIfOff in the gateway) and then sends On; every other model gets
brightness as before."""
import sys

from vmtest import Checks, Template, Vm, availability_checks, effects, rebuild_checks, sets, status

T = Template("zigbee-white-spectrum-light")
LED1736G9 = "TRADFRI bulb E27 WS clear 806lm"      # table: [1, 250, 454, 15] (flag 8: freezing bug)
KAJPLATS = "KAJPLATS E27 WS globe 470lm"           # table: [2, 153, 454, 7]
LED2201G8 = "TRADFRI bulb E27 WS globe 1055lm"     # table: [1, 250, 454, 31] (flag 16: level while off, then On)


def lamp(vm, inst, model, mired=370, brightness=100):
    """A fresh instance: the model reporting its colour temperature (unless None) and brightness, default parameters."""
    vm.call("dp_event", inst=inst, ch=0, key="zigbeeModel", value=model, initial=True)
    if mired is not None:
        vm.call("dp_event", inst=inst, ch=1, key="color_temp", value=mired, initial=True)
    vm.call("dp_event", inst=inst, ch=1, key="brightness", value=brightness, initial=True)
    vm.call("dp_event", inst=inst, ch=1, key="state", value="ON", initial=True)
    vm.call("params", inst=inst, p=T.segment())


def main():
    check = Checks()
    rebuild_checks(check, T, app_version=6, replaces=[2, 3, 4, 5], available=11, section=("Lights", "Lights"))
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
    check("KO8 increase by the whole range (more K) -> color_temp_move -55 within the lamp's 250..454 mired",
          sets(vm.call("knx_write", ko=8, raw="09")) == [("color_temp_move", "-55,250,454")], None)
    check("KO8 decrease by the whole range -> +55 within 250..454",
          sets(vm.call("knx_write", ko=8, raw="01")) == [("color_temp_move", "55,250,454")], None)
    check("KO8 stop", sets(vm.call("knx_write", ko=8, raw="08")) == [("color_temp_move", "stop")], None)
    ops = vm.call("dp_event", ch=1, key="color_temp", value=370, initial=False)
    check("color_temp 370 -> KO9 2703 K", status(ops) == [(9, "0a8f", False)], status(ops))
    check("KO6 identify on 0 as well (DPT 1.017 trigger)",
          sets(vm.call("knx_write", ko=6, raw="00")) == [("identify", "identify")], None)
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

    # relative colour temperature, KAJPLATS at 370 mired (2703 K): MINCT..MAXCT 1000..10000 K within the lamp's
    # 153..454 mired = 2203..6536 K, 55 mired/s
    lamp(vm, 5, KAJPLATS)
    got = sets(vm.call("knx_write", inst=5, ko=8, raw="0b"))
    check("KO8 up, step code 3: 2703 K + 25 % of 2203..6536 K = 3786 K (264 mired) in 1.9 s",
          got == [("transition", 1.9), ("color_temp", 264)], got)
    lamp(vm, 6, KAJPLATS)
    got = sets(vm.call("knx_write", inst=6, ko=8, raw="04"))
    check("KO8 down, step code 4: stops at the warm end, 2203 K (454 mired), in 1.5 s",
          got == [("transition", 1.5), ("color_temp", 454)], got)
    lamp(vm, 7, KAJPLATS)
    vm.call("knx_write", inst=7, ko=8, raw="0c")
    got = sets(vm.call("knx_write", inst=7, ko=8, raw="0c"))
    check("KO8 up 12.5 % twice at once: the second counts from the first target (3245 K -> 3787 K, 264 mired)",
          got == [("transition", 0.8), ("color_temp", 264)], got)
    lamp(vm, 8, KAJPLATS)
    got = sets(vm.call("knx_write", inst=8, ko=8, raw="0f"))
    check("KO8 up, step code 7: 2703 K -> 2771 K (361 mired) in 0.2 s", got == [("transition", 0.2), ("color_temp", 361)], got)
    lamp(vm, 9, LED1736G9)
    got = sets(vm.call("knx_write", inst=9, ko=8, raw="0b"))
    check("LED1736G9 (freezing bug): a step jumps, 2703 K + 25 % of 2203..4000 K = 3152 K (317 mired)",
          got == [("color_temp", 317)], got)
    lamp(vm, 10, KAJPLATS, mired=None)
    got = sets(vm.call("knx_write", inst=10, ko=8, raw="0c"))
    check("no colour temperature reported yet: a step counts from the middle of the range (4369 K -> 4911 K)",
          got == [("transition", 0.5), ("color_temp", 204)], got)
    lamp(vm, 11, LED1736G9)
    got = sets(vm.call("knx_write", inst=11, ko=2, raw="0c"))
    check("KO2 up, step code 4: brightness 100 -> 132 in 0.5 s", got == [("transition", 0.5), ("brightness", 132)], got)

    # LED2201G8: from off the level while off, then On
    lamp(vm, 12, LED2201G8)
    vm.call("dp_event", inst=12, ch=1, key="state", value="OFF", initial=False)
    got = sets(vm.call("knx_write", inst=12, ko=3, raw="80"))
    check("LED2201G8 off, KO3 50 %: brightness_no_onoff 127, then On",
          got == [("brightness_no_onoff", 127), ("state", "ON")], got)
    vm.call("dp_event", inst=12, ch=1, key="state", value="ON", initial=False)
    vm.call("dp_event", inst=12, ch=1, key="brightness", value=127, initial=False)
    got = sets(vm.call("knx_write", inst=12, ko=3, raw="40"))
    check("LED2201G8 on, KO3 25 %: brightness_no_onoff 64, then On (it never gets Move to Level with On/Off)",
          got == [("brightness_no_onoff", 64), ("state", "ON")], got)
    vm.call("knx_write", inst=12, ko=1, raw="00")
    vm.call("dp_event", inst=12, ch=1, key="state", value="OFF", initial=False)
    vm.call("params", inst=12, p=T.segment(TransitionTime=15))
    got = sets(vm.call("knx_write", inst=12, ko=1, raw="01"))
    check("LED2201G8 off, KO1 on with 1.5 s: brightness_no_onoff 127 (last brightness), then On (the lamp's own fade)",
          got == [("brightness_no_onoff", 127), ("state", "ON")], got)
    lamp(vm, 13, LED2201G8)
    vm.call("dp_event", inst=13, ch=1, key="state", value="OFF", initial=False)
    got = sets(vm.call("knx_write", inst=13, ko=2, raw="0b"))
    check("LED2201G8 off, KO2 up, step code 3: one command, the minimum 3 plus the step = 67 while off, then On",
          got == [("brightness_no_onoff", 67), ("state", "ON")], got)
    got = sets(vm.call("knx_write", inst=13, ko=2, raw="0b"))
    check("LED2201G8 just switched on (set value 67), KO2 up again: 67 -> 131 in 1.1 s without On/Off, then On",
          got == [("transition", 1.1), ("brightness_no_onoff", 131), ("state", "ON")], got)
    lamp(vm, 14, LED1736G9)
    vm.call("dp_event", inst=14, ch=1, key="state", value="OFF", initial=False)
    got = sets(vm.call("knx_write", inst=14, ko=3, raw="80"))
    check("a model without flag 16 (LED1736G9) off, KO3 50 %: brightness 127 with On/Off, no On",
          got == [("brightness", 127)], got)
    # a stop with relative off: never with On/Off to the LED2201G8, as before to any other model
    lamp(vm, 15, LED2201G8)
    vm.call("params", inst=15, p=T.segment(RelativeOffEnable=1))
    got = sets(vm.call("knx_write", inst=15, ko=2, raw="00"))
    check("LED2201G8, relative off on, KO2 stop: Stop without On/Off", got == [("brightness_move", "stop")], got)
    lamp(vm, 16, LED1736G9)
    vm.call("params", inst=16, p=T.segment(RelativeOffEnable=1))
    got = sets(vm.call("knx_write", inst=16, ko=2, raw="00"))
    check("LED1736G9, relative off on, KO2 stop: Stop with On/Off as before", got == [("brightness_move_onoff", "stop")], got)
    # relative colour temperature, step code 1: MINCT..MAXCT go with the command (KNX 07_20_02 p. 96, table 20)
    lamp(vm, 17, LED1736G9)
    vm.call("params", inst=17, p=T.segment(CtMinimum=2700, CtMaximum=4000))
    got = sets(vm.call("knx_write", inst=17, ko=8, raw="09"))
    check("KO8 step code 1 with MINCT 2700 K and MAXCT 4000 K: color_temp_move -55 within 250..370 mired",
          got == [("color_temp_move", "-55,250,370")], got)
    # memory function: the last brightness reported while on (07_20_02 p. 42), not one reported while off
    lamp(vm, 18, LED1736G9, brightness=120)
    vm.call("knx_write", inst=18, ko=1, raw="00")
    vm.call("dp_event", inst=18, ch=1, key="state", value="OFF", initial=False)
    vm.call("dp_event", inst=18, ch=1, key="brightness", value=1, initial=False)
    got = sets(vm.call("knx_write", inst=18, ko=1, raw="01"))
    check("memory function: on with 120 (reported while on), not 1 (reported while off)", got == [("brightness", 120)], got)
    # LED2201G8 that the class believes on (its last report), whatever it really is: level, then On
    lamp(vm, 20, LED2201G8)
    got = sets(vm.call("knx_write", inst=20, ko=3, raw="80"))
    check("LED2201G8 reported on: KO3 50 % -> brightness_no_onoff 127, then On (a lamp that is off comes on too)",
          got == [("brightness_no_onoff", 127), ("state", "ON")], got)
    # dimming down never switches on: a LED2201G8 that is off but believed on stays off, as a KNX dimming actuator would
    got = sets(vm.call("knx_write", inst=20, ko=2, raw="04"))
    check("LED2201G8 believed on at 127, KO2 down 12.5 %: brightness_no_onoff 95 in 0.5 s, no On",
          got == [("transition", 0.5), ("brightness_no_onoff", 95)], got)
    # memory function after dimming to off with relative off: the level before the way down, not the last on it
    lamp(vm, 21, LED1736G9, brightness=120)
    vm.call("params", inst=21, p=T.segment(RelativeOffEnable=1))
    got = sets(vm.call("knx_write", inst=21, ko=2, raw="01"))
    check("relative off, KO2 down by the whole range from 120: Move with On/Off down", got == [("brightness_move_onoff", -60)], got)
    vm.call("dp_event", inst=21, ch=1, key="brightness", value=1, initial=False)
    vm.call("dp_event", inst=21, ch=1, key="state", value="OFF", initial=False)
    got = sets(vm.call("knx_write", inst=21, ko=1, raw="01"))
    check("then KO1 on: 120 (before the way down), not 1 (reported on the way to off)", got == [("brightness", 120)], got)
    # no state reported yet: a reported brightness counts as on
    vm.call("dp_event", inst=19, ch=0, key="zigbeeModel", value=LED1736G9, initial=True)
    vm.call("dp_event", inst=19, ch=1, key="brightness", value=100, initial=True)
    vm.call("params", inst=19, p=T.segment())
    got = sets(vm.call("knx_write", inst=19, ko=2, raw="0c"))
    check("no state yet, brightness 100: KO2 up 12.5 % from 100 to 132 in 0.5 s, no jump to the minimum first",
          got == [("transition", 0.5), ("brightness", 132)], got)
    # switched on by a value: the value status waits for the lamp's new level (the dimmable test has more of this)
    lamp(vm, 22, LED1736G9, brightness=67)
    vm.call("dp_event", inst=22, ch=1, key="state", value="OFF", initial=False)
    vm.call("knx_write", inst=22, ko=3, raw="bf")
    ops = vm.call("dp_event", inst=22, ch=1, key="state", value="ON", initial=False)
    check("LED1736G9 off at 67, KO3 75 %: its On -> KO4 1 only, no KO5 with the old 67",
          status(ops) == [(4, "01", False)], status(ops))
    ops = vm.call("dp_event", inst=22, ch=1, key="brightness", value=190, initial=False)
    check("then its brightness 190 -> KO5 0xbf", status(ops) == [(5, "bf", False)], status(ops))
    # LED2201G8: its level is set while off and reported before the On, so the On carries the new value at once
    lamp(vm, 23, LED2201G8, brightness=67)
    vm.call("dp_event", inst=23, ch=1, key="state", value="OFF", initial=False)
    vm.call("knx_write", inst=23, ko=3, raw="bf")
    ops = vm.call("dp_event", inst=23, ch=1, key="brightness", value=190, initial=False)
    check("LED2201G8 off at 67, KO3 75 %: its level 190 while off -> no KO5 yet", status(ops) == [], status(ops))
    ops = vm.call("dp_event", inst=23, ch=1, key="state", value="ON", initial=False)
    check("then its On -> KO4 1 and KO5 0xbf", status(ops) == [(4, "01", False), (5, "bf", False)], status(ops))
    vm.close()
    w = set(T.manifest["datapoints"]["write"])
    e = set(T.manifest["datapoints"]["events"])
    check("datapoints declared", {"1.color_temp", "1.color_temp_move", "1.color_temp_startup", "1.brightness_no_onoff"} <= w and
          {"0.zigbeeModel", "1.colorTempPhysicalMin", "1.colorTempPhysicalMax", "1.color_temp"} <= e, (w, e))
    return check.finish()


if __name__ == "__main__":
    sys.exit(main())
