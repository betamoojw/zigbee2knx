# Notices

zigbee2knx — Copyright (c) 2026 Dirk Tostmann

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
License version 3 as published by the Free Software Foundation (SPDX: `GPL-3.0-only`). It is distributed in the hope
that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
A PARTICULAR PURPOSE. See [LICENSE](LICENSE) for the full text.

## Additional permission under GNU GPL version 3 section 7

If you modify this Program, or any covered work, by linking or combining it with libraries that Espressif Systems
distributes in object code form only, as part of ESP-IDF or of the esp-zigbee-lib component (or modified versions of
those libraries), the licensors of this Program grant you additional permission to convey the resulting work.
Corresponding Source for a non-source form of such a combination shall include the source code for the parts of those
libraries that are available in source form, as well as that of the covered work.

## Third-party components

| Component | Where | License |
|---|---|---|
| [Berry](https://github.com/berry-lang/berry) 1.1.0 | `firmware/components/berry/`, with an ESP-IDF port and an allocation counter added | MIT, Copyright (c) 2018-2020 Guan Wenliang, some files also Stephan Hadinger (see their headers); text in `firmware/components/berry/LICENSE` |
| [zigbee-herdsman-converters](https://github.com/Koenkk/zigbee-herdsman-converters) 26.57.0 | `distill/data/ikea-26.57.0.jsonl` (device definitions as the library outputs them); the model tables in `templates/` are derived from them | MIT, Copyright (c) 2018 Koen Kanters; text below |
| [ESP-IDF](https://github.com/espressif/esp-idf) 6.0 | build dependency, not part of this repository | Apache-2.0; some libraries in object code form |
| [esp-zigbee-lib](https://components.espressif.com/components/espressif/esp-zigbee-lib) 2.0.1 | fetched by the ESP-IDF component manager at build time | Apache-2.0; libraries in object code form |
| [ESP Web Tools](https://github.com/esphome/esp-web-tools) 10.5.2 | fetched by `.github/workflows/pages.yml` (checksum pinned) and served with the web installer page, not part of this repository | Apache-2.0; text served next to it |

Firmware images built from this repository contain Berry, ESP-IDF and esp-zigbee-lib.

### zigbee-herdsman-converters

```
MIT License

Copyright (c) 2018 Koen Kanters

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
