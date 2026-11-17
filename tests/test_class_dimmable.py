#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dirk Tostmann
"""Class zigbee-dimmable-light (FB Dimming Actuator Basic) in the firmware's Berry VM, as the IKEA LED1934G3 and
as a model the table does not know. No device, no network.

Relative dimming (DPT 3.007, KNX 07_20_02 state tables p. 44-46): step code 1..7 moves the set value by 1/2^(code-1)
of 0..100 % (254 brightness steps: 254, 127, 64, 32, 16, 8, 4) within MIN_SV..MAX_SV at the dimming rate, 0 stops.
A step counts from the target of the last command while it runs and for 1 s after (the gateway reads the lamp back
0.7 s after a command), then from what the lamp reports. The step checks use an instance each, so that only the one
check that waits depends on time."""
import sys
import time

from vmtest import Checks, Template, Vm, availability_checks, effects, rebuild_checks, sets, status

T = Template("zigbee-dimmable-light")
LED1934G3 = "TRADFRIbulbE27WWclear250lm"           # table: min_on 2 (turns off at brightness 1)


def lamp(vm, inst, brightness=100, state="ON", **params):
    """A fresh instance: the LED1934G3 reporting `brightness` and `state`, then the parameters."""
    vm.call("dp_event", inst=inst, ch=0, key="zigbeeModel", value=LED1934G3, initial=True)
    vm.call("dp_event", inst=inst, ch=1, key="brightness", value=brightness, initial=True)
    vm.call("dp_event", inst=inst, ch=1, key="state", value=state, initial=True)
    vm.call("params", inst=inst, p=T.segment(**params))


def dim(vm, inst, raw):
    return sets(vm.call("knx_write", inst=inst, ko=2, raw=raw))


