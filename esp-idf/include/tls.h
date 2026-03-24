/**
 * tls — mbedTLS server wrapper for HTTPS, secure WS, etc.
 *
 * - EC P-256 self-signed cert, generated on first boot when any SSL port is
 *   non-zero and no cert matching <hostname>.local exists on /state/.
 * - Cert + private key stored as PEM files on /state/ (tls_cert / tls_key).
 * - Shared server SSL config (one cert, reused by all listening tasks).
 * - Per-connection tls_fd_t wraps mbedtls_ssl_context + raw fd.
 * - tlsRead/tlsWrite/tlsClose mirror recv/send/close semantics.
 */
#ifndef SECCAM_TLS_H
#define SECCAM_TLS_H

#include <stddef.h>
#include <stdbool.h>

/**
 * Opaque handle for a TLS-wrapped connection.
 * Callers use tls_fd_t* where they'd otherwise use an int fd.
 */
typedef struct tls_conn tls_conn_t;

/** Initialize TLS subsystem.  Call once from app_main after storageLoad + nvsRunBoot.
 *  Generates cert if needed (may take ~2 s for EC P-256). */
void tlsInit();

/** Check if the TLS subsystem is ready (cert loaded, config valid). */
bool tlsReady();

/** Accept a new TLS connection on a listening socket.
 *  Returns NULL if no connection pending or handshake failed.
 *  The underlying fd is set non-blocking before accept, then blocking
 *  for the handshake, then restored to the caller's preference.
 *  Caller must call tlsClose() to free. */
tls_conn_t* tlsAccept(int serverFd);

/** Get the raw fd from a TLS connection (for select/poll). */
int tlsFd(tls_conn_t* conn);

/** Read up to len bytes.  Returns bytes read, 0 on timeout/EAGAIN, -1 on error/close. */
int tlsRead(tls_conn_t* conn, void* buf, size_t len);

/** Write len bytes.  Returns bytes written or -1 on error. */
int tlsWrite(tls_conn_t* conn, const void* buf, size_t len);

/** Close TLS session and free resources.  Sets *conn to NULL. */
void tlsClose(tls_conn_t*& conn);

/** Returns number of bytes buffered in TLS layer (not yet read by app). */
size_t tlsBytesAvail(tls_conn_t* conn);

/** Force-regenerate the certificate (e.g. after hostname change).
 *  Call from CLI context — blocks for ~2s. */
void tlsRegenCert();

/** Reload cert + key from /state/ (after ACME renewal). */
void tlsReloadCert();

/** Get SHA-256 fingerprint of DER cert as "XX:XX:..." string (for SDP).
 *  Returns false if TLS not ready. */
bool tlsCertFingerprint(char* out, size_t outLen);

/** Access shared RNG, cert and key for DTLS config. */
struct mbedtls_ctr_drbg_context;
struct mbedtls_x509_crt;
struct mbedtls_pk_context;
mbedtls_ctr_drbg_context* tlsGetRng();
mbedtls_x509_crt*         tlsGetCert();
mbedtls_pk_context*        tlsGetKey();

#endif
