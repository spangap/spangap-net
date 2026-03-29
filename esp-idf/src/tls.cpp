/**
 * tls — mbedTLS server: EC P-256 self-signed cert, /state/ storage, per-connection wrappers.
 */
#include "tls.h"
#include "storage.h"
#include "log.h"
#include "cli.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "compat.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <lwip/sockets.h>
#include <fcntl.h>


#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/pk.h"
#include "mbedtls/ecp.h"
#include "mbedtls/x509.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"
#include "mbedtls/sha256.h"
#include "mbedtls/bignum.h"
#include "esp_heap_caps.h"

/* ---- Per-connection handle ---- */

struct tls_conn {
    mbedtls_ssl_context ssl;
    int fd;
};

/* ---- Shared server state ---- */

static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context ctr_drbg;
static mbedtls_x509_crt srvcert;
static mbedtls_pk_context pkey;
static mbedtls_ssl_config conf;
static bool ready = false;

/* ---- State file helpers (certs stored on /state/ LittleFS partition) ---- */

static std::string statePath(const char* key) {
    return std::string("/state/") + key + ".pem";
}

static bool stateWrite(const char* key, const uint8_t* data, size_t len) {
    auto path = statePath(key);
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return false;
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    return written == len;
}

static bool stateRead(const char* key, std::string& out) {
    auto path = statePath(key);
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }
    out.resize((size_t)sz);
    fread(out.data(), 1, (size_t)sz, f);
    fclose(f);
    return true;
}

/* ---- Config hostname for cert CN ---- */

static std::string nvsHostnameLocal() {
    char hostname[32];
    storageGetStr("s.net.hostname", hostname, sizeof(hostname), "seccam");
    return std::string(hostname) + ".local";
}

/* ---- Check if stored cert matches current hostname ---- */

static bool certMatchesHostname() {
    std::string certPem;
    if (!stateRead("tls_cert", certPem)) return false;

    mbedtls_x509_crt tmp;
    mbedtls_x509_crt_init(&tmp);
    int ret = mbedtls_x509_crt_parse(&tmp, (const uint8_t*)certPem.c_str(), certPem.size());
    if (ret != 0) { mbedtls_x509_crt_free(&tmp); return false; }

    std::string cn = nvsHostnameLocal();

    /* Check CN matches */
    char buf[256];
    mbedtls_x509_dn_gets(buf, sizeof(buf), &tmp.subject);
    std::string subject(buf);
    bool cnOk = subject.find("CN=" + cn) != std::string::npos;

    /* Check SAN exists (certs without SAN are rejected by browsers) */
    bool sanOk = tmp.subject_alt_names.buf.len > 0;

    mbedtls_x509_crt_free(&tmp);
    return cnOk && sanOk;
}

/* ---- Generate EC P-256 self-signed cert ---- */

