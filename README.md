# zigbee2knx

Zigbee devices as KNX devices, with one gateway that is the Zigbee coordinator itself.

zigbee2knx is firmware for an ESP32-C6 with an NCN5130 KNX TP transceiver. The gateway forms its own Zigbee
network and turns every paired Zigbee device into a KNX device of its own: the device has its own individual address
and its own application with parameters and group objects in ETS, and you program it from ETS like any other KNX
device. No hub, no Home Assistant and no IP network sit in between. In operation the gateway needs nothing but the
KNX bus.

> **Status: preview (0.6).** Runs with IKEA lights and plugs and ETS 6.4.1. Not released for production
> installations yet; see [Status](#status).

## How it works

- **One KNX device per Zigbee device.** The gateway answers for up to 32 virtual KNX devices in its own line. It
  acknowledges their frames itself, so no coupler is needed.
- **Pairing from ETS.** When ETS programs an individual address in the gateway's range, the gateway opens the
  Zigbee network, and the first device that joins becomes that KNX device. A group object and the button on the
  gateway open a pairing window as well.
- **Device classes instead of device drivers.** A class (dimmable light, white spectrum light, switch/plug, remote)
  is one KNX application with its parameters and group objects, plus a small [Berry](https://github.com/berry-lang/berry)
  program that maps KNX to Zigbee. The models a class serves and their quirks come from the device definitions of
  [Zigbee2MQTT](https://github.com/Koenkk/zigbee-herdsman-converters), distilled into KNX functional blocks.
- **The behaviour travels with the product data.** The Berry program is embedded and signed in the parameter segment
  of the application. An ETS download brings it into the gateway, so a new device type needs no firmware update. The
  gateway runs only programs whose signature it trusts.

## Hardware

Runs best with the busware **TUL32** (KNX TP, powered over USB) and the bus-powered **TULX32** (experimental, see [Status](#status)): <https://busware.de>.

Other ESP32-C6 boards with an NCN5130 need their pins in [`firmware/main/board.h`](firmware/main/board.h).

## Getting started

1. **Flash the firmware.** Images are attached to the [releases](../../releases); to build it yourself see
   [docs/building-firmware.md](docs/building-firmware.md).
2. **Build the product files.** The KNX product data is in [`producer/`](producer/) as input for OpenKNXproducer;
   [docs/building-knxprod.md](docs/building-knxprod.md) shows how to build the `.knxprod` files with your own ETS.
   Import them into ETS.
3. **Add the gateway.** Insert "Zigbee gateway (TUL32)" into a line, program its individual address (button S1),
   set the range of device numbers for Zigbee devices and download it.
4. **Add a Zigbee device.** Insert its product (for example "IKEA LED1934G3 …") into the same line with an address in
   that range and program the individual address. While ETS waits, reset the Zigbee device or put it into pairing
   mode: it joins and takes the address. Then download the application and link the group objects.

[docs/getting-started.md](docs/getting-started.md) has the details, the gateway's objects and the known limitations.

## Supported devices

| Application | Class | Products |
|---|---|---|
| 0xC000 | Zigbee gateway | the gateway itself |
| 0xC001 | dimmable light | 23 IKEA models |
| 0xC002 | white spectrum light | 48 IKEA models |
| 0xC003 | switch and plug | 14 IKEA models |
| 0xC004 | remote (experimental) | 6 IKEA models |

Each class also has a generic product. A model the class does not list runs with the class defaults; the gateway
reports it. The full list is in [docs/devices.md](docs/devices.md).

## Status

What has run with ETS 6.4.1 on a TUL32: import of the product files, programming the gateway, pairing a device from
ETS, full and partial downloads including the embedded program, switching and dimming with status feedback, the
"Zigbee device reachable" object in both directions, unloading an address (the device is removed and asked to leave
the Zigbee network), and updating a device to a newer application version. Devices on the bench: IKEA LED1934G3,
LED2201G8 and E2204.

Not done yet: the remote class with ETS, long frames through line couplers and USB interfaces, the TULX32's bus
current with Zigbee running, and installations with more than a few real Zigbee devices. Known issues are listed in
[docs/getting-started.md](docs/getting-started.md#known-limitations).

## Repository

| Directory | Content |
|---|---|
| `firmware/` | ESP-IDF project: Zigbee coordinator, KNX link layer and device server, Berry VM, host tests |
| `templates/` | the device classes: `manifest.json` (KNX application, Zigbee mapping) and the Berry program |
| `producer/` | the KNX product data as OpenKNXproducer input, one file per class, with the program embedded |
| `berry/` | host API of the templates (`prelude.be`, `PROTOCOL.md`) and the host VM for the tests |
| `distill/` | Zigbee2MQTT definitions to classes and product lists |
| `tools/` | product data generator and the signed template container (KGT2) |
| `tests/` | template tests in the host VM, product list tests |
| `docs/` | building, getting started, templates, device list |

## License

GPL-3.0-only, with an additional permission under section 7 for linking with Espressif's libraries that come in
object code form (see [NOTICE.md](NOTICE.md)). Third-party parts and their licenses are listed there as well.

zigbee2knx is not affiliated with or endorsed by the KNX Association, the Connectivity Standards Alliance, IKEA or the
Zigbee2MQTT project. Product names are used only to identify the devices.
