# Building the firmware

The firmware is the ESP-IDF project in `firmware/`. It is built with ESP-IDF 6.0.1; the component manager fetches
esp-zigbee-lib 2.1.0 (`firmware/main/idf_component.yml`, pinned in `firmware/dependencies.lock`). The Berry VM is part
of the tree (`firmware/components/berry`), and the build embeds `berry/prelude.be` and the two light classes from
`templates/`, so build from a checkout of the whole repository.

## TUL32

    cd firmware
    idf.py build
    idf.py -p PORT flash

The result is `build/zigbee2knx.bin`. The partition table (`partitions.csv`) has one application in `factory` at
0x10000, `zb_storage` for the Zigbee network, `devsrv` for the records of the virtual KNX devices (address, tables,
parameters, templates) and `coredump`. Writing a new application keeps the Zigbee network and the KNX devices.

The console is on the USB port (USB-Serial-JTAG).

## TULX32

    cd firmware
    idf.py -B build-tulx -DSDKCONFIG=build-tulx/sdkconfig "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.tulx" build

The TULX32 has a fixed partition table (`partitions_tulx.csv`) with a recovery system in `factory`. The application
belongs in `ota_0` at 0x10000:

    python -m esptool --chip esp32c6 -p PORT write-flash 0x10000 build-tulx/zigbee2knx.bin

**Do not use `idf.py flash` for this build:** with this partition table ESP-IDF writes the application into `factory`
and overwrites the recovery system.

On the TULX32 the records of the virtual devices go to `actor_db`, and the Zigbee network to `nvs`, which the
recovery system uses as well. The firmware never erases `nvs` on this board; the console command `zbreset` is refused
there. The console is on UART0 at the service header.

## Board

`firmware/main/board.h` holds the UART and pins of the NCN5130, the LED, the button S1 and the partitions the records
and the Zigbee network go to. The TULX32 build is selected by `CONFIG_Z2K_BOARD_TULX32` in `sdkconfig.tulx`, which
also moves the console to UART0 and selects the TULX32's partition table.

## Tests

The host tests compile the link layer, the device server, pairing and the Zigbee link for the PC and run them on a
virtual clock, without hardware: see [firmware/test/host/README.md](../firmware/test/host/README.md).

The template tests run every class in the firmware's Berry VM, built for the PC:

    berry/build_hostvm.sh                       # -> build/hostvm/knxgw-berry
    python3 tests/test_class_dimmable.py
    python3 tests/test_class_white_spectrum.py
    python3 tests/test_class_switch.py
    python3 tests/test_class_remote.py
    python3 tests/test_knxprod_append.py

`KNXGW_BERRY=/path/to/knxgw-berry` points the tests to a VM built elsewhere.