static bool generateCert() {
    info("generating EC P-256 self-signed cert...\n");
    uint32_t startMs = millis();

    int ret;
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;

    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);

    /* Generate EC key */
    ret = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (ret != 0) { err("pk_setup: -0x%04x\n", -ret); goto fail; }

    ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,
                               mbedtls_pk_ec(key),
                               mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret != 0) { err("ecp_gen_key: -0x%04x\n", -ret); goto fail; }

    /* Build self-signed cert */
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    { uint8_t serial_buf[] = {0x01};
      mbedtls_x509write_crt_set_serial_raw(&crt, serial_buf, sizeof(serial_buf)); }
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);

    {
        std::string cn = "CN=" + nvsHostnameLocal();
        ret = mbedtls_x509write_crt_set_issuer_name(&crt, cn.c_str());
        if (ret != 0) { err("set_issuer: -0x%04x\n", -ret); goto fail; }
        ret = mbedtls_x509write_crt_set_subject_name(&crt, cn.c_str());
        if (ret != 0) { err("set_subject: -0x%04x\n", -ret); goto fail; }
    }

    /* Validity: 2025-01-01 to 2035-12-31 */
    ret = mbedtls_x509write_crt_set_validity(&crt, "20250101000000", "20351231235959");
    if (ret != 0) { err("set_validity: -0x%04x\n", -ret); goto fail; }

    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);

    /* Basic constraints: CA=false */
    ret = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);
    if (ret != 0) { err("basic_constraints: -0x%04x\n", -ret); goto fail; }

    /* Subject Alternative Name — required by browsers (CN alone rejected since 2017) */
    {
        std::string dnsName = nvsHostnameLocal();
        mbedtls_x509_san_list san = {};
        san.node.type = MBEDTLS_X509_SAN_DNS_NAME;
        san.node.san.unstructured_name.p = (unsigned char*)dnsName.c_str();
        san.node.san.unstructured_name.len = dnsName.size();
        san.next = nullptr;
        ret = mbedtls_x509write_crt_set_subject_alternative_name(&crt, &san);
        if (ret != 0) { err("set_san: -0x%04x\n", -ret); goto fail; }
    }

    /* Key Usage: digitalSignature + keyAgreement (required for ECDSA TLS) */
    ret = mbedtls_x509write_crt_set_key_usage(&crt,
        MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_KEY_AGREEMENT);
    if (ret != 0) { err("set_key_usage: -0x%04x\n", -ret); goto fail; }

    /* Write cert to PEM */
    {
        uint8_t buf[2048];
        ret = mbedtls_x509write_crt_pem(&crt, buf, sizeof(buf),
                                         mbedtls_ctr_drbg_random, &ctr_drbg);
        if (ret != 0) { err("crt_pem: -0x%04x\n", -ret); goto fail; }
        size_t certLen = strlen((char*)buf) + 1;  /* include NUL for PEM parsing */
        if (!stateWrite("tls_cert", buf, certLen)) {
            err("write tls_cert failed\n");
            goto fail;
        }

        /* Write private key to PEM */
        ret = mbedtls_pk_write_key_pem(&key, buf, sizeof(buf));
        if (ret != 0) { err("pk_pem: -0x%04x\n", -ret); goto fail; }
        size_t keyLen = strlen((char*)buf) + 1;
        if (!stateWrite("tls_key", buf, keyLen)) {
            err("write tls_key failed\n");
            goto fail;
        }
    }

    info("cert generated in %ums\n", (unsigned)(millis() - startMs));
    mbedtls_pk_free(&key);
    mbedtls_x509write_crt_free(&crt);
    return true;

fail:
    mbedtls_pk_free(&key);
    mbedtls_x509write_crt_free(&crt);
    return false;
}

/* ---- Load cert + key from /state/, init SSL config ---- */

static bool loadAndConfigure() {
    std::string certPem, keyPem;
    if (!stateRead("tls_cert", certPem)) { err("no tls_cert\n"); return false; }
    if (!stateRead("tls_key", keyPem))   { err("no tls_key\n"); return false; }

    int ret;
    ret = mbedtls_x509_crt_parse(&srvcert, (const uint8_t*)certPem.c_str(), certPem.size());
    if (ret != 0) { err("crt_parse: -0x%04x\n", -ret); return false; }

    ret = mbedtls_pk_parse_key(&pkey, (const uint8_t*)keyPem.c_str(), keyPem.size(),
                                nullptr, 0, mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret != 0) { err("pk_parse: -0x%04x\n", -ret); return false; }

    ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) { err("ssl_config: -0x%04x\n", -ret); return false; }

    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
    mbedtls_ssl_conf_ca_chain(&conf, &srvcert, nullptr);
    ret = mbedtls_ssl_conf_own_cert(&conf, &srvcert, &pkey);
    if (ret != 0) { err("conf_own_cert: -0x%04x\n", -ret); return false; }

    /* Authmode: none (self-signed, no client certs) */
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);

    /* ChaCha20-Poly1305 only — faster in software on Xtensa than AES-GCM,
     * and avoids the ESP32-S3 hardware GCM DMA bug entirely.
     * All modern browsers support this cipher suite. */
    static const int ciphersuites[] = {
        MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
        0
    };
    mbedtls_ssl_conf_ciphersuites(&conf, ciphersuites);

    return true;
}

/* ---- Public API ---- */

static void tlsRegenCert();

