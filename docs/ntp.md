# ntp — time sync + timezone

`ntp` keeps the system clock set and the timezone applied. It runs `esp_sntp`
non-blocking against a configured server while the internet is reachable, applies
the user's timezone from an on-disk IANA→POSIX map, yields to a local time
authority (GPS) when one claims the clock, and accepts a browser-pushed time when
NTP is unavailable. It hosts the `date` CLI.

This is the operator guide; the engine reconcile model and the timezone resolver
are in [ntp-internals.md](ntp-internals.md).

## What it does

Time sync starts automatically when `net` is in the build — there is no init
call. `ntp` subscribes to net's events:

- On **upstream up**, it starts `esp_sntp` (poll mode) against `s.ntp.server`.
- On **upstream down**, it stops the engine.
- On every net **poll**, it reconciles whether the engine *should* be running —
  it runs iff upstream is up **and** no local time authority has inhibited it.

When time first becomes valid (≥ 2025-01-01), `ntp` publishes the ephemeral
`sys.time.valid = 1` so subscribers — e.g. the LCD status-bar clock, the rns boot
barrier — react without polling. A background SNTP poll that re-sets the clock
also flips it.

**Timezone.** `s.ntp.tz` holds an IANA name (e.g. `America/Argentina/Buenos_Aires`).
`ntp` resolves it to a POSIX `TZ` string and applies it, so log timestamps and
`localtime` show local time. The resolution comes from the on-disk timezone DB
at `<stateDir>/timezones.json` (a ~15 KB map kept out of the config tree / RAM,
parsed only on a timezone change), and the resolved string is cached in
`s.ntp.posix` to skip the parse on later boots. On a fresh device, before the
browser sends a timezone, `TZ` is unset and times are **UTC** — there is no
default-timezone guess.

## How others integrate

- **A local clock source (GPS)** parks NTP by writing `sys.time.ext = 1` on the
  storage bus, and releases it with `0`. Driving it through storage means the
  time source needs no compile-time dependency on net — a net-less image just has
  no subscriber. The C entry point `ntpInhibit(bool)` exists for direct callers;
  it's thread-safe (the net task reconciles on its next poll).
- **The browser** pushes the user's IANA timezone via `s.ntp.tz` on first
  connect (the device does the POSIX lookup itself), and can set the wall clock
  with `sys.time.set = <epoch>` when NTP can't be reached.
- `ntpApplyTimezone()` is callable as soon as storage has loaded, for code that
  wants local time early in boot.

## Storage variables

### Settings (`s.ntp.*`)

| Key | Default | Meaning |
|---|---|---|
| `s.ntp.server` | `pool.ntp.org` | NTP server hostname (DNS must be reachable). |
| `s.ntp.tz` | `""` | IANA timezone name. Empty → UTC. |
| `s.ntp.posix` | `""` | Cached POSIX `TZ` string resolved from `s.ntp.tz`; cleared automatically when `s.ntp.tz` changes. |
| `s.ntp.zones_etag` | `""` | ETag of the on-disk timezone DB (the browser refreshes the file when GitHub's copy is newer). |

The hostname / timezone / NTP-server fields surface in the generated **System**
settings pane (the web leaf keeps a searchable timezone picker).

### Runtime (ephemeral)

| Key | Meaning |
|---|---|
| `sys.time.valid` | `1` once the clock is past 2025-01-01. |

### Command sentinels (read, self-clearing)

| Key | Action |
|---|---|
| `sys.time.set` | `<epoch>` sets the clock if it isn't already valid (then cleared). |
| `sys.time.ext` | `1` inhibits SNTP (a local clock owns time), `0` releases it. |

## CLI

```
date                      show current local date/time
date <yyyymmddhhmmss>     set date/time manually (e.g. date 20260315143000)
date wait [timeout_secs]  block until time is valid (default 60 s)
```

Run on-device through `spangap cli "<command>"`.

## Read next

- [ntp-internals.md](ntp-internals.md) — the engine reconcile, the timezone
  resolver, and pitfalls.
