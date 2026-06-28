# mdns — `<hostname>.local` service advertisement

`mdns` advertises the device on the local link as `<hostname>.local` and
publishes its TCP services over multicast DNS, so a browser or client can reach
the device by name without knowing its IP. It wraps the ESP-IDF `mdns` component
and is driven entirely by storage.

This is the operator guide; the responder-enable detail and the init-order trap
are in [mdns-internals.md](mdns-internals.md).

## What it does

mDNS starts automatically when `net` is in the build. It registers for net's
link events: on **link up** (STA or AP) it initialises the responder, sets the
hostname and instance name from `s.net.hostname`, and advertises every entry in
the `s.net.mdns.*` config tree; on **link down** it frees the responder. Because
it re-advertises on every up edge, the published ports follow the services'
actual configured ports rather than a stale snapshot.

Each key `s.net.mdns.<name> = <port>` is advertised as the service
`_<name>._tcp`. The value is either a literal port (`"443"`) or the **name of the
config key** that holds the live port (`"s.net.https_port"`) — the reference form
is resolved at advertise time, so the advertisement tracks the real port. A
resolved port `≤ 0` advertises nothing; an empty tree means no services.

`net` owns only the mechanism and the `s.net.mdns_enable` master switch. The
service entries belong to whoever serves them — [spangap-web](../../spangap-web)
seeds the http/https entries, [sshd](../../sshd) seeds ssh — each in its own
init.

## Storage variables

| Key | Default | Meaning |
|---|---|---|
| `s.net.mdns_enable` | `1` | Master switch (seeded from the straddle's `settings:` block). `0` → no advertisement at all. |
| `s.net.hostname` | (net's default) | Hostname / instance name; `<hostname>.local` is the advertised name. Owned by [net](net.md). |
| `s.net.mdns.<name>` | — | One advertised `_<name>._tcp` service; value is a literal port or a config-key reference. |

The built-in entries `s.net.mdns.http` (80) and `s.net.mdns.https` (443) are
seeded by net's own defaults; the mDNS enable/port pane is generated from the
straddle's declarative `settings:` block (no hand-written panel).

## Read next

- [mdns-internals.md](mdns-internals.md) — the AP-responder enable, the
  init-order trap, and pitfalls.
