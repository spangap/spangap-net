# net — WiFi + TCP socket relay + event bus

`net` is one FreeRTOS task that owns the device's WiFi state and every TCP
server socket. It runs a STA/AP state machine, relays bytes between sockets and
[ITS](../../spangap/INTERNALS.md) handles for any task that registers a listen
port or asks for an outbound dial, and fans WiFi lifecycle events out to
subscribers over the `NET_EV_*` bus. It also hosts the `net` / `ping` / `wget`
CLI commands.

This is the operator guide; the task model, the relay internals, and the
pitfalls are in [net-internals.md](net-internals.md).

## What it does

WiFi comes up automatically when `net` is in the build — there is no init call
to make. On boot the task reads the master switch `s.net.wifi.enable`, then:

- **STA** — scans for a known network from `s.net.wifi.nets[]` and associates
  with the first match (strongest-RSSI handling is in the scan). DHCP by
  default, or a per-network static IP. A flaky association gets three attempts
  before falling back to AP.
- **AP** — if no known network is visible after two scans (or none is
  configured), the device brings up its own access point so it is
  always reachable. From AP it keeps re-scanning every `s.net.wifi.ap.retry`
  seconds (non-disruptively, via APSTA) and switches to STA when a known network
  appears. `s.net.wifi.ap.active_for` bounds this: `-1` never starts the AP,
  `0` keeps it up until a known network appears, and `N>0` (default 300) shuts
  the AP down after N seconds without link traffic, once per boot. Any TCP
  traffic restarts the idle timer, so an active browser session keeps the AP
  alive as long as it is used. After the window the device keeps rescanning
  for known networks every `s.net.wifi.ap.retry` seconds (radio off between
  passes) and reconnects as STA on a hit — only the AP is spent until the
  next reboot. A pocketed device stops beaconing; rebooting force-summons
  the AP. The spent window survives deep sleep, so cron wakes don't re-arm it.

`net` distinguishes **link up** (STA *or* AP — `NET_EV_UP`) from **upstream up**
(STA associated to a real network, internet reachable — `NET_EV_UPSTREAM_UP`).
Modules that need the internet (ntp, duckdns, wg, upnp, cloud HTTP clients)
listen for upstream, not link.

When the device reaches a real upstream it runs the boot script
`/state/net_up` (via `cliRunFile`) — net policy that every net device gets for
free.

## How other straddles use it

Three integration points, all over ITS — no `net` type leaks into a consumer.

**Subscribe to WiFi events.** A module registers a callback for any `NET_EV_*`:

```c
netRegister(NET_EV_UPSTREAM_UP, [](const char*) { /* internet is reachable */ });
```

The UP edges (`NET_EV_UP`, `NET_EV_UPSTREAM_UP`) are **level-replayed**: a
handler that registers *after* the link is already up fires immediately, on the
registering task. So UP handlers must be idempotent and must tolerate running off
the net task. `DOWN` / `CFG_CHANGED` / `POLL` are edge-only.

**Register an inbound TCP listen port.** A server task sends a `net_port_msg_t`
to `net` on `NET_PORT_REG_PORT`; net opens the socket, accepts connections, and
hands each one back as an ITS connection to the registering task on the ITS port
it named:

```c
net_port_msg_t reg = { .itsPort = MY_PORT, .tcpPort = 0, .tls = 0,
                       .tcpNoDelay = 1, .keepAlive = 0, .backlog = 4,
                       .nvsKey = "rtsp_port", .defaultPort = 554 };
itsSendAux("net", NET_PORT_REG_PORT, &reg, sizeof(reg), pdMS_TO_TICKS(500));
```

`tcpPort = 0` means net reads the live listen port from `s.net.<nvsKey>` — so a
service ships its port as a config key under `s.net.*` and net re-opens the
socket when that key changes. Set `tls = 1` and net does TLS termination on the
port (see [tls.md](tls.md)); the registering task always receives plain bytes.

**Dial outbound.** Connect to net on `NET_PORT_TCP_DIAL` with an ASCII
`"host:port"` payload (≤95 bytes). Net does the DNS lookup and connect on its own
task, and the accepted ITS handle *is* the TCP byte stream from byte zero, with
net relaying both ways. The RNS TCP interface uses this for its outbound peers.
Both directions are stream-mode (not packet-mode): TCP is a byte stream, so a
caller that needs framing layers it on top.

## Public C API

Declared in [include/net.h](../esp-idf/include/net.h):

