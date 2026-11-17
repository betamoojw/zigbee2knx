#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dirk Tostmann
"""Class zigbee-dimmable-light (FB Dimming Actuator Basic) in the firmware's Berry VM, as the IKEA LED1934G3 and
as a model the table does not know. No device, no network."""
import sys

from vmtest import Checks, Template, Vm, availability_checks, effects, rebuild_checks, sets, status

T = Template("zigbee-dimmable-light")
LED1934G3 = "TRADFRIbulbE27WWclear250lm"


def main():
    check = Checks()
    m = T.manifest
    rebuild_checks(check, T, app_version=3, replaces=[2], objects=7)
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
    check("KO2 brighter -> brightness_move +60", sets(vm.call("knx_write", ko=2, raw="0b")) == [("brightness_move", 60)], None)
    check("KO2 darker -> brightness_move -60", sets(vm.call("knx_write", ko=2, raw="03")) == [("brightness_move", -60)], None)
    check("KO2 stop", sets(vm.call("knx_write", ko=2, raw="08")) == [("brightness_move", "stop")], None)
    check("KO6 identify (model has it)", sets(vm.call("knx_write", ko=6, raw="01")) == [("identify", "identify")], None)
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
    check("KO2 darker with relative off -> brightness_move_onoff -30",
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
    vm.close()
    return check.finish()


if __name__ == "__main__":
    sys.exit(main())
