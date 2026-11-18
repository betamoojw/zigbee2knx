# Security

Please report a vulnerability privately: in this repository, Security → "Report a vulnerability". Do not open a public
issue for it.

Of particular interest:

- a way to make a gateway run a program (Berry) that is not signed with one of the keys in
  [`tools/kgt_trust.json`](tools/kgt_trust.json), through a download, the bus or the Zigbee network;
- frames on the KNX bus or in the Zigbee network that crash or hang the gateway;
- the firmware images, the product files attached to the releases and the web installer.

Fixes go into the next release.
