# zigbee2knx

![zigbee2knx: Zigbee lights and plugs as KNX devices, set up in ETS](docs/images/hero.jpg)

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

## Concept

**The firmware is generic; the devices are data.** The firmware knows Zigbee (the gateway is the coordinator), KNX
(the link and a device server for the virtual devices) and [Berry](https://github.com/berry-lang/berry), a small
scripting language. It knows no Zigbee device model. What a device is and how it behaves comes from its product
file: a device class is a KNX application, and its behaviour is a Berry program inside that application's parameter
segment. When ETS downloads the application, the program comes along; the gateway checks its signature, loads it and
runs every device of the class with it. The variety of supported devices therefore lies in the `.knxprod` files, not
in the firmware of the ESP32-C6.

Describing devices as Berry templates is the approach of [BREMA](https://github.com/tostmann/BREMA), a rule engine
for KNX and radio sticks that handles its devices the same way; zigbee2knx carries it into KNX product data.

**Compared with Zigbee2MQTT.** [Zigbee2MQTT](https://www.zigbee2mqtt.io/) supports a device through a converter, a
piece of JavaScript in zigbee-herdsman-converters or loaded as an external converter, which runs on the host next to
the Zigbee stack. zigbee2knx distils those device definitions into KNX classes and products: models that behave
alike share one class, and what differs between them is a table in the class's Berry program.

| | Zigbee2MQTT | zigbee2knx |
|---|---|---|
| Support for a device lives in | a converter in the host software | a Berry program inside the KNX product file |
| A device it does not know yet needs | a converter | a product file, imported into ETS |
| Configured with | its web front end and MQTT | ETS: parameters and group objects |
| Runs on | a host with a Zigbee adapter | the gateway alone, on the KNX bus |

**More devices: contributions welcome.** Support for further devices depends almost only on product files.

- Another model of an existing class (another lamp that is a dimmable light, another plug) is only a catalog
  product: `distill/knxprod_products.py --append` adds it without touching the signed program, so anybody can build
  and import such a file.
- A new class (a kind of device not covered yet) is a manifest and a Berry program under [`templates/`](templates/),
  tested in the host VM ([`tests/`](tests/)). Send it as a pull request: once reviewed, it is signed with the
  project's release keys and goes out in a product file that every gateway accepts.

[docs/templates.md](docs/templates.md) describes a class, [docs/building-knxprod.md](docs/building-knxprod.md) the
product files.

**Why the programs are signed.** Berry is no sandbox: a program in the gateway can do what the firmware can, on the
KNX bus and in the Zigbee network. So the gateway runs only programs signed with a key compiled into its firmware.
For the images of this project these are the release keys, whose private halves exist only in two hardware keys. A
product file whose program was changed or comes from someone else is refused, and the download ends with an error in
ETS: importing a product file from an unknown source cannot bring code into an installation. Products added to an
existing class keep the signed program as it is and need no key. Whoever wants to run programs of their own builds
the firmware with their own key ([docs/templates.md](docs/templates.md#templates-of-your-own)). This signature is
not the one ETS checks on a `.knxprod`: that one covers the product data for ETS; the program's signature protects
the gateway.

## In ETS

The device classes in the catalog, one product per model:

![ETS catalog with the Zigbee2KNX sections and the IKEA lights](docs/images/ets-catalog.png)

A gateway, two lights and a plug, each a KNX device with its own address, application and serial number:

![ETS device list with the gateway and three Zigbee devices](docs/images/ets-devices.png)

Pairing from ETS: while ETS programs the address 15.15.3, the plug joins the Zigbee network and takes the address
(group monitor below):

![ETS group monitor while a Zigbee plug is paired](docs/images/ets-pairing.png)

The group objects of a white spectrum light, linked like those of any KNX actuator:

![Group objects of a white spectrum light](docs/images/ets-group-objects.png)

The gateway's diagnostic objects with their last values:

![Group addresses of the gateway's diagnostic objects](docs/images/ets-gateway-diagnostics.png)

The title image is a generated scene with a real ETS screenshot on the laptop.

## Hardware

Runs best with the busware **TUL32** (KNX TP, powered over USB) and the bus-powered **TULX32** (experimental, see [Status](#status)): <https://busware.de>.

Other ESP32-C6 boards with an NCN5130 need their pins in [`firmware/main/board.h`](firmware/main/board.h).

## Getting started

1. **Flash the firmware** from the browser with the [web installer](https://tostmann.github.io/zigbee2knx/) (Chrome,
   Edge or Opera), or with esptool from the images attached to the [releases](../../releases); to build it yourself
   see [docs/building-firmware.md](docs/building-firmware.md). The TULX32 has no USB port for this; it installs
   firmware from its recovery system ([install.busware.de/tulx](https://install.busware.de/tulx/)).
2. **Build the product files** with OpenKNXproducer and your ETS, as described
   [below](#building-the-product-files), and import them into ETS.
3. **Add the gateway.** Insert "Zigbee gateway (TUL32)" into a line, program its individual address (button S1),
   set the range of device numbers for Zigbee devices and download it.
4. **Add a Zigbee device.** Insert its product (for example "IKEA LED1934G3 …") into the same line with an address in
   that range and program the individual address. While ETS waits, reset the Zigbee device or put it into pairing
   mode: it joins and takes the address. Then download the application and link the group objects.

[docs/getting-started.md](docs/getting-started.md) has the details, the gateway's objects and the known limitations.

## Building the product files

ETS takes devices from `.knxprod` files, and a `.knxprod` has to be signed by the signing library of an ETS. This
repository holds the source of the product files in [`producer/`](producer/), one XML file per device class.
[OpenKNXproducer](https://github.com/OpenKNX/OpenKNXproducer) turns them into `.knxprod` files with the ETS installed
on your PC:

1. On a Windows PC with ETS installed, download OpenKNXproducer from its
   [releases](https://github.com/OpenKNX/OpenKNXproducer/releases).
2. Copy the XML files from `producer/` into a folder and run OpenKNXproducer there, once per file:

       OpenKNXproducer create zigbee-gateway
       OpenKNXproducer create zigbee-dimmable-light
       OpenKNXproducer create zigbee-white-spectrum-light
       OpenKNXproducer create zigbee-switch
       OpenKNXproducer create zigbee-remote

   Each run checks the file and writes `<name>.knxprod` next to it.
3. Import the `.knxprod` files into the ETS catalog. The products appear under KNX Association → Zigbee2KNX, in the
   sections Gateway, Lights, Plugs and switches and Remotes.

The files use M-00FA, the manufacturer ID of the KNX Association, as open-source KNX projects do; they are not
registered with the KNX Association. The same XML gives the same applications in every build, so product files built
on different PCs can be used side by side (checked with OpenKNXproducer 4.3.12 and ETS 6.4.1).
[docs/building-knxprod.md](docs/building-knxprod.md) covers adding products and new application versions.

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

What has run with ETS 6.4.1 on a TUL32 with the firmware of this source and the applications in `producer/`
(0xC000 v3, 0xC001 v3, 0xC002 v4, 0xC003 v3): import of the product files, a new project, downloads with long frames,
pairing a plug from ETS, switching, dimming and colour temperature with status feedback, and the gateway's diagnostic
objects. With earlier development builds of 0.6 also: partial downloads, the "Zigbee device reachable" object in both
directions, unloading an address (the device is removed and asked to leave the Zigbee network), and updating a device
to a newer application version. Devices on the bench: IKEA LED1934G3, LED2201G8 and E2204.

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
