# tls — the shared TLS server

`tls` is the device's mbedTLS server: one shared SSL config with an EC P-256
certificate, used by every task that terminates TLS. It generates a self-signed
cert on first boot, accepts an ACME-issued cert when [acme](../../acme) lands
one, and hot-reloads after renewal without a reboot. It hosts the `cert` CLI.

This is the operator guide; the cert lifecycle, the cipher choice, and the
per-connection internals are in [tls-internals.md](tls-internals.md).

## What it does

TLS starts automatically when `net` is in the build. On boot, if any SSL port is
configured (`s.net.https_port > 0`) and no cert matching `<hostname>.local`
exists on the `/state` partition, it generates an EC P-256 keypair and a
self-signed X.509 cert (~2 s, on a temporary 16 KB task because EC keygen needs
the stack). The cert carries a SAN and Key Usage extensions — both required by
modern browsers — and is stored as PEM on `/state`, so it survives a reflash and
is erased on factory reset.

`tls` does not open sockets itself. The [net](net.md) task does TLS termination
for any listen port registered with `tls = 1` in its `net_port_msg_t`: net calls
`tlsAccept()` to run the handshake, then relays plain bytes to the owning task
over ITS. So HTTPS, secure WebSocket, and any other TLS endpoint share this one
cert and config.

## How others use it

A task gets a TLS-terminated port simply by setting `tls = 1` when it registers
with net — it never touches mbedTLS. The C wrappers in
[include/tls.h](../esp-idf/include/tls.h) exist for net's relay and for code that
needs the raw connection or the cert material:

| Call | Purpose |
|---|---|
| `tlsReady()` | Cert loaded and config valid (and no handshake in progress). |
| `tlsAccept(serverFd)` | Run the server handshake on a pending connection; returns an opaque `tls_conn_t*`. |
| `tlsRead` / `tlsWrite` / `tlsClose` / `tlsBytesAvail` / `tlsFd` | Per-connection wrappers mirroring `recv`/`send`/`close`. |
| `tlsReloadCert()` | Re-load cert+key from `/state` after an ACME renewal. |
| `tlsCertFingerprint(out, len)` | SHA-256 fingerprint of the DER cert as `sha-256 XX:XX:…` (for WebRTC SDP). |
| `tlsGetRng` / `tlsGetCert` / `tlsGetKey` | Shared RNG / cert / key, for the DTLS (WebRTC) config. |

## Storage

`tls` defines no config keys of its own. It reads `s.net.hostname` (owned by
[net](net.md)) for the cert CN/SAN (`<hostname>.local`) and the CTR_DRBG
personalization string, and `s.net.https_port` to decide whether to generate a
cert at all. The cert and key are PEM **files** on `/state`
(`tls_cert.pem`, `tls_key.pem`), not storage keys.

## CLI

```
cert                show cert info (issuer, subject, expiry, days left, self-signed vs CA)
cert self-signed    generate a self-signed cert if none exists
cert delete         remove the cert + key (and tear down the loaded config)
```

The `acme renew` command is registered separately by [acme](../../acme), not
here.

## Read next

- [tls-internals.md](tls-internals.md) — cert generation/reload, the
  ChaCha20-Poly1305 choice, the per-connection handle, and pitfalls.