static bool anySslPort() {
    return storageGetInt("s.net.https_port", 443) > 0;
}

void tlsInit() {
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_x509_crt_init(&srvcert);
    mbedtls_pk_init(&pkey);
    mbedtls_ssl_config_init(&conf);

    int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                     (const uint8_t*)"seccam", 6);
    if (ret != 0) {
        err("ctr_drbg_seed: -0x%04x\n", -ret);
        return;
    }

    if (!anySslPort()) {
        info("no SSL ports configured, skipping cert\n");
        return;
    }

    /* Generate cert if missing or hostname mismatch (but keep ACME certs) */
    if (!certMatchesHostname()) {
        /* Check if existing cert is from a real CA (ACME) — don't overwrite */
        std::string existingCert;
        bool isAcmeCert = false;
        if (stateRead("tls_cert", existingCert)) {
            mbedtls_x509_crt tmp;
            mbedtls_x509_crt_init(&tmp);
            if (mbedtls_x509_crt_parse(&tmp, (const uint8_t*)existingCert.c_str(),
                                        existingCert.size()) == 0) {
                isAcmeCert = (tmp.issuer_raw.len != tmp.subject_raw.len ||
                              memcmp(tmp.issuer_raw.p, tmp.subject_raw.p,
                                     tmp.issuer_raw.len) != 0);
            }
            mbedtls_x509_crt_free(&tmp);
        }
        if (!isAcmeCert) {
            if (!generateCert()) {
                err("cert generation failed\n");
                return;
            }
        }
    }

    if (loadAndConfigure()) {
        ready = true;
        info("TLS ready\n");
    }

    /* cert CLI commands */
    cliRegisterCmd("cert self-signed", [](const char* a) {
        if (strcmp(a, "help") == 0) { cliPrintf("  %-*s generate self-signed cert (if none exists)\n", CLI_HELP_COL, "cert self-signed"); return; }
        std::string existing;
        if (stateRead("tls_cert", existing)) { cliPrintf("  certificate already exists\n"); return; }
        tlsRegenCert();
    });
    cliRegisterCmd("cert delete", [](const char* a) {
        if (strcmp(a, "help") == 0) { cliPrintf("  %-*s delete TLS certificate\n", CLI_HELP_COL, "cert delete"); return; }
        remove("/state/tls_cert.pem");
        remove("/state/tls_key.pem");
        if (ready) {
            mbedtls_x509_crt_free(&srvcert);
            mbedtls_pk_free(&pkey);
            mbedtls_ssl_config_free(&conf);
            mbedtls_x509_crt_init(&srvcert);
            mbedtls_pk_init(&pkey);
            mbedtls_ssl_config_init(&conf);
            ready = false;
        }
        cliPrintf("  certificate deleted\n");
    });
    cliRegisterCmd("cert", [](const char* a) {
        if (strcmp(a, "help") == 0) {
            cliPrintf("  %-*s show certificate info\n", CLI_HELP_COL, "cert");
            cliPrintf("  %-*s generate self-signed cert\n", CLI_HELP_COL, "cert self-signed");
            cliPrintf("  %-*s delete certificate\n", CLI_HELP_COL, "cert delete");
            cliPrintf("  %-*s get/renew ACME cert\n", CLI_HELP_COL, "cert acme [days]");
            return;
        }
        std::string certPem;
        if (!stateRead("tls_cert", certPem)) { cliPrintf("  no TLS certificate\n"); return; }
        mbedtls_x509_crt crt;
        mbedtls_x509_crt_init(&crt);
        if (mbedtls_x509_crt_parse(&crt, (const uint8_t*)certPem.c_str(), certPem.size()) != 0) {
            cliPrintf("  certificate parse error\n");
            mbedtls_x509_crt_free(&crt);
            return;
        }
        char buf[256];
        mbedtls_x509_dn_gets(buf, sizeof(buf), &crt.issuer);
        cliPrintf("  issuer:  %s\n", buf);
        mbedtls_x509_dn_gets(buf, sizeof(buf), &crt.subject);
        cliPrintf("  subject: %s\n", buf);
        cliPrintf("  expires: %04d-%02d-%02d\n",
                  crt.valid_to.year, crt.valid_to.mon, crt.valid_to.day);
        time_t now = time(nullptr);
        if (now > 1735689600) {
            struct tm expiry = {};
            expiry.tm_year = crt.valid_to.year - 1900;
            expiry.tm_mon = crt.valid_to.mon - 1;
            expiry.tm_mday = crt.valid_to.day;
            time_t expiryTime = mktime(&expiry);
            cliPrintf("  days left: %d\n", (int)((expiryTime - now) / 86400));
        }
        bool selfSigned = (crt.issuer_raw.len == crt.subject_raw.len &&
                           memcmp(crt.issuer_raw.p, crt.subject_raw.p, crt.issuer_raw.len) == 0);
        cliPrintf("  type: %s\n", selfSigned ? "self-signed" : "CA-signed");
        mbedtls_x509_crt_free(&crt);
    });
}

