# Getting started

This page assumes a gateway running the firmware of this repository, ETS 6 (tested with 6.4.1) and the product files
built from `producer/` imported into ETS ([building-knxprod.md](building-knxprod.md)). Where ETS menus are named,
the names are those of ETS 6.4.1 with a German interface, as they were used; an English interface may name them
differently.

## Topology

The gateway and its Zigbee devices share one KNX line. Each Zigbee device is a virtual KNX device with an individual
address in a range of device numbers you choose in the gateway's parameters; the gateway acknowledges the frames for
all of them. Up to 32 Zigbee devices fit into one gateway. The KNX serial number of a virtual device is derived from
its Zigbee device's IEEE address (a hash of it), so it stays with the Zigbee device.

## The gateway

Insert "Zigbee gateway (TUL32)" into that line and program its individual address: a short press on the button S1
switches the gateway's programming mode, the LED is on while it is.

| Parameter | Default |
|---|---|
| Bind a new Zigbee device when ETS asks for its programming button | yes |
| Addresses for Zigbee devices in this line: first device number | 1 |
| Addresses for Zigbee devices in this line: last device number | 48 |
| Pairing window for the object and the button (s) | 180 |
| Long frames for downloads | Long frames, 55 octets |

While binding from ETS is on, the gateway takes every individual address that ETS programs in its range as a request
to pair a Zigbee device, also when the address is meant for an ordinary KNX device. Keep the range free of other
devices, or switch the binding off while you program them.

| No. | Object | DPT | |
|---|---|---|---|
| 1 | Zigbee pairing | 1.010 | opens and closes the pairing window |
| 2 | Zigbee pairing active | 1.011 | |
| 3 | Zigbee devices with an address | 5.010 | |
| 4 | Zigbee devices waiting for an address | 5.010 | joined through the pairing window, not yet addressed |
| 5 | Free places for Zigbee devices | 5.010 | |
| 6, 7, 8 | Free memory (heap), its minimum since the start, largest free block | 12.001 | read only |
| 9 | Device classes loaded | 5.010 | |
| 10 | Device storage used | 5.001 | |
| 11 | Last message | 16.001 | `<address> <code>`, see below |
| 12 | Fault | 1.005 | 1 while an application is in error or not running, while a Zigbee device does not answer, or after a device was refused for want of a place |

Codes of the last message: `OFF` the Zigbee device does not answer, `ON` it answers again, `MODEL` its model is not
in the class's list (it runs with the class defaults), `DEL` the device was removed, `FULL` refused for want of a
place, `RAM`, `CRC`, `CONF`, `SW`, `LOAD` a download the gateway could not take (ETS then reports the download as
failed), `FLASH` a record does not get into the flash, `DUP` an individual address another of the gateway's devices
already has was refused, `GA` more group addresses than the gateway can acknowledge.

## Pairing a Zigbee device

1. Insert the product of the device into the gateway's line, with a device number inside the gateway's range.
2. Program the individual address. While ETS asks for the programming button, the gateway opens the Zigbee network
   (for three minutes at most; it closes when ETS stops asking).
3. Reset the Zigbee device or put it into pairing mode. The first device that joins becomes this KNX device: it takes
   the address and identifies itself (a lamp blinks). Other devices that join in the same window are asked to leave.
4. Download the application. It carries the parameters and the device class into the gateway.

Most IKEA lights are reset by switching them off and on six times in quick succession; the device pages of
Zigbee2MQTT describe the procedure per model. Do it close to the gateway.

The pairing window is the other way: object 1, or holding S1 for 3 s, opens the Zigbee network for the configured
time (the LED blinks). Devices that join wait without an address (object 4) and are taken, one by one, by the next
address assignments from ETS in the gateway's range.

Every programming mode ends by itself after 4 minutes. While a virtual device is in programming mode, the gateway's
LED flashes briefly once a second.

Note: when ETS programs several devices at once and one of them has no Zigbee device yet, the gateway treats that as a
pairing request and opens the network. The first device that joins in those three minutes gets that address.

## Removing a device

Unload the device in ETS (address and application). ETS needs the device in programming mode for that; ETS 6.4.1
sets it under Monitore → Diagnose → Physikalische Adressen → Überprüfung der physikalischen Adresse → Geräte-LED
„Ein“. A few seconds after the unload the gateway removes the virtual device, asks the Zigbee device to leave the
network and reports `DEL`. A device that is off at that moment gets the request when it comes back. (Both halves
have run with ETS 6.4.1, but not yet in one go.)

IKEA lights do not look for a network by themselves after leaving; reset them by hand before pairing them again.

## A new application version

ETS 6.4.1 offered a newer version of an application only after a device of that version had been inserted into the
project. What worked: insert one device of the new version, open the device to update, Info → Applikation →
„Aktualisieren“, delete the helper device again, download. On the device updated this way the links and a changed
parameter stayed. Changing the version through the dropdown „Applikationsprogramm ändern“ instead reset a changed
parameter to its default.

## Download times

The gateway parameter "Long frames for downloads" sets what all virtual devices announce to ETS: standard frames
(12 data octets per memory write), long frames of 55 octets (the default; every TP1 line coupler has to pass them) or
of 66 octets. A full download of a white spectrum light took about 85 s with standard frames and 45 s with 66 octets
(ETS 6.4.1, through a KNXnet/IP tunnel, no coupler); 55 octets has not been timed. Downloads through a line coupler or
a USB interface with long frames have not been tested yet; if one fails, set the parameter to standard frames.

## Known limitations

- TULX32: the firmware has started, formed its Zigbee network and received telegrams; sending on the bus, pairing,
  downloads and the bus current with Zigbee running have not been tested, and there is no release image for it.
- Loading a new version of a device class while another device still runs the old one needs memory for both. With
  three classes loaded this may not fit, and the device updated last is then left without a class (worked out from
  the code, not yet seen on a device).
- The remote class (0xC004) has not run with ETS.
- Relative colour temperature with step code 1 moves until a stop telegram and is not held within the parameterized
  colour temperature range; the other step codes are.
- A device that was asked to leave knows the network key and can join again while the network is open; install
  codes are not supported.
- There is no backup function yet. The Zigbee network (its key and the joined devices) and the KNX devices live in the
  gateway's flash; the only backup today is a copy of the whole flash over USB (`esptool read-flash`). Restoring such a
  copy, and moving a network to another gateway, have not been tested.

## Console

The TUL32 has a console on its USB port, the TULX32 on UART0 at the service header. `help` lists the commands;
`gw`, `vdev`, `pair`, `zb`, `knx` and `heap` show the state of the gateway, the virtual devices, pairing, the Zigbee
network, the KNX link and the memory.