| Call | Purpose |
|---|---|
| `netRegister(event, cb)` | Subscribe to a `NET_EV_*` event (never unregistered). |
| `netUp()` / `netDown(force)` | Bring WiFi up / down. `netUp()` is a no-op while `s.net.wifi.enable=0` (the radio stays off). `netDown(force=false)` waits for 30 s idle; `force=true` is immediate. |
| `netIsUp()` | True if STA-connected or AP is up. |
| `netIsStaConnected()` | True only when associated to an upstream as STA. |
| `netGetLocalIp(out, len)` | The STA IP, or `""` if not STA-connected. |
| `netActivity()` | Reset the graceful-shutdown idle timer. |
| `netTrafficIn/Out(bytes)` | Add to the traffic counters (for tasks with their own sockets, e.g. WebRTC UDP; net's own relayed TCP is counted automatically). |
| `netForceClose(itsHandle)` | RST a relayed TCP connection. |

`NET_EV_*` values, `net_port_msg_t`, and the `net_connect_t` connect payload
(`ws` / `tls` / `clientAddr`, sent on every inbound connection) are in the
header.

## Events

| Event | Fires when | Replay |
|---|---|---|
| `NET_EV_UP` | Link up — STA got an IP, or AP started. | Level-replayed to late subscribers. |
| `NET_EV_DOWN` | Link going down. | Edge-only. |
| `NET_EV_UPSTREAM_UP` | STA associated to a real network (internet reachable). AP-only does **not** fire it. | Level-replayed. |
| `NET_EV_UPSTREAM_DOWN` | STA leaving connected (disconnect / reconnect loop / drop to AP-only). | Edge-only. |
| `NET_EV_CFG_CHANGED` | A watched config key changed; `arg` is the key name. | Edge-only. |
| `NET_EV_POLL` | Periodic, ~10 ms while connected. | Edge-only. |

## Storage variables

`net` has no socket API for configuration — storage is the control surface.
Defaults are seeded into `s.net.*` on first boot.

### Settings (`s.net.*`)

| Key | Default | Meaning |
|---|---|---|
| `s.net.hostname` | `CONFIG_SPANGAP_FW_HOSTNAME` | Device hostname (DHCP hostname, mDNS name, TLS cert CN). |
| `s.net.http_port` | `80` | HTTP listen port (used by [spangap-web](../../spangap-web); key owned here). |
| `s.net.https_port` | `443` | HTTPS listen port; `0` disables TLS cert generation. |
| `s.net.rtsp_port` | `554` | RTSP listen port (used by [seccam](../../seccam)). |
| `s.net.webrtc_port` | `4433` | WebRTC/DTLS port (used by spangap-web). |
| `s.net.log_port` | `0` | Plain-TCP log stream port; `0` = off. |
| `s.net.cli_port` | `0` | Plain-TCP CLI port; `0` = off. |
| `s.net.mdns_enable` | `1` | mDNS master switch (seeded from the `settings:` block). |
| `s.net.mdns.http` | `80` | mDNS `_http._tcp` advertised port (literal or a config-key reference). |
| `s.net.mdns.https` | `443` | mDNS `_https._tcp` advertised port. |
| `s.net.dns.fqdn` | `""` | Public FQDN (set by [duckdns](../../duckdns) / [acme](../../acme); read by services that need the external name). |
| `s.net.wifi.enable` | `1` | Master radio switch. Setting `0` brings WiFi down live and survives reboot. |
| `s.net.wifi.ap.ssid` | `<hostname>_<MAC last 2 bytes>` | AP SSID — computed per-device on first boot so a fleet doesn't present identical APs (e.g. `reticulous_dcbc`). User-editable after. |
| `s.net.wifi.ap.pass` | `""` | AP password (`""` = open network). |
| `s.net.wifi.ap.ip` | `192.168.1.1` | AP gateway IP. |
| `s.net.wifi.ap.mask` | `255.255.255.0` | AP netmask. |
| `s.net.wifi.ap.retry` | `300` | Seconds between background re-scans while in AP mode. |
| `s.net.wifi.ap.active_for` | `300` | `-1`: never start the AP. `0`: AP stays up until a known network appears. `N>0`: AP shuts down after N seconds without link traffic (traffic restarts the timer), once per boot; known-network rescans continue every `ap.retry` seconds, only the AP is spent until reboot. (Replaced `ap.disable` in config v2.) |
| `s.net.wifi.nets` | `[]` | Array of known STA networks. |

Each entry in `s.net.wifi.nets[i]` has: `ssid`, `pass`, and the optional
static-IP / custom-MAC fields `ip`, `gw`, `mask`, `dns`, `mac` (all empty = DHCP,
default MAC).

### Runtime state & telemetry (ephemeral, written by net)

| Key | Meaning |
|---|---|
| `net.up` | `1` once STA upstream is reachable — the [rns](../../rns) boot barrier waits on this. |
| `net.want` | `1` if any STA network is configured (tells rns whether to expect `net.up`). |
| `wifi.mac` | STA MAC address. |
| `wifi.traffic_in` / `wifi.traffic_out` | Human-formatted byte counters. |
| `wifi.sta.state` | `off` / `connecting` / `connected`. |
| `wifi.sta.{ssid,rssi,channel,ip,router,netmask,dns,up}` | STA association detail. |
| `wifi.ap.state` | `off` / `active`. |
| `wifi.ap.{ssid,ip,netmask,up}` | AP detail when active. |
| `wifi.scanned` | JSON array of nearby networks (`{ssid,bssid,rssi,locked}`, strongest first), published while a scan is requested. One row per SSID — when several APs serve the same network only the loudest is listed; hidden (empty-SSID) APs are kept individually. |

### Command sentinels (write to trigger; net clears them)

| Key | Action |
|---|---|
| `wifi.scan` | `1` starts publishing `wifi.scanned` (re-scans every 20 s while set). |
| `wifi.connect` | `<idx>` joins the known network at that array index. |
| `wifi.disconnect` | `1` drops the current STA and returns to AP. |
| `wifi.cmd.add` | `"<ssid>\t<pass>"` adds (or updates) a known network and joins it. |
| `wifi.cmd.del` | `"<idx>"` removes a known network (array-correct shift). |

The browser WiFi panel rewrites `s.net.wifi.nets[]` directly; the on-device LCD
pane drives the `wifi.cmd.*` sentinels — both surfaces converge on the same
stored networks.

## CLI

```
net                       WiFi status (state, SSID/IP/DNS, AP detail, traffic)
net up | down | down!     bring WiFi up / down (graceful) / down immediately
net list                  list stored networks (* marks the connected one)
net add <ssid> [pass]     save a network (quote spaces) and join if not on STA
net join <ssid>           force-join a known network
net delete <ssid>         remove a network (and disconnect if it was current)

ping [ip] [count]         ICMP echo (default target = router, count = 4)

wget <url>                download an http(s) URL to its basename in the cwd
wget -O <file> <url>      download to a specific file
```

Run any of these on-device through `spangap cli "<command>"`.

### wget

`wget` fetches an `http(s)://` URL with ESP-IDF's `esp_http_client`, validating
TLS against the IDF certificate bundle and following up to 10 redirects. With no
`-O`, it saves to the URL's basename (or `index.html`) in the CLI's current
directory; `-O <file>` writes to a specific path. The transfer runs on a
dedicated PSRAM-stack worker (a TLS handshake needs more stack than the CLI
task has) and the command blocks until it completes — bounded by a 30 s client
timeout — then reports bytes saved or the failure. A non-2xx response or a write
error removes the partial file.

## Browser & on-device UI

`net` ships its own front ends — it is not firmware-only:

- **Browser:** `modules/net.ts` registers the **Settings → Internet → WiFi**
  panel (`NetworkPanel.vue` + `WifiScanDialog.vue`): enable toggle, live STA/AP
  status, a draggable list of known networks with per-network DHCP/static-IP and
  custom-MAC editing, a scan dialog, and AP configuration. Wired in
  automatically via the `browser_register: registerNet` hook when spangap-web
  stages the SPA shell.
- **On-device (LVGL):** when [spangap-lcd](../../spangap-lcd) is staged,
  `net_lcd.cpp` registers **Settings → Internet → WiFi** — enable + live status,
  a join/delete list of saved networks, a scan picker, and SSID/password entry,
  so a browserless device can still get onto a network. Per-network static-IP /
  custom-MAC editing stays browser-only.
- The **mDNS** and **System** (hostname / timezone / NTP server) panes are
  generated from the straddle's declarative `settings:` block, not hand-written.

## Read next

- [net-internals.md](net-internals.md) — the task model, the socket relay,
  the WiFi state machine, and pitfalls.
- [tls.md](tls.md) / [ntp.md](ntp.md) / [mdns.md](mdns.md) — the other functions
  hosted by this straddle.