static volatile bool handshakeInProgress = false;

bool tlsReady() { return ready && !handshakeInProgress; }

tls_conn_t* tlsAccept(int serverFd) {
    if (!ready || serverFd < 0) return nullptr;

    /* Non-blocking accept */
    fd_set fds;
    struct timeval tv = {0, 0};
    FD_ZERO(&fds);
    FD_SET(serverFd, &fds);
    if (select(serverFd + 1, &fds, nullptr, nullptr, &tv) <= 0) return nullptr;

    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int fd = accept(serverFd, (struct sockaddr*)&addr, &len);
    if (fd < 0) return nullptr;
    handshakeInProgress = true;

    /* TCP_NODELAY for TLS handshake performance */
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    /* Blocking for handshake */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    /* Allocate connection in PSRAM */
    auto* conn = (tls_conn_t*)heap_caps_calloc(1, sizeof(tls_conn_t), MALLOC_CAP_SPIRAM);
    if (!conn) { close(fd); handshakeInProgress = false; return nullptr; }
    conn->fd = fd;

    mbedtls_ssl_init(&conn->ssl);
    int ret = mbedtls_ssl_setup(&conn->ssl, &conf);
    if (ret != 0) {
        err("%sssl_setup: -0x%04x\n", cfd(fd), -ret);
        mbedtls_ssl_free(&conn->ssl);
        heap_caps_free(conn);
        close(fd);
        handshakeInProgress = false;
        return nullptr;
    }

    mbedtls_ssl_set_bio(&conn->ssl, &conn->fd,
                         [](void* ctx, const unsigned char* buf, size_t len) -> int {
                             int fd = *(int*)ctx;
                             int n = send(fd, buf, len, 0);
                             if (n < 0) {
                                 if (errno == EAGAIN || errno == EWOULDBLOCK)
                                     return MBEDTLS_ERR_SSL_WANT_WRITE;
                                 return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
                             }
                             return n;
                         },
                         [](void* ctx, unsigned char* buf, size_t len) -> int {
                             int fd = *(int*)ctx;
                             int n = recv(fd, buf, len, 0);
                             if (n < 0) {
                                 if (errno == EAGAIN || errno == EWOULDBLOCK)
                                     return MBEDTLS_ERR_SSL_WANT_READ;
                                 return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
                             }
                             if (n == 0) return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
                             return n;
                         },
                         nullptr);

    /* Short handshake timeout — don't block the task on stale/broken clients */
    struct timeval hs_tv = {3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &hs_tv, sizeof(hs_tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &hs_tv, sizeof(hs_tv));

    /* Perform handshake */
    while ((ret = mbedtls_ssl_handshake(&conn->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            char errbuf[128];
            mbedtls_strerror(ret, errbuf, sizeof(errbuf));
            dbg("%sTLS handshake: -0x%04x %s\n", cfd(fd), -ret, errbuf);
            mbedtls_ssl_free(&conn->ssl);
            heap_caps_free(conn);
            close(fd);
            handshakeInProgress = false;
            return nullptr;
        }
    }

    /* Clear handshake timeouts — socket stays blocking.
       Callers that need non-blocking (web HTTP) set it themselves. */
    struct timeval no_tv = {0, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &no_tv, sizeof(no_tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &no_tv, sizeof(no_tv));

    handshakeInProgress = false;
    return conn;
}

int tlsFd(tls_conn_t* conn) {
    return conn ? conn->fd : -1;
}

int tlsRead(tls_conn_t* conn, void* buf, size_t len) {
    if (!conn) return -1;
    int ret = mbedtls_ssl_read(&conn->ssl, (unsigned char*)buf, len);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        return 0;
    if (ret < 0 || ret == 0) {
        dbg("tlsRead: %d (-0x%04x) len=%d\n", ret, ret < 0 ? -ret : 0, (int)len);
        return -1;
    }
    return ret;
}

int tlsWrite(tls_conn_t* conn, const void* buf, size_t len) {
    if (!conn) return -1;
    int ret = mbedtls_ssl_write(&conn->ssl, (const unsigned char*)buf, len);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        return 0;
    if (ret < 0) {
        dbg("tlsWrite: -0x%04x len=%d\n", -ret, (int)len);
        return -1;
    }
    return ret;
}

size_t tlsBytesAvail(tls_conn_t* conn) {
    if (!conn) return 0;
    return mbedtls_ssl_get_bytes_avail(&conn->ssl);
}

void tlsClose(tls_conn_t*& conn) {
    if (!conn) return;
    mbedtls_ssl_close_notify(&conn->ssl);
    mbedtls_ssl_free(&conn->ssl);
    if (conn->fd >= 0) close(conn->fd);
    heap_caps_free(conn);
    conn = nullptr;
}

bool tlsCertFingerprint(char* out, size_t outLen) {
    if (!ready || outLen < 96) return false;  /* "sha-256 XX:XX:...\0" = 95 chars */
    uint8_t hash[32];
    mbedtls_sha256(srvcert.raw.p, srvcert.raw.len, hash, 0);
    int pos = snprintf(out, outLen, "sha-256 ");
    for (int i = 0; i < 32; i++)
        pos += snprintf(out + pos, outLen - pos, "%s%02X", i ? ":" : "", hash[i]);
    return true;
}

mbedtls_ctr_drbg_context* tlsGetRng()  { return &ctr_drbg; }
mbedtls_x509_crt*         tlsGetCert() { return &srvcert; }
mbedtls_pk_context*        tlsGetKey()  { return &pkey; }

static void tlsRegenTask(void* arg) {
    info("regenerating TLS cert...\n");

    /* Free old state */
    if (ready) {
        mbedtls_x509_crt_free(&srvcert);
        mbedtls_pk_free(&pkey);
        mbedtls_ssl_config_free(&conf);
        mbedtls_x509_crt_init(&srvcert);
        mbedtls_pk_init(&pkey);
        mbedtls_ssl_config_init(&conf);
        ready = false;
    }

    if (generateCert() && loadAndConfigure()) {
        ready = true;
        info("TLS cert regenerated\n");
    } else {
        err("TLS cert regeneration failed\n");
    }

    SemaphoreHandle_t sem = (SemaphoreHandle_t)arg;
    if (sem) xSemaphoreGive(sem);
    vTaskDelete(nullptr);
}

void tlsReloadCert() {
    if (ready) {
        mbedtls_x509_crt_free(&srvcert);
        mbedtls_pk_free(&pkey);
        mbedtls_ssl_config_free(&conf);
        mbedtls_x509_crt_init(&srvcert);
        mbedtls_pk_init(&pkey);
        mbedtls_ssl_config_init(&conf);
        ready = false;
    }
    if (loadAndConfigure()) {
        ready = true;
        info("TLS cert reloaded\n");
    } else {
        err("TLS cert reload failed\n");
    }
}

static void tlsRegenCert() {
    /* EC key gen needs ~10KB stack — run on a temporary task */
    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCoreWithCaps(tlsRegenTask, "tls_gen", 16384, sem, 1, nullptr, 0, MALLOC_CAP_DEFAULT);
    xSemaphoreTake(sem, portMAX_DELAY);
    vSemaphoreDelete(sem);
}
