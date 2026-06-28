# spangap-net

The IP-networking half of the [spangap](../spangap) platform: WiFi, TCP/UDP,
TLS, NTP, and mDNS. Any straddle that needs an IP stack — [spangap-web](../spangap-web),
[acme](../acme), [duckdns](../duckdns), [upnp](../upnp), [wg](../wg),
[sshd](../sshd), [ota](../ota), and the TCP / AutoInterface RNS interfaces —
pulls this in. A node that talks only over LoRa or ESP-NOW skips it entirely and
saves the lwIP code plus the WiFi blob's RAM footprint.

`spangap-net` is in the dispatcher's platform band, so the IP stack is up before
any service or app straddle runs; nothing needs to order itself against it. It
has three sides: the firmware (the `net` task and the TLS/NTP/mDNS helpers it
hosts), a browser WiFi panel, and an on-device LVGL WiFi pane — a device with no
browser can still join a network.

## Functions

| Function | Operator guide | What it is |
|---|---|---|
| **net** | [docs/net.md](docs/net.md) | WiFi STA/AP state machine, the TCP socket relay (inbound listen ports + outbound dial), the `NET_EV_*` event bus, and the `wget` download CLI. |
| **tls** | [docs/tls.md](docs/tls.md) | The shared mbedTLS server — EC P-256 cert (self-signed or ACME), ChaCha20-Poly1305, hot reload, the `cert` CLI. |
| **ntp** | [docs/ntp.md](docs/ntp.md) | Non-blocking time sync via `esp_sntp`, the IANA→POSIX timezone resolver, GPS inhibit, the `date` CLI. |
| **mdns** | [docs/mdns.md](docs/mdns.md) | `<hostname>.local` advertisement of the device's services via the ESP-IDF `mdns` component. |

Each function's operator guide links to its `*-internals.md` maintainer
reference.

## Bundled remote-access services

Because every remote-access add-on hard-requires the IP stack, pulling in `net`
stages them by default through `additional_installs`: [acme](../acme),
[upnp](../upnp), [wg](../wg), [duckdns](../duckdns), and [sshd](../sshd). Each
carries its own init hook and browser registration, so dropping one with
`spangap build --without spangap/<name>` compiles and un-bundles its UI cleanly.

## Owned ports

`net` owns the config keys for the platform's well-known TCP ports — other
straddles read them and register listeners, but the keys live here: `http_port`
(80), `https_port` (443), `rtsp_port` (554), `webrtc_port` (4433), and the
off-by-default `log_port` / `cli_port`. See [docs/net.md](docs/net.md) for the
full storage surface.

## What it does NOT own

- HTTPS server / REST / WebDAV / WebSocket — [spangap-web](../spangap-web)
  (it registers an HTTPS listener with `net`; the `https_port` key is net's).
- WireGuard tunnel — [wg](../wg). ACME / DuckDNS / UPnP — [acme](../acme) /
  [duckdns](../duckdns) / [upnp](../upnp). Firmware update — [ota](../ota).
- Reticulum / LoRa / ESP-NOW — those are the [rns](../rns) interfaces.

## Dependencies

- [spangap-core](../spangap-core) — base runtime (ITS, storage, log, CLI, fs,
  pm). `spangap-net` transitively requires it; there is no spangap build without
  core.
