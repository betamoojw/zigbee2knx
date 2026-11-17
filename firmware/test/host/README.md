# Host tests for the KNX side of the firmware

These programs compile the real `main/knx_link.c` and `main/vdev.c` for the PC (`#include` of the source file, stubs for
FreeRTOS, NVS, UART, timer and the Berry/Zigbee side in `stubs/`) and run them against a virtual clock. They need no
hardware. They do not replace a run on the bus: the NCN5130 timing model in `link_sim.c` is written from the datasheet,
and nothing here knows what ETS does.

| program | what it runs |
|---|---|
| `link_sim` | the link layer against a model of the NCN5130: host acknowledge in time, our own frames, extended frames (taken, sent with the index offset under both readings of datasheet p. 46, LTE format dropped), a stalled task, a full send queue (cases S1 to S12) |
| `ep_pick_test` | the endpoint rule of `main/ep_pick.h` (full interview): Green Power and foreign profiles left out, actuator before remote, made-up descriptors |
| `zlink_test` | `main/zlink.c` with its real link task on a virtual clock: the interview (complete answers only, a better endpoint only, answers matched to the endpoint asked), availability over an address change and a Leave, staggered setup after a return, queue headroom, a link taken down for a class change while its device is off (comes back as off, told once), an instance that moves to another VM (takes what is known of its device along) |
| `pair_test` | `main/pair.c` with its real task on a virtual clock: what happens to the Zigbee device when its virtual device goes. ETS unloads an address (removed after 5 s, asked to leave, reported), an address given again in time, a device that was switched off then and comes back, the same after a restart, a removed device paired again, devices that wait for an address, a full list. Also: a join the stack reports no announce for, a key exchange of 11.9 s and one that never ends, the network opened from elsewhere while a series or the window holds it open, an address assignment right after an unloading, a restart between unloading and removal, two assignments 3 s apart, a Device_Update "left" without a leave indication, a second join in one opening |
| `avail_test` | the availability rules of `main/avail.h` on a virtual clock: lamps that report, lamps without reporting, a lamp switched off at the wall, commands to a lamp that is off, a sleepy remote, counters that wrap |
| `link_embed` | a long extended frame whose payload contains a complete standard frame to one of our addresses |
| `link_fuzz ROUNDS SEED` | random octet streams and frames into the link layer |
| `vdev_cases` | the device server, cases A to G: segment memory and its budget, restore with a failing allocation, repetitions, when the record is saved, a refused status value, 15.15.255, the connection queue |
| `vdev_case_h` | saving without memory, the limit of the group address list |
| `vdev_sem` | 160 checks of what the device server answers (properties, load states and their error code in PID 28, memory, restart, a template part kept once per content, replacing a device, the gateway's record key, what the device info of ETS shows, an unloaded address reported to `pair.c`) |
| `vdev_fuzz STEPS SEED GARBAGE GSEED ALLOCFAIL% TXFAIL%` | random management traffic with restarts, damaged records, failing allocations and a refusing link; every restart is compared with the state before it |

Build and run (on the build host, output on a local disk, never into this tree):

    cd firmware/test/host
    for t in link_sim link_embed link_fuzz avail_test ep_pick_test zlink_test pair_test vdev_cases vdev_case_h vdev_sem vdev_fuzz; do
        gcc -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Wno-format \
            -I stubs -I ../../main -o "$OUT/$t" "$t.c"
    done
    "$OUT/avail_test" && "$OUT/ep_pick_test" && "$OUT/zlink_test" && "$OUT/pair_test" && "$OUT/vdev_sem" && "$OUT/vdev_cases" && "$OUT/vdev_fuzz" 150000 31 1 31 1 9

`avail_test`, `pair_test`, `vdev_sem` and `vdev_fuzz` end with a count of failures (`0 failed`, `invariant failures 0`, `reboots N (0 with a
difference)`); the case programs print what happened and are read, not asserted. A new source file in `main/` that
`vdev.c` or `knx_link.c` calls needs its stub in `stubs/` before these build again.