def main():
    check = Checks()
    m = T.manifest
    rebuild_checks(check, T, app_version=3, replaces=[2], available=7, section=("Lights", "Lights"))
    check("LED1934G3 is in the class table", LED1934G3 in m["match"]["zigbeeModel"], m["match"]["zigbeeModel"][:5])
    vm = Vm(T)
    vm.call("dp_event", ch=0, key="zigbeeModel", value=LED1934G3, initial=True)
    ops = vm.call("params", p=T.segment())
    check("params write power-on 'on' and startup 'previous'",
          sets(ops) == [("power_on_behavior", "on"), ("level_config.current_level_startup", "previous")], sets(ops))
    vm.call("dp_event", ch=1, key="brightness", value=200, initial=True)
    ops = vm.call("dp_event", ch=1, key="state", value="ON", initial=True)
    check("initial ON fills the shadows only", status(ops) == [(4, "01", True), (5, "c9", True)], status(ops))
    check("KO1 off -> state OFF", sets(vm.call("knx_write", ko=1, raw="00")) == [("state", "OFF")], None)
    ops = vm.call("knx_write", ko=1, raw="01")
    check("KO1 on with memory function -> last brightness 200", sets(ops) == [("brightness", 200)], sets(ops))
    ops = vm.call("knx_write", ko=3, raw="80")
    check("KO3 50 % -> brightness 127", sets(ops) == [("brightness", 127)], sets(ops))
    ops = vm.call("knx_write", ko=3, raw="01")
    check("KO3 smallest value stays on", sets(ops) == [("brightness", 3)], sets(ops))
    check("KO3 0 -> state OFF", sets(vm.call("knx_write", ko=3, raw="00")) == [("state", "OFF")], None)
    check("KO6 identify (model has it)", sets(vm.call("knx_write", ko=6, raw="01")) == [("identify", "identify")], None)
    check("KO6 identify on 0 as well (DPT 1.017 trigger)",
          sets(vm.call("knx_write", ko=6, raw="00")) == [("identify", "identify")], None)
    ops = vm.call("dp_event", ch=1, key="brightness", value=100, initial=False)
    check("brightness 100 -> KO5 0x64", status(ops) == [(5, "64", False)], status(ops))
    ops = vm.call("dp_event", ch=1, key="state", value="OFF", initial=False)
    check("state OFF -> KO4 0, KO5 0", status(ops) == [(4, "00", False), (5, "00", False)], status(ops))
    ops = vm.call("params", p=T.segment(SwitchOnValue=40, MemoryFunction=0, MinimumValue=20, MaximumValue=80,
                                         RelativeOffEnable=1, TransitionTime=15, DimmingRate=30, StartupLevel=2,
                                         StartupLevelValue=50, PowerOnBehaviour=3))
    check("params: previous / fixed 50 % -> 127",
          sets(ops) == [("power_on_behavior", "previous"), ("level_config.current_level_startup", 127)], sets(ops))
    ops = vm.call("knx_write", ko=1, raw="01")
    check("KO1 on without memory -> transition 1.5 s, 40 % (102)", sets(ops) == [("transition", 1.5), ("brightness", 102)], sets(ops))
    ops = vm.call("knx_write", ko=3, raw="ff")
    check("KO3 100 % clamped to 80 % (203)", sets(ops) == [("transition", 1.5), ("brightness", 203)], sets(ops))
    check("KO2 darker by the whole range with relative off -> dims to off: brightness_move_onoff -30",
          sets(vm.call("knx_write", ko=2, raw="01")) == [("brightness_move_onoff", -30)], None)
    availability_checks(check, T, vm, inst=1)
    ops = vm.call("dp_event", ch=1, key="state", value="ON", initial=False)
    check("after availability: state ON -> KO4 1, KO5 0x64 only",
          status(ops) == [(4, "01", False), (5, "64", False)] and len(effects(ops)) == 2, effects(ops))
    # instance 2: a lamp the table does not know -> class defaults (identify and startup level assumed)
    vm.call("dp_event", inst=2, ch=0, key="zigbeeModel", value="UNKNOWN-LAMP", initial=True)
    ops = vm.call("params", inst=2, p=T.segment())
    check("unknown model: params still set startup level", ("level_config.current_level_startup", "previous") in sets(ops), sets(ops))
    check("unknown model: KO6 identify", sets(vm.call("knx_write", inst=2, ko=6, raw="01")) == [("identify", "identify")], None)

    # relative dimming, defaults: MIN_SV 1 % (3, the model turns off at 1), MAX_SV 100 % (254), 60 steps/s
    up = {1: (254, 2.6), 2: (227, 2.1), 3: (164, 1.1), 4: (132, 0.5), 5: (116, 0.3), 6: (108, 0.1), 7: (104, 0.1)}
    for code, (to, t) in up.items():
        lamp(vm, 10 + code)
        got = dim(vm, 10 + code, f"{0x08 | code:02x}")
        check(f"KO2 up, step code {code}: from 100 to {to} in {t} s", got == [("transition", t), ("brightness", to)], got)
    for code, to, t in ((1, 3, 1.6), (3, 36, 1.1), (4, 68, 0.5)):
        lamp(vm, 20 + code)
        got = dim(vm, 20 + code, f"{code:02x}")
        check(f"KO2 down, step code {code}: from 100 to {to} in {t} s", got == [("transition", t), ("brightness", to)], got)
    lamp(vm, 30)
    dim(vm, 30, "0c")
    got = dim(vm, 30, "0c")
    check("KO2 up 12.5 % twice at once: the second counts from the first target (132 -> 164)",
          got == [("transition", 0.5), ("brightness", 164)], got)
    lamp(vm, 31, state="OFF")
    got = dim(vm, 31, "0c")
    check("KO2 up from off: on at MIN_SV (3), then up by the step to 35",
          got == [("brightness", 3), ("transition", 0.5), ("brightness", 35)], got)
    lamp(vm, 32, state="OFF")
    check("KO2 down from off: nothing", dim(vm, 32, "04") == [], None)
    lamp(vm, 33, brightness=190, MinimumValue=20, MaximumValue=80)
    got = dim(vm, 33, "0b")
    check("KO2 up 25 % from 190 stops at MAX_SV 80 % (203)", got == [("transition", 0.2), ("brightness", 203)], got)
    lamp(vm, 34, MinimumValue=20, MaximumValue=80)
    got = dim(vm, 34, "01")
    check("KO2 down by the whole range stops at MIN_SV 20 % (51)", got == [("transition", 0.8), ("brightness", 51)], got)
    lamp(vm, 35, RelativeOffEnable=1)
    got = dim(vm, 35, "04")
    check("relative off: a step that stays above MIN_SV is a step (100 -> 68)",
          got == [("transition", 0.5), ("brightness", 68)], got)
    lamp(vm, 36, brightness=40, RelativeOffEnable=1)
    got = dim(vm, 36, "03")
    check("relative off: a step below MIN_SV dims to off", got == [("brightness_move_onoff", -60)], got)
    lamp(vm, 37, brightness=40)
    got = dim(vm, 37, "03")
    check("without relative off the same step stops at MIN_SV (3)", got == [("transition", 0.6), ("brightness", 3)], got)
    lamp(vm, 38)
    check("KO2 stop", dim(vm, 38, "08") == [("brightness_move", "stop")], None)
    lamp(vm, 39, RelativeOffEnable=1)
    check("KO2 stop with relative off", dim(vm, 39, "00") == [("brightness_move_onoff", "stop")], None)
    lamp(vm, 40)
    dim(vm, 40, "09")                                  # up by the whole range: 100 -> 254 in 2.6 s
    dim(vm, 40, "08")                                  # stopped right away: where the lamp has got to counts
    got = dim(vm, 40, "07")
    check("stopped right after the start, a step down counts from where it stopped (about 100 -> 96)",
          len(got) == 2 and got[0] == ("transition", 0.1) and got[1][0] == "brightness" and 95 <= got[1][1] <= 99, got)
    lamp(vm, 41)
    vm.call("knx_write", inst=41, ko=3, raw="80")     # 50 % = 127
    got = dim(vm, 41, "0c")
    check("a step right after an absolute value counts from that value (127 -> 159)",
          got == [("transition", 0.5), ("brightness", 159)], got)
    time.sleep(1.6)                                    # the step has run (0.5 s) and the 1 s after it
    vm.call("dp_event", inst=41, ch=1, key="brightness", value=140, initial=False)   # the lamp reports something else
    got = dim(vm, 41, "0c")
    check("once the step has run and the lamp has reported, the next one counts from the report (140 -> 172)",
          got == [("transition", 0.5), ("brightness", 172)], got)
    vm.close()
    return check.finish()


if __name__ == "__main__":
    sys.exit(main())
