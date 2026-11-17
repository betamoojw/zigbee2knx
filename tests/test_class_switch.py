#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dirk Tostmann
"""Class zigbee-switch (one switched output) in the firmware's Berry VM, as the IKEA TRETAKT E2204, as the
TRADFRI control outlet (no child lock) and as a model the table does not know. The model settings (power-on
behaviour, child lock) wait until the device has told its model, as a gateway that hands over the parameters before
it asks the model has it. No device, no network."""
import sys

from vmtest import Checks, Template, Vm, availability_checks, effects, rebuild_checks, sets, status

T = Template("zigbee-switch")
TRETAKT = "TRETAKT Smart plug"
OUTLET = "TRADFRI control outlet"
LED1934G3 = "TRADFRIbulbE27WWclear250lm"            # replaces the TRETAKT below, a model the class does not know
LOCK_ON = [("power_on_behavior", "on"), ("zcl.write", "fc85,0000,10,117c,1")]


def main():
    check = Checks()
    m = T.manifest
    rebuild_checks(check, T, app_version=2, replaces=[1], objects=4)
    check("TRETAKT is in the class table", TRETAKT in m["match"]["zigbeeModel"], m["match"]["zigbeeModel"])
    orders = [p["model"] for p in m["products"]]
    check("a product for the number on the box (E2204) and for the Zigbee2MQTT model (E22x4)",
          "E2204" in orders and "E22x4" in orders, orders)
    vm = Vm(T)
    vm.call("dp_event", ch=0, key="zigbeeModel", value=TRETAKT, initial=True)
    ops = vm.call("params", p=T.segment())
    check("params: power-on 'on', child lock off as a manufacturer-specific write",
          sets(ops) == [("power_on_behavior", "on"), ("zcl.write", "fc85,0000,10,117c,0")], sets(ops))
    ops = vm.call("params", p=T.segment(PowerOnBehaviour=3, ChildLock=1))
    check("params: power-on 'previous', child lock on",
          sets(ops) == [("power_on_behavior", "previous"), ("zcl.write", "fc85,0000,10,117c,1")], sets(ops))
    ops = vm.call("dp_event", ch=1, key="state", value="ON", initial=True)
    check("initial ON fills the shadow only", status(ops) == [(2, "01", True)], status(ops))
    check("KO1 off -> state OFF", sets(vm.call("knx_write", ko=1, raw="00")) == [("state", "OFF")], None)
    check("KO1 on -> state ON", sets(vm.call("knx_write", ko=1, raw="01")) == [("state", "ON")], None)
    check("KO3 identify", sets(vm.call("knx_write", ko=3, raw="01")) == [("identify", "identify")], None)
    ops = vm.call("dp_event", ch=1, key="state", value="OFF", initial=False)
    check("state OFF -> KO2 0", status(ops) == [(2, "00", False)], status(ops))
    check("brightness from a device is ignored", status(vm.call("dp_event", ch=1, key="brightness", value=254,
                                                                 initial=False)) == [], None)
    availability_checks(check, T, vm, inst=1)
    ops = vm.call("dp_event", ch=1, key="state", value="ON", initial=False)
    check("after availability: state ON -> KO2 1 only", status(ops) == [(2, "01", False)] and len(effects(ops)) == 1,
          effects(ops))
    # instance 2: the control outlet has no child lock
    vm.call("dp_event", inst=2, ch=0, key="zigbeeModel", value=OUTLET, initial=True)
    ops = vm.call("params", inst=2, p=T.segment(ChildLock=1))
    check("control outlet: no child lock write", sets(ops) == [("power_on_behavior", "on")], sets(ops))
    # instance 3: a plug the table does not know -> class defaults (power-on and identify, no child lock)
    vm.call("dp_event", inst=3, ch=0, key="zigbeeModel", value="UNKNOWN-PLUG", initial=True)
    ops = vm.call("params", inst=3, p=T.segment(ChildLock=1))
    check("unknown model: power-on only", sets(ops) == [("power_on_behavior", "on")], sets(ops))
    check("unknown model: KO3 identify", sets(vm.call("knx_write", inst=3, ko=3, raw="01")) == [("identify", "identify")],
          None)
    # instance 4: parameters before the model (a fresh VM in the gateway), then a replacement by another device
    ops = vm.call("params", inst=4, p=T.segment(ChildLock=1))
    check("params before the model: nothing written yet", effects(ops) == [], effects(ops))
    ops = vm.call("dp_event", inst=4, ch=0, key="zigbeeModel", value=TRETAKT, initial=True)
    check("model arrives: power-on and child lock written", sets(ops) == LOCK_ON, sets(ops))
    ops = vm.call("dp_event", inst=4, ch=0, key="zigbeeModel", value=TRETAKT, initial=False)
    check("same model told again: nothing written", effects(ops) == [], effects(ops))
    ops = vm.call("params", inst=4, p=T.segment(ChildLock=1))
    check("params again with the model known (the gateway sends them once more): written again", sets(ops) == LOCK_ON,
          sets(ops))
    vm.call("forget", inst=4)                         # the gateway replaces the device (vdev_replace -> forget)
    ops = vm.call("params", inst=4, p=T.segment(ChildLock=1))
    check("replaced: params wait for the new device's model", effects(ops) == [], effects(ops))
    ops = vm.call("dp_event", inst=4, ch=0, key="zigbeeModel", value=LED1934G3, initial=True)
    check("replaced by a lamp: power-on only, no child lock of the plug before",
          sets(ops) == [("power_on_behavior", "on")], sets(ops))
    vm.close()
    return check.finish()


if __name__ == "__main__":
    sys.exit(main())
