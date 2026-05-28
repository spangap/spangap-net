# License

This repository, **spangap-net** (IP networking + TLS + NTP + mDNS for spangap
device apps), is released under the **Apache License, Version 2.0**.

Full license text: <https://www.apache.org/licenses/LICENSE-2.0>

Copyright (c) 2026 by spangap project contributors.

## Third-party software

### Vendored in this repository

None.

### Build-time dependencies

Declared in `esp-idf/idf_component.yml`:

| Component | Source | License |
|---|---|---|
| ESP-IDF (platform) | espressif/esp-idf | Apache-2.0 |
| `espressif/mdns`   | components.espressif.com | Apache-2.0 |

TLS, lwIP and other libraries linked transitively via ESP-IDF retain their
upstream licenses.
