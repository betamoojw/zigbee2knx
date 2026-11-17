# Templates

A device class is a template in `templates/<class>/`:

- `manifest.json` (schema `knxgw-template/0`): the KNX application (group objects with DPTs and flags, parameters
  and their place in the parameter segment, application number and version, catalog and hardware), the Zigbee side
  (radio, datapoints, the models of the class in `products`) and the name of the Berry file.
- `<class>.be`: the behaviour, in [Berry](https://github.com/berry-lang/berry). It reacts to group telegrams, to
  values from the Zigbee device and to downloaded parameters, and it acts through the host API.

The host API (the `host.*` functions and the hooks a template provides) is described in
[berry/PROTOCOL.md](../berry/PROTOCOL.md). `berry/prelude.be` implements it; the firmware runs the same prelude in
every VM, one VM per template with every device of the class inside it, and so does the host VM of the tests.

`distill/z2m_to_knx.py` builds the classes from the device definitions of Zigbee2MQTT (exported by
`distill/dump_z2m.cjs`): it sorts each model into a class or refuses it with a reason, and writes the model table
into the template. The tests in `tests/` run each class in the host VM ([building-firmware.md](building-firmware.md#tests)).

## The embedded template (KGT2)

A template travels inside the parameter segment of its own application (load state machine 4), behind the
application's parameters, as base64 text in hidden text parameters. A trailer `KGTE…` at the end of the segment says
where the text lies. The blob:

    "KGT2" | length of the compressed payload (u32, big endian) | first 8 bytes of its SHA-256 |
    key fingerprint (8) | signature r||s (64) | zlib-compressed payload

The payload is JSON with the runtime part of the manifest and the Berry source without its comment lines. The
signature is ECDSA P-256 with SHA-256 over the uncompressed payload; the key fingerprint is the first 8 bytes of the
SHA-256 over the uncompressed public point. `tools/kgt.py` packs and checks the blob, `firmware/main/kgt.c` checks it
in the gateway.

The gateway loads an embedded template only if the signature is valid and the key is one of those compiled into
`firmware/main/kgt.c` (`TRUST`): Berry is no sandbox, and the signature is what keeps foreign code out. A template is
stored once per content (its digest is the SHA-256 of the payload) and runs in one VM for all devices of the class.

## Templates of your own

Firmware images of this project trust the project's key only. To run templates of your own:

1. Make a P-256 key:

       openssl ecparam -name prime256v1 -genkey -noout -out my-template-key.pem

2. Add its public point (65 bytes, uncompressed, starting with 0x04) to `TRUST` in `firmware/main/kgt.c`, and for
   reference to `tools/kgt_trust.json`; then build and flash the firmware. The point as a C array:

       openssl ec -in my-template-key.pem -pubout -outform DER | tail -c 65 | xxd -i

3. Give the application a number of your own, outside 0xC000–0xC3FF which this project uses, and generate the
   product file with `--sign-key my-template-key.pem` or `ZIGBEE2KNX_SIGN_KEY`
   ([building-knxprod.md](building-knxprod.md#a-new-application-version)).

Keep the private key out of any repository.
