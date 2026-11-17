# knxgw-berry — protocol between the device server and the template VM

The device templates run in Berry, one VM per template with `prelude.be` loaded first, and the device server
talks to such a VM in JSON lines. In the firmware (`firmware/main/bvm.c`) the VM gets one request line per call;
on a PC, `knxgw-berry` runs the prelude's `_main()` loop over stdin and stdout, and the template tests (`tests/`)
run every template that way. Hooks and host functions have the same names in both.

```
knxgw-berry PRELUDE.be TEMPLATE.be      # berry/prelude.be + templates/<name>/<file>.be
knxgw-berry --version
```

One process serves every instance of one template. stdin and stdout carry one JSON object per line;
stderr carries Berry's `print()` and tracebacks and is not part of the protocol. The VM is the firmware's
(Berry sources and `berry_conf.h` from `firmware/components/berry`, built by `build_hostvm.sh`).

## Start

The host prints `{"op":"ready","berry":"1.1.0"}` once the files are loaded. A file that fails to load ends
the process with exit code 2 and the traceback on stderr.

## Requests (device server → VM)

Every request carries `call`, `inst` (instance number) and `id` (echoed in `done`).

| call | fields | hook it runs |
|---|---|---|
| `knx_write` | `ko`, `raw` (hex) | `knx_write(ko, raw)`, raw as `bytes` |
| `dp_event` | `ch`, `key`, `value`, `initial` | `dp_event(ch, key, value, initial)`; the value is stored first, so `host.dp_get` sees it |
| `params` | `p` (hex, parameter segment) | `params(p)` |
| `start` | – | `start()` |
| `dp_seed` | `ch`, `key`, `value` | none: puts a last value back after a restart of the VM |
| `forget` | – | none: drops the instance's datapoints and state |

A missing hook is skipped. The manifest's `datapoints.events` names the datapoints a template expects as
events; the firmware passes on what `firmware/main/zlink.c` maps from Zigbee and does not check that list.

## Answers (VM → device server)

A request produces zero or more operations and then exactly one `done`:

```
{"op":"dp_set","inst":1,"ch":1,"key":"state","value":"ON","vt":"string"}
{"op":"done","id":7,"ok":true,"err":null}
```

| op | fields | device server does |
|---|---|---|
| `dp_set` | `ch`, `key`, `value`, `vt` | to the instance's link (in the firmware a ZCL command to the Zigbee device, `firmware/main/zlink.c`); `ch.key` belongs in `datapoints.write` |
| `master_put` | `ch`, `values`, `vt` (per key) | configuration values of one channel, for keys in `datapoints.master`; the firmware does not act on it (the Zigbee classes declare none) |
| `ko_send` | `ko`, `raw` | sends the object; the object needs the transmit flag |
| `ko_status` | `ko`, `raw`, `initial` | status object: sent on change, `initial` only fills the shadow; needs the transmit or read flag |
| `log` | `kind`, `fields` | a log record; the firmware writes it to its console |

`vt` is the Berry type of the value (`bool`, `int`, `real`, `string`). JSON alone loses it: `json.dump`
writes the real 5.0 as `5`. Other value types (lists, maps, nil) make the hook fail with `value_error`.

A hook that raises answers `done` with `ok:false` and the error text; operations it sent before the error
are carried out. The firmware stops a call that runs longer than 2 s, and the call is lost. Template state
in `host.state_*` is lost with the VM (v0 does not keep it).

## host.* inside the VM

| function | does |
|---|---|
| `host.dp_set(ch, key, value)` | write a VALUES datapoint |
| `host.dp_get(ch, key)` | last value of a datapoint for this instance, `nil` if none came |
| `host.master_put(ch, {key: value})` | MASTER values of one channel |
| `host.ko_send(ko, raw)` | send an object now |
| `host.ko_status(ko, raw, initial)` | status object, sent on change |
| `host.state_get(key)` / `host.state_set(key, value)` | per-instance state (not persistent in v0) |
| `host.log(kind, {fields})` | log record |
| `host.now_ms()` | monotonic milliseconds |
