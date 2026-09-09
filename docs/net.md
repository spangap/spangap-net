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
  appears. Two settings bound this. `s.net.wifi.ap.enable` at `0` never starts
  the AP at all. `s.net.wifi.ap.timeout` is then how long it lives, in minutes:
  `0` keeps it up until a known network appears, and `N>0` (default 10) shuts
  the AP down after N minutes without link traffic, once per boot. Any TCP
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

**Publish a port to the internet.** `publicFacing = 1` in the same message says
this listener is meant to be reachable from outside the LAN — a flag, not a port
number. Net keeps the flag with the endpoint and reports every *open*
public-facing port through `netPublicPorts()`; the [upnp](../../upnp) straddle
reads that list and asks the router to forward each one in at the same external
port, and `NET_EV_PORTS_CHANGED` tells it when the list has moved. Net itself
opens no holes and needs no port mapper to be in the build — the flag is a
statement of intent that a mapper may or may not be there to act on. Re-send the
registration to change it: net keys endpoints by `nvsKey`, so a re-send updates
the one already there rather than adding another.

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
| `netMulticastRxAcquire()` / `netMulticastRxRelease()` | Refcounted hold: while held, WiFi modem power-save stays at `WIFI_PS_MIN_MODEM` (wake for every DTIM beacon — when the AP transmits buffered multicast) instead of the default `WIFI_PS_MAX_MODEM`, which sleeps through most multicast. Hold while a service depends on receiving multicast; safe from any task, applied immediately and on every later bring-up. |
| `netTrafficIn/Out(bytes)` | Add to the traffic counters (for tasks with their own sockets, e.g. WebRTC UDP; net's own relayed TCP is counted automatically). |
| `netForceClose(itsHandle)` | RST a relayed TCP connection. |
| `netPublicPorts(out, max)` | Fill `out[]` with the open listen ports whose registrant set `publicFacing`, and return how many. What a port mapper forwards; safe from any task. |

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
| `NET_EV_PORTS_CHANGED` | A public-facing listen socket opened or closed, or a registrant flipped `publicFacing`. Fires from the endpoint-open pass — the net task, or whoever wrote an `s.net.*` key — so a handler defers its work instead of doing it inline. | Edge-only. |

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
| `s.net.mdns.<service>` | — | One advertised service per entry, seeded by whoever serves it, never by net. The value is a literal port or the name of the key holding the live one, resolved on every advertise — spangap-web seeds `http`/`https` as `s.net.http_port` / `s.net.https_port`, so the advertisement is whatever the web server is actually configured for. |
| `s.net.dns.fqdn` | `""` | Public FQDN (set by [duckdns](../../duckdns) / [acme](../../acme); read by services that need the external name). |
| `s.net.wifi.enable` | `1` | Master radio switch. Setting `0` brings WiFi down live and survives reboot. |
| `s.net.wifi.ap.ssid` | `<hostname>_<MAC last 2 bytes>` | AP SSID — computed per-device on first boot so a fleet doesn't present identical APs (e.g. `reticulous_dcbc`). User-editable after. |
| `s.net.wifi.ap.pass` | `""` | AP password (`""` = open network). |
| `s.net.wifi.ap.ip` | `192.168.1.1` | AP gateway IP. |
| `s.net.wifi.ap.mask` | `255.255.255.0` | AP netmask. |
| `s.net.wifi.ap.retry` | `300` | Seconds between background re-scans while in AP mode. |
| `s.net.wifi.ap.enable` | `1` | `0`: never start the AP. Cleared while the AP is live (the settings toggle) it drops immediately; the window is not spent, so re-enabling can start it again. |
| `s.net.wifi.ap.timeout` | `10` | Minutes of link idleness before the AP is spent. `0`: AP stays up until a known network appears. `N>0`: AP shuts down after N minutes without link traffic (traffic restarts the timer), once per boot; known-network rescans continue every `ap.retry` seconds, only the AP is spent until reboot. |
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
| `wifi.sta.ip6` / `wifi.sta.ip6_ll` | Best non-link-local IPv6 address (global scope preferred over unique-local) and the link-local, RFC 5952 text. Empty until SLAAC / duplicate-address detection delivers one; the settings rows hide on empty. IPv6 has no settings: the addresses are always automatic (link-local + SLAAC), so unlike v4 there is no static configuration to offer. |
| `wifi.ap.state` | `off` / `active`. |
| `wifi.ap.{ssid,ip,netmask,up}` | AP detail when active. |
| `wifi.scanned` | JSON array of nearby networks (`{ssid,bssid,rssi,locked}`, strongest first), rewritten by **every** scan whatever asked for it — the connect search, the browser beat, `net scan` — so it is always the radio's most recent look around. One row per SSID — when several APs serve the same network only the loudest is listed; hidden (empty-SSID) APs are kept individually. |

### Command keys (write to trigger; net clears them)

| Key | Action |
|---|---|
| `wifi.scan` | `1` re-scans every 20 s while set, keeping `wifi.scanned` fresh. **The re-scan runs while associated too**, and a scan takes the one radio off its channel — so a UI that arms this key owns clearing it, and one left set is a join/rejoin cycle for as long as it stands. `wifi.scanned` is published by every scan regardless of this key; what the key buys is a scan on a beat rather than only when something else needed one. |
| `wifi.connect` | `<idx>` joins the known network at that array index. Like every command key here it is **cleared by net as it takes the command**, and a clear arrives at subscribers as an empty value — so a reader of this key must ignore an empty one rather than let `atoi` turn it into index 0. |
| `wifi.disconnect` | `1` drops the current STA and returns to AP. |
| `wifi.cmd.add` | `"<ssid>\t<pass>"` adds (or updates) a known network and joins it. |
| `wifi.cmd.del` | `"<idx>"` removes a known network (array-correct shift). |

The browser WiFi panel rewrites `s.net.wifi.nets[]` directly; the on-device LCD
pane drives the `wifi.cmd.*` command keys — both surfaces converge on the same
stored networks.

**A factory-reset boot keeps the radio down.** `netInit` leaves `rtcWantUp`
false when `spangapSafeMode()` is `SAFE_MODE_FACTORY_RESET`: that boot exists to
erase the store and restart, and joining a network, standing the AP up or
answering for a hostname all describe a device that is about to stop existing.
The stack still comes up — the console, the log and the socket relay ride it.
Backup and restore keep the radio: those modes are *reached* over the network.
See [safe-mode.md](../../spangap-core/docs/safe-mode.md).

## CLI

```
net                       WiFi status (state, SSID/IP/DNS, AP detail, traffic)
net up | down | down!     bring WiFi up / down (graceful) / down immediately
net list                  list stored networks (* marks the connected one)
net scan                  scan now, then list every AP seen this boot
net add <ssid> [pass]     save a network (quote spaces) and join if not on STA
net join <ssid>           force-join a known network
net delete <ssid>         remove a network (and disconnect if it was current)

net -O                    onboarding output: state / ssid / ip / hostname
net scan -O               onboarding output: count, then one ap= per network

ping [ip] [count]         ICMP echo (default target = router, count = 4)

wget <url>                download an http(s) URL to its basename in the cwd
wget -O <file> <url>      download to a specific file
```

Run any of these on-device through `spangap cli "<command>"`.

### One scan, one record of it

Every scan the radio runs goes through `wifiScanRun()` — the connect search, the
browser's 20-second beat, `net scan` — because a scan is one look at the
neighbourhood and there is only ever one of those. It feeds the cache, publishes
`wifi.scanned` and answers the connect search's question, in that order, so no
two views of what the radio just heard can disagree.

The cache holds one record per SSID and accumulates across every scan this boot,
keeping the loudest RSSI a network has ever shown — so the picture fills in over
time rather than being whatever the most recent single scan happened to hear.
Hidden SSIDs are skipped, and it is reset only by a reboot. The same cache is
what limits `scan found …` to one log line per network per boot.

### `net scan`

Scans now, then prints the cache. The scan runs on the net task — the radio is
its, and two tasks must not both be driving a scan — so the CLI sends the
request and waits for the generation counter to move, printing a dot a second.
Wherever the radio is when the request lands: down brings it up for the scan and
puts it straight back; on this node's own AP it goes APSTA so clients keep their
connection. A known network turning up in the results is *not* acted on — asking
what is in earshot is not asking to be moved onto it.

**Ctrl-C abandons the wait, not the scan.** The scan is the net task's and
finishes either way, so its results are in the cache a moment later regardless.

`net scan -O` does not scan. It is the onboarding contract, read over the framed
channel by a caller holding a two-second timeout that a full sweep of the band
does not fit inside, and it answers from the cache — free, instant, and as
current as the last scan by anyone.

### Waiting for a connect

`net add` (when it joins) and `net join` print `connecting` with a dot a second
until the attempt reaches a conclusion — associated, fallen back to this node's
own AP, or radio off — and then print exactly what `net` prints. Ctrl-C stops the
waiting; the connect attempt is the net task's and runs to its own end either
way, which is what makes the abort free to take.

### `-O`, onboarding output

`net -O` and `net scan -O` print machine-readable `key=value` lines and nothing
else — the contract a flasher provisions against. See
[onboarding-output.md](../../spangap-core/docs/onboarding-output.md) for the
format and its rules; the keys are:

```
net -O          state=ap|sta|connecting|down
                ssid=…            (when there is one)
                ip=…              (when there is one)
                hostname=…

net scan -O     count=<n>
                ap=<rssi> <open|closed> <ssid>      × n
```

`count` is emitted first and computed after dropping any SSID that isn't
representable on one line, so it always equals the number of `ap=` lines that
follow. `ap=` puts the SSID last, so a space in it needs no quoting.

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

`net` ships its own settings — it is not firmware-only:

- **WiFi, mDNS and System** are all described by the `settings:` block in
  `straddle.yaml`, which the build lowers to the browser and to the display. The
  same enable toggle, live status, known-network list (drag to reorder, with
  per-network DHCP/static-IP and custom-MAC editing), scan-and-adopt picker and
  access-point configuration appear on both — a browserless device can do
  everything a browser can.
- Nothing about them is hand-written on either side. The known networks are a
  collection whose every mutation goes through a `wifi.net.*` command key, and net
  publishes the state, the signal quality and the scan rows as finished text —
  see [net-internals](net-internals.md#8-front-ends).

## Read next

- [net-internals.md](net-internals.md) — the task model, the socket relay,
  the WiFi state machine, and pitfalls.
- [tls.md](tls.md) / [ntp.md](ntp.md) / [mdns.md](mdns.md) — the other functions
  hosted by this straddle.
