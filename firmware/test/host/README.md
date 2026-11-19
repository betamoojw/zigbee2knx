# Host tests for the KNX side of the firmware

These programs compile the real `main/knx_link.c` and `main/vdev.c` for the PC (`#include` of the source file, stubs for
FreeRTOS, NVS, UART, timer and the Berry/Zigbee side in `stubs/`) and run them against a virtual clock; `vdev_sem` adds
the real `main/gw.c` and `main/console.c`. They need no hardware. They do not replace a run on the bus: the NCN5130 timing
model in `link_sim.c` is written from the datasheet, and nothing here knows what ETS does.

The device server's task runs as far as one thread can have it: whenever its lock comes free, what waits in its two
queues is taken through the task's own loop body (`drain()`), as the task's higher priority makes it happen on the
stick; the timer part runs where a test lets time pass. NVS can be modelled by entries (`g_nvs_cap`): a blob takes its
data entries, a header per chunk and its index, one page stays free, and a blob that does not fit is refused.

| program | what it runs |
|---|---|
| `link_sim` | the link layer against a model of the NCN5130: host acknowledge in time, our own frames, extended frames (taken, sent with the index offset under both readings of datasheet p. 46, LTE format dropped), a stalled task, a full send queue (cases S1 to S12, printed); frames of ours the link gives up and reports back (S13, 5 checks) |
| `ep_pick_test` | the endpoint rule of `main/ep_pick.h` (full interview): Green Power and foreign profiles left out, actuator before remote, made-up descriptors |
| `zlink_test` | `main/zlink.c` with its real link task on a virtual clock: the interview (complete answers only, a better endpoint only, answers matched to the endpoint asked), availability over an address change and a Leave, staggered setup after a return, queue headroom, a link taken down for a class change while its device is off (comes back as off, told once), an instance that moves to another VM (takes what is known of its device along); model names read as UTF-8 the way Zigbee2MQTT reads them, the battery percentage as the device reports it and SWBuildID with the model, the first state of every device after a start told on the bus one device after the other, the parameters again when a device answers again; the task watchdog fed with every event and with every device of an availability round, a ZCL command the stack does not take (sent once more after 200 ms, then counted as a failed send, two of them make the device unreachable; nothing counted while Zigbee is not up) |
| `pair_test` | `main/pair.c` with its real task on a virtual clock: what happens to the Zigbee device when its virtual device goes. ETS unloads an address (removed after 5 s, asked to leave, reported), an address given again in time, a device that was switched off then and comes back, the same after a restart, a removed device paired again, devices that wait for an address, a full list. Also: a join the stack reports no announce for, a key exchange of 11.9 s and one that never ends, the network opened from elsewhere while a series or the window holds it open, an address assignment right after an unloading, a restart between unloading and removal, two assignments 3 s apart, a Device_Update "left" without a leave indication, a second join in one opening; two sources that ask at once, a sleepy device that waits, reports from a member that was asked to leave, one of our devices that joins anew after a reset; binding from ETS before the gateway's own application is loaded (nothing opens, the pairing window does), a device that joins while nothing holds the network open (asked to leave, the network closed once more), the console's "join" and "join 0"; the task watchdog fed in every round |
| `tplmem_test` | the memory rule for loading a template (`main/tplmem.h`): the case the gateway refused after an update in the ETS on 2026-11-19 (loads now), the four templates given out against what their load took on a PC, which part of the rule decides, the edges, the VM a device gives up counted by the check before a download completes |
| `kgtjson_test` | the behaviour taken out of a template's payload (`main/kgtjson.h`): only the top-level key, strings with braces and quotes, blanks, broken payloads; unescaped in place as Berry's `json.load()` does it, its edges included |
| `avail_test` | the availability rules of `main/avail.h` on a virtual clock: lamps that report, lamps without reporting, a lamp switched off at the wall, commands to a lamp that is off, a sleepy remote, counters that wrap |
| `link_embed` | a long extended frame whose payload contains a complete standard frame to one of our addresses |
| `link_fuzz ROUNDS SEED` | random octet streams and frames into the link layer, frames it gives up included |
| `vdev_cases` | the device server, cases A to G: segment memory and its budget, restore with a failing allocation, repetitions, when the record is saved, a refused status value, 15.15.255, the connection queue |
| `vdev_case_h` | saving without memory, the limit of the group address list |
| `vdev_sem` | 297 checks of what the device server and the gateway's own device answer: properties, load states and their error code in PID 28, memory (the limits of PID 56 for reading and writing), restart and master reset, priorities, a template part kept once per content and deleted with its last record, room in the flash, failed writes and their message, the gateway's parameters (LongFrames, the range check), its fault and message objects, the two queues at a central command and at the start, a template not checked again while it runs, a stopped class's values, unloading, frames the link gave up, serial numbers, duplicate addresses, the update flag, the group address limit, the console's number checks and that nothing is printed under the lock; binding from ETS told to begin by the gateway's download and its record, the held status values of a device on the bus once after a start and after its restart (one device every 300 ms), the task watchdog fed for every item the device server takes; a template load that runs out of memory tried three times, a check that finds too little memory tried again without a limit |
| `vdev_fuzz STEPS SEED GARBAGE GSEED ALLOCFAIL% TXFAIL%` | random management traffic with restarts, damaged records, failing allocations and a refusing link; every restart is compared with the state before it |

Build and run (on the build host, output on a local disk, never into this tree):

    cd firmware/test/host
    for t in link_sim link_embed link_fuzz avail_test tplmem_test kgtjson_test ep_pick_test zlink_test pair_test vdev_cases vdev_case_h vdev_sem vdev_fuzz; do
        gcc -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Wno-format \
            -I stubs -I ../../main -o "$OUT/$t" "$t.c"
    done
    "$OUT/avail_test" && "$OUT/tplmem_test" && "$OUT/kgtjson_test" && "$OUT/ep_pick_test" && "$OUT/zlink_test" && "$OUT/pair_test" && "$OUT/vdev_sem" && "$OUT/link_sim" && "$OUT/vdev_cases" && "$OUT/vdev_fuzz" 150000 31 1 31 1 9
    "$OUT/vdev_case_h"; "$OUT/link_embed"; "$OUT/link_fuzz" 500 1

`avail_test`, `tplmem_test`, `kgtjson_test`, `pair_test`, `vdev_sem`, `link_sim` and `vdev_fuzz` end with a count of failures (`0 failed`, `invariant failures 0`,
`reboots N (0 with a difference)`) and exit non-zero on one; the case programs print what happened and are read, not asserted. A
new source file in `main/` that `vdev.c`, `gw.c`, `console.c` or `knx_link.c` calls needs its stub in `stubs/` before these build
again. The task watchdog of `main/main.c` (`wdt_watch`, `wdt_feed`) is a pair of counters in each harness that needs it
(`vdev_fuzz.c`, `zlink_test.c`, `pair_test.c`).
