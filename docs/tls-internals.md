# tls — internals

Maintainer reference for the TLS server. The [operator guide](tls.md) covers the
public surface. Source: [src/tls.cpp](../esp-idf/src/tls.cpp),
[include/tls.h](../esp-idf/include/tls.h).

## 1. What this function provides

A thin layer over mbedTLS:

- **One shared server config** (`mbedtls_ssl_config`) plus the EC P-256 cert and
  key, an entropy source, and a CTR_DRBG seeded per-device.
- **Self-signed cert generation** with browser-mandatory extensions (SAN, Key
  Usage, basic constraints CA=false).
- **PEM-on-`/state` persistence** rather than storage keys, so a ~1.2 KB cert
  pair doesn't sit in `cfgRoot`/RAM.
- **ACME-cert preservation** and **hot reload** for renewal.
- **Per-connection wrappers** (`tls_conn_t`) that net's relay uses
  interchangeably with raw fds.
- The **`cert` CLI** and the **DER fingerprint** helper for WebRTC SDP.

## 2. Shared state and placement

`entropy` and `srvcert` are `PSRAM_BSS` (large, read-mostly); `ctr_drbg`,
`pkey`, and `conf` are ordinary BSS. `ready` gates every accept. The
personalization string for `mbedtls_ctr_drbg_seed` is pulled from
`s.net.hostname` so RNG output is domain-separated per device.

Per connection, `tls_conn_t` (`mbedtls_ssl_context` + `fd`) is allocated in
**PSRAM** (`heap_caps_calloc(MALLOC_CAP_SPIRAM)`), since a TLS session context is
large and there can be several concurrent.

## 3. Cert lifecycle

`tlsInit()`:

1. Inits mbedTLS objects and seeds CTR_DRBG. If `anySslPort()` is false
   (`s.net.https_port <= 0`) it stops — no cert on a TLS-less build.
2. If `certMatchesHostname()` fails (no cert, or CN/SAN doesn't match
   `<hostname>.local`), it checks whether the existing cert is **CA-signed**
   (issuer ≠ subject → an ACME cert) and, if so, leaves it alone; otherwise it
   regenerates the self-signed cert. This is what lets an ACME cert survive a
   hostname change.
3. `loadAndConfigure()` parses cert+key, builds the server config, and sets
   `ready`.

`generateCert()` writes a v3 cert valid 2025-01-01…2035-12-31 with serial `0x01`,
SHA-256 signature, SAN = `<hostname>.local`, and KeyUsage
`digitalSignature | keyAgreement` (required for an ECDSA TLS cert). It runs
inline during init but `cert self-signed` / regeneration runs it on a temporary
**16 KB task** (`tlsRegenTask`) because EC keygen needs the stack; the caller
blocks on a semaphore.

`tlsReloadCert()` (called by acme after renewal) tears down the loaded config and
re-runs `loadAndConfigure()` from the new `/state` PEMs — no regeneration, no
reboot, and in-flight sessions keep their own context.

## 4. The handshake (`tlsAccept`)

`tlsAccept` is non-blocking on the listen fd (zero-timeout `select` before
`accept`), then sets the accepted fd **blocking** for the handshake with a 3 s
RCV/SND timeout so a stale client can't park the net task, runs
`mbedtls_ssl_handshake` to completion, then clears the timeouts (callers that
want non-blocking, e.g. web's HTTP loop, set it themselves). The BIO callbacks
map `EAGAIN/EWOULDBLOCK` to `WANT_READ/WANT_WRITE`, `recv == 0` to
`PEER_CLOSE_NOTIFY`, and anything else to `INTERNAL_ERROR`.

`handshakeInProgress` makes `tlsReady()` return false during a handshake, so
net refuses a *second* incoming TLS connection (with an RST) rather than
interleaving two handshakes on the one shared config.

## 5. Pitfalls

- **ChaCha20-Poly1305 only — do not re-enable AES-GCM.** The config forces the
  single ciphersuite `ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256`, and the build
  sets `CONFIG_MBEDTLS_HARDWARE_AES=n`. The ESP32-S3's hardware GCM DMA driver
  has a state-machine bug (espressif/esp-idf#12689) that corrupts payloads /
  yields intermittent `-1` errors under sustained TLS write load. Software
  ChaCha20 at 240 MHz handles streaming fine (~8 ms overhead) and every modern
  browser supports it.
- **A cert without a SAN is rejected by browsers** (CN-only has been invalid
  since 2017). `certMatchesHostname()` checks both CN and SAN presence; keep both
  in `generateCert()`.
- **Self-signed vs CA detection is a raw issuer/subject compare.** ACME-cert
  preservation hinges on `issuer_raw != subject_raw`; don't replace it with a
  string compare that could false-match.
- **`tls` opens no sockets.** All accept/relay lives in net's `netPollOnce()`;
  `tls` only runs the handshake and the byte wrappers. Keep socket policy
  (keepalive, NODELAY, RST-on-not-ready) in net.
