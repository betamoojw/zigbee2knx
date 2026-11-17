# Building the product files

The files in `producer/` are inputs for [OpenKNXproducer](https://github.com/OpenKNX/OpenKNXproducer): one file per
class, with the class's application, its catalog products and the embedded, signed template. OpenKNXproducer turns
each file into a `.knxprod` and has it signed by the signing library of an installed ETS, so the build runs on a
Windows PC with ETS.

Tested with OpenKNXproducer 4.3.12 and ETS 6.4.1: run `OpenKNXproducer create <name>` in a folder with the XML files,
once per file (for example `OpenKNXproducer create zigbee-switch` for `zigbee-switch.xml`), and import the resulting
`.knxprod` files into ETS.

| File | Application | Version | Catalog section |
|---|---|---|---|
| `zigbee-gateway.xml` | 0xC000 Zigbee gateway | 3 | Zigbee2KNX / Gateway |
| `zigbee-dimmable-light.xml` | 0xC001 dimmable light | 3 | Zigbee2KNX / Lights |
| `zigbee-white-spectrum-light.xml` | 0xC002 white spectrum light | 4 | Zigbee2KNX / Lights |
| `zigbee-switch.xml` | 0xC003 switch and plug | 3 | Zigbee2KNX / Plugs and switches |
| `zigbee-remote.xml` | 0xC004 remote | 2 | Zigbee2KNX / Remotes |

The applications use the manufacturer ID M-00FA (KNX Association), as open-source KNX projects do. They are not
registered with the KNX Association.

## The application ID

ETS identifies an application by an ID that the signing library computes from its content. Built from the same XML,
an application gets the same ID: two builds with the signing library of ETS 6.4.1 (OpenKNXproducer on Windows and an
independent build) gave the same IDs and the same files. So `.knxprod` files built from the same XML on different PCs
can be used side by side.

Any change inside an application gives it another ID, even a new signature over the same template. ETS then sees
another application, and devices loaded with the old one have to be replaced. A released file is therefore never
generated again; a change becomes a new application version.

## Adding products

Another model of an existing class (another lamp that is a "dimmable light", a plug under a new name) needs only a
catalog product, not a new application:

    python3 distill/knxprod_products.py --append producer/zigbee-switch.xml --products my-products.json -o my-switch.xml

`my-products.json` is a list such as

    [{"vendor": "Example", "model": "PLUG-1", "description": "smart plug", "zigbeeModel": ["Example plug"]}]

(`zigbeeModel` may be left out). Nothing is signed, and the application stays byte for byte, so its ID stays. ETS
6.4.1 accepted such a file with one more product next to the original import; a device inserted from an added
product has not been loaded yet.

A model the class's template does not list runs with the class defaults, and the gateway reports `MODEL` for it.

## A new application version

`distill/knxprod_products.py templates/<class>` runs the whole chain for a class: `tools/knxprod_gen.py` writes the
application with the template embedded and signed, then the products of the manifest are added. The gateway's file
comes from `producer/gateway_build.py`; its application embeds no template and is not signed.

The template is signed with a key the firmware trusts, either from a file (`--sign-key PEM`, or
`ZIGBEE2KNX_SIGN_KEY`) or through an external signer such as a hardware key (`--sign-cmd CMD` with the signer's public
key as `--sign-pub HEX` or, by fingerprint from `tools/kgt_trust.json`, `--sign-fpr FPR`). The command gets the
SHA-256 digest of the payload as hex on stdin and prints the signature r||s as hex; every signature is checked
before it is used. See [templates.md](templates.md) for working with your own key.

`producer/issued.json` lists every application version that has been given out (imported into an ETS installation).
Before such a run, raise `knx.application.version` in the manifest: ReplacesVersions is taken from that list, and the
generators refuse to write an issued version with other content, or a version that changes the meaning of an object
number or parameter id of an issued one. After a new version has been imported, `tools/knxprod_gen.py --issued-record
XML APP_ID` prints its entry for the list. `tests/test_issued.py` checks the producer files against the list.
