/**
 * Net — WiFi state machine + TCP call center + event dispatch.
 *
 * WiFi: scan/connect/AP state machine (wifi1–wifi9 STA, wifi0 AP fallback).
 * TCP:  owns all server sockets, proxies bytes between sockets and ITS.
 * Events: modules register callbacks via netRegister() for UP/DOWN/CFG/POLL.
 */
#include "net.h"
#include "mem.h"
#include "storage.h"
#include "compat.h"
#include "pm.h"
#include "log.h"
#include "cli.h"
#include "fs.h"
#include "its.h"
#include "tls.h"
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <fcntl.h>
#include "esp_wifi.h"
#include "esp_sleep.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "lwip/ip4_addr.h"
#include <cstring>
#include <cstdio>
#include <string>
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_mac.h"
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <sys/time.h>
#include <cJSON.h>
#include "lwip/inet.h"
#include "lwip/inet_chksum.h"
#include "lwip/prot/icmp.h"
#include "lwip/prot/ip.h"
#include "lwip/prot/ip4.h"

#define MAX_STA_NETWORKS 9

/* A known network that's visible in the scan but won't complete a connection
 * gets this many connect attempts before we give up on it and fall back to the
 * built-in AP. Without this a single failed association (which can take the
 * full connect timeout) immediately drops us to AP. */
#define WIFI_CONNECT_RETRIES 3

static TaskHandle_t netHandle = nullptr;
static SemaphoreHandle_t readySem = nullptr;

/* esp_netif handles */
static esp_netif_t* sta_netif = nullptr;
static esp_netif_t* ap_netif = nullptr;

/* Event-driven connection signaling */
static SemaphoreHandle_t wifiConnectedSem = nullptr;
static volatile bool staConnected = false;
static volatile bool cmdUp = false;
static volatile bool cmdDown = false;
static volatile bool cmdForceDown = false;
static volatile uint32_t lastActivityMs = 0;
static uint32_t connectTimeMs = 0;
static uint32_t trafficIn = 0;
static uint32_t trafficOut = 0;
#define WIFI_IDLE_TIMEOUT_MS 30000

/* ITS aux command interface */
enum { NET_CMD_UP = 1, NET_CMD_DOWN, NET_CMD_FORCE_DOWN, NET_CMD_CONNECT, NET_CMD_DISCONNECT,
       NET_CMD_WIFI_ADD, NET_CMD_WIFI_DEL };
static volatile int cmdConnectIdx = -1;  /* target network index for NET_CMD_CONNECT */
/* On-device WiFi add/delete sentinels (LCD WiFi pane). Captured once by the
 * wifi.cmd.* subscribers, applied in the net task loop (see netTaskFn). */
static volatile bool cmdWifiAdd = false;
static char          cmdWifiAddBuf[96];
static volatile bool cmdWifiDel = false;
static volatile int  cmdWifiDelIdx = -1;

/* RTC state: survives deep sleep. On cold boot, boot file runs "net up". */
RTC_DATA_ATTR static bool rtcWantUp = false;

/* The master off switch s.net.wifi.enable gates EVERY "should we be up?" check —
 * boot bring-up, scan, AP-retry. rtcWantUp is only a runtime cache (it can drift:
 * preserved across resets, set by net up/down), so gating on it alone let a stale
 * rtcWantUp=1 bring the radio up against an explicit enable=0. enable is the
 * persistent truth, so it wins here unconditionally. */
static bool wantUp() { return rtcWantUp && storageGetInt("s.net.wifi.enable", 1) != 0; }

static pm_lock_handle_t netDeepLock = nullptr;

/* ---- Event callback registry ---- */

#define NET_MAX_CBS 8

static struct {
    net_event_cb_t cbs[NET_MAX_CBS];
    int count;
} evRegistry[NET_EV_COUNT] = {};

/* Current link state, so a handler that registers AFTER the net task has
 * already brought the link up still gets the UP edge — registration order
 * vs. bring-up must not matter (this is what made mDNS flaky on fresh
 * AP-only boots: net task fired NET_EV_UP before mdnsInit registered). */
static bool s_linkUp   = false;  /* true between doUp() and doDown()  */
static bool upstreamUp = false;  /* true iff in ST_STA_CONNECTED      */

void netRegister(int event, net_event_cb_t cb) {
    if (event < 0 || event >= NET_EV_COUNT) return;
    auto& r = evRegistry[event];
    if (r.count < NET_MAX_CBS) r.cbs[r.count++] = cb;
    /* Level-replay the UP edges to a late subscriber. DOWN/CFG/POLL stay
     * edge-only (replaying a teardown to a handler that never set up would
     * be wrong). Note: cb may run synchronously here, on the caller's task
     * rather than the net task — UP handlers must be idempotent. */
    if      (event == NET_EV_UP          && s_linkUp)   cb(nullptr);
    else if (event == NET_EV_UPSTREAM_UP && upstreamUp) cb(nullptr);
}

static void fireEvent(int event, const char* arg = nullptr) {
    auto& r = evRegistry[event];
    for (int i = 0; i < r.count; i++) r.cbs[i](arg);
}

/* ---- TCP call center: endpoint table + client proxy ---- */

#define NET_MAX_ENDPOINTS 8
#define NET_MAX_CLIENTS   8

struct net_endpoint_t {
    TaskHandle_t task;
    uint16_t itsPort;
    int serverFd;
    int port;         /* currently open port */
    char nvsKey[16];
    int defaultPort;
    bool tls;
    bool tcpNoDelay;
    bool keepAlive;
    int backlog;
};

struct net_client_t {
    int fd;
    tls_conn_t* tlsConn;
    int itsHandle;    /* ITS client handle (-1 = inactive) */
    int epIdx;
    TaskHandle_t serverTask;
};

static net_endpoint_t netEps[NET_MAX_ENDPOINTS];
static int netEpCount = 0;
static net_client_t netClients[NET_MAX_CLIENTS];
static uint8_t* netProxyBuf;  /* 4096 bytes, PSRAM */


static net_endpoint_t* epFindByKey(const char* nvsKey) {
    for (int i = 0; i < netEpCount; i++)
        if (strcmp(netEps[i].nvsKey, nvsKey) == 0) return &netEps[i];
    return nullptr;
}

/* Register (or refresh) a TCP endpoint owned by `task`. Two callers: the aux
 * path below, where a task registers its own port, and netRegisterCorePorts(),
 * where net exposes the always-present core services (cli/log) on their behalf
 * — those tasks have no compile-time knowledge of TCP, so the dependency runs
 * net → core, never the reverse. */
static void epRegister(TaskHandle_t task, uint16_t itsPort, const char* nvsKey,
                       int defaultPort, bool tls, bool keepAlive, int backlog) {
    if (!task) return;
    net_endpoint_t* ep = epFindByKey(nvsKey);
    if (!ep) {
        if (netEpCount >= NET_MAX_ENDPOINTS) return;
        ep = &netEps[netEpCount++];
        memset(ep, 0, sizeof(*ep));
        ep->serverFd = -1;
        ep->port = 0;
    }
    ep->task = task;
    ep->itsPort = itsPort;
    safeStrncpy(ep->nvsKey, nvsKey, sizeof(ep->nvsKey));
    ep->defaultPort = defaultPort;
    ep->tls = tls;
    ep->tcpNoDelay = true;
    ep->keepAlive = keepAlive;
    ep->backlog = backlog > 0 ? backlog : 4;
}

/* ITS aux callback: tasks register TCP endpoints */
static void netOnAux(TaskHandle_t sender, const void* data, size_t len) {
    if (len < sizeof(net_port_msg_t)) return;
    auto* msg = (const net_port_msg_t*)data;
    epRegister(sender, msg->itsPort, msg->nvsKey, msg->defaultPort,
               msg->tls, msg->keepAlive, msg->backlog);
}

/* Expose the core platform services (CLI + log) over TCP. They live in
 * spangap-core and know nothing about net; net resolves their tasks by name
 * (ITS names them "cli"/"log") and registers their endpoints. Both default to
 * port 0 (off) in net's config tree — exposed only when the user sets
 * s.net.cli_port / s.net.log_port. */
static void netRegisterCorePorts() {
    epRegister(xTaskGetHandle("cli"), CLI_PORT_TCP, "cli_port", 0,
               /*tls=*/false, /*keepAlive=*/false, /*backlog=*/0);
    epRegister(xTaskGetHandle("log"), LOG_PORT_TCP, "log_port", 0,
               /*tls=*/false, /*keepAlive=*/false, /*backlog=*/0);
}

/* Run /state/net_up whenever the device reaches a real upstream. Net policy,
 * not a consumer's: it fires off NET_EV_UPSTREAM_UP, which only net knows
 * about, so net owns the boot hook and every net device gets it without
 * wiring anything in app_main. The script is run on its own task because
 * cliRunFile blocks until the script's commands drain. */
static void netUpScriptTask(void*) {
    cliRunFile(fsStatePath("/net_up").c_str());
    killSelf();
}

static void netOnUpstreamUp(const char*) {
    spawnTask(netUpScriptTask, "net_up", 4096, nullptr, 1, 1);
}

static void epOpenPort(net_endpoint_t& ep) {
    char fullKey[32];
    snprintf(fullKey, sizeof(fullKey), "s.net.%s", ep.nvsKey);
    int newPort = storageGetInt(fullKey, ep.defaultPort);
    if (newPort == ep.port && (newPort <= 0 || ep.serverFd >= 0)) return;
    if (ep.serverFd >= 0) {
        info("closing port %d (%s)\n", ep.port, ep.nvsKey);
        close(ep.serverFd);
        ep.serverFd = -1;
    }
    ep.port = newPort;
    if (newPort <= 0) return;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { err("port %d open failed\n", newPort); return; }
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(newPort);
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(s, ep.backlog) < 0) {
        err("port %d bind/listen failed\n", newPort);
        close(s); return;
    }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    ep.serverFd = s;
    info("opening port %d (%s)\n", newPort, ep.nvsKey);
}

static void epOpenAll() {
    for (int i = 0; i < netEpCount; i++) epOpenPort(netEps[i]);
}

static void netClientClose(net_client_t& c) {
    if (c.itsHandle >= 0) { itsDisconnect(c.itsHandle); c.itsHandle = -1; }
    if (c.tlsConn) tlsClose(c.tlsConn);
    else if (c.fd >= 0) close(c.fd);
    c.fd = -1;
    c.tlsConn = nullptr;
}

static void epCloseAll() {
    for (int i = 0; i < NET_MAX_CLIENTS; i++) netClientClose(netClients[i]);
    for (int i = 0; i < netEpCount; i++) {
        auto& ep = netEps[i];
        if (ep.serverFd >= 0) { close(ep.serverFd); ep.serverFd = -1; }
        ep.port = 0;
    }
}

static void netItsDisconnect(int handle);

static void netAcceptOne(int ei, int fd, tls_conn_t* conn, struct sockaddr_in* peer) {
    auto& ep = netEps[ei];
    int ci = -1;
    for (int i = 0; i < NET_MAX_CLIENTS; i++)
        if (netClients[i].fd < 0) { ci = i; break; }
    if (ci < 0) { if (conn) tlsClose(conn); else close(fd); return; }
    net_connect_t cd = { 0, (uint8_t)(conn ? 1 : 0), {} };
    ip_addr_set_ip4_u32_val(cd.clientAddr, peer->sin_addr.s_addr);
    int h = itsConnectByTaskHandle(ep.task, ep.itsPort, &cd, sizeof(cd),
                                    pdMS_TO_TICKS(100), -1, nullptr, netItsDisconnect);
    if (h < 0) { if (conn) tlsClose(conn); else close(fd); return; }
    netClients[ci] = { fd, conn, h, ei, ep.task };
}

/* Unified select on all server fds + client fds, then accept + proxy */
static void netPollOnce() {
    while (itsPoll(0)) {}
    epOpenAll();

    fd_set rfds;
    FD_ZERO(&rfds);
    int maxFd = -1;

    for (int i = 0; i < netEpCount; i++) {
        int sfd = netEps[i].serverFd;
        if (sfd >= 0) { FD_SET(sfd, &rfds); if (sfd > maxFd) maxFd = sfd; }
    }
    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        if (netClients[i].fd >= 0) {
            FD_SET(netClients[i].fd, &rfds);
            if (netClients[i].fd > maxFd) maxFd = netClients[i].fd;
        }
    }

    /* This is the net task's real block point, and it is timeout-driven, not
     * notify-driven: select() (and the idle vTaskDelay) wake on socket activity
     * or the 10ms tick, never on a task notify — so itsPoll's auto-boost is
     * never dropped here. The connected loop otherwise blocks only in select(),
     * so a CPU_FREQ_MAX count carried in from the OFF->up notify-wake would pin
     * 240 MHz for the whole time WiFi stays up. Release it before the block: the
     * steady proxy path rides the DFS floor (heavy throughput would opt into a
     * manual pmBoost()). Idempotent — a no-op once the count is gone. */
    pmBoostAuto(false);
    if (maxFd >= 0) {
        struct timeval tv = { 0, 10000 };
        select(maxFd + 1, &rfds, NULL, NULL, &tv);
    } else {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /* Accept new connections */
    for (int ei = 0; ei < netEpCount; ei++) {
        auto& ep = netEps[ei];
        if (ep.serverFd < 0 || !FD_ISSET(ep.serverFd, &rfds)) continue;
        if (ep.tls) {
            if (!tlsReady()) {
                /* Refuse connection with RST so browser backs off fast */
                struct sockaddr_in addr;
                socklen_t alen = sizeof(addr);
                int reject = accept(ep.serverFd, (struct sockaddr*)&addr, &alen);
                if (reject >= 0) {
                    struct linger lo = {1, 0};
                    setsockopt(reject, SOL_SOCKET, SO_LINGER, &lo, sizeof(lo));
                    close(reject);
                }
                continue;
            }
            tls_conn_t* conn = tlsAccept(ep.serverFd);
            if (!conn) continue;
            int fd = tlsFd(conn);
            if (ep.tcpNoDelay) { int yes = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)); }
            if (ep.keepAlive) {
                int yes = 1; setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
                int idle = 10; setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
                int intvl = 5; setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
                int cnt = 3;   setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
            }
            struct sockaddr_in sa = {};
            socklen_t sl = sizeof(sa);
            getpeername(fd, (struct sockaddr*)&sa, &sl);
            netAcceptOne(ei, fd, conn, &sa);
        } else {
            struct sockaddr_in peer;
            socklen_t peerLen = sizeof(peer);
            int fd = accept(ep.serverFd, (struct sockaddr*)&peer, &peerLen);
            if (fd < 0) continue;
            if (ep.tcpNoDelay) { int yes = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)); }
            if (ep.keepAlive) {
                int yes = 1; setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
                int idle = 10; setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
                int intvl = 5; setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
                int cnt = 3;   setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
            }
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
            netAcceptOne(ei, fd, nullptr, &peer);
        }
    }

    /* Proxy active clients */
    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        auto& c = netClients[i];
        if (c.fd < 0) continue;
        if (!itsConnected(c.itsHandle)) { netClientClose(c); continue; }

        bool canRecv = FD_ISSET(c.fd, &rfds);
        if (!canRecv && c.tlsConn && tlsBytesAvail(c.tlsConn) > 0) canRecv = true;
        if (canRecv) {
            int n = c.tlsConn ? tlsRead(c.tlsConn, netProxyBuf, 4096)
                              : recv(c.fd, netProxyBuf, 4096, MSG_DONTWAIT);
            /* Close on dead peer. TLS: tlsRead maps real error/EOF to <0 and
             * no-data (WANT_READ) to 0. Raw recv is the opposite — 0 is EOF,
             * <0 is either no-data (EAGAIN) or a real error (ECONNRESET, or
             * ETIMEDOUT from the keepalive set on accept). Treat any non-EAGAIN
             * negative as dead; otherwise an errored fd stays select-readable
             * forever and the net task spins at ~100% CPU with no log output. */
            bool dead = c.tlsConn ? (n < 0)
                                  : (n == 0 ||
                                     (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK));
            if (dead) { netClientClose(c); continue; }
            if (n > 0) { itsSend(c.itsHandle, netProxyBuf, n, 0); netActivity(); trafficIn += n; }
        }

        for (int rounds = 0; rounds < 4; rounds++) {
            size_t n = itsRecv(c.itsHandle, netProxyBuf, 4096, 0);
            if (n == 0) break;
            trafficOut += n;
            const uint8_t* p = netProxyBuf;
            size_t rem = n;
            while (rem > 0) {
                int sent = c.tlsConn ? tlsWrite(c.tlsConn, p, rem)
                                     : send(c.fd, p, rem, MSG_DONTWAIT);
                if (sent <= 0) { netClientClose(c); goto nextClient; }
                p += sent; rem -= sent;
            }
            netActivity();
        }
        nextClient:;
    }


    fireEvent(NET_EV_POLL);
}

/* Find client entry by matching ITS remote task */
static net_client_t* netFindClient(int serverHandle) {
    TaskHandle_t caller = xTaskGetCurrentTaskHandle();
    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        auto& c = netClients[i];
        if (c.fd < 0 || c.itsHandle < 0) continue;
        if (itsRemoteTask(c.itsHandle) == caller) return &c;
    }
    return nullptr;
}

/* ITS disconnect callback */
static void netItsDisconnect(int handle) {
    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        if (netClients[i].itsHandle == handle) {
            if (netClients[i].tlsConn) tlsClose(netClients[i].tlsConn);
            else if (netClients[i].fd >= 0) close(netClients[i].fd);
            netClients[i].fd = -1;
            netClients[i].tlsConn = nullptr;
            netClients[i].itsHandle = -1;
            break;
        }
    }
}

/* ---- NET_PORT_TCP_DIAL: outbound dial-on-behalf-of ---- */

/* Synchronous DNS + non-blocking connect with bounded wait. Returns
 * connected fd >= 0 on success, -1 on failure. Runs on the net task —
 * a slow DNS lookup briefly stalls net's select loop, acceptable for
 * the dial cadence (RNS peer reconnects, not high-rate). */
static int netDialSync(const char* host, uint16_t port, uint32_t timeoutMs) {
    if (!netIsStaConnected()) return -1;

    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%u", (unsigned)port);
    /* AF_UNSPEC so a host with both A and AAAA records is reachable — getaddrinfo
     * orders the candidates per RFC 6724 (IPv6 preferred when we have a global
     * v6 source address), and we try them in turn until one connects. This is
     * what lets us reach v6-only hosts (e.g. play.rop.nl off-VPN). */
    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host, portStr, &hints, &res) != 0 || !res) {
        warn("dial: DNS failed for %s\n", host);
        return -1;
    }

    int fd = -1;
    for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        int yes = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno != EINPROGRESS) {
            warn("dial: connect %s:%u immediate fail (af=%d errno %d)\n",
                 host, port, ai->ai_family, errno);
            close(fd); fd = -1; continue;
        }
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv = { (long)(timeoutMs / 1000), (long)((timeoutMs % 1000) * 1000) };
        int sel = select(fd + 1, nullptr, &wfds, nullptr, &tv);
        if (sel <= 0) {
            warn("dial: connect %s:%u timeout (af=%d)\n", host, port, ai->ai_family);
            close(fd); fd = -1; continue;
        }
        int soerr = 0;
        socklen_t soerrLen = sizeof(soerr);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &soerrLen);
        if (soerr != 0) {
            warn("dial: connect %s:%u failed (af=%d errno %d)\n",
                 host, port, ai->ai_family, soerr);
            close(fd); fd = -1; continue;
        }
        info("dial: connected %s:%u (af=%d fd=%d)\n", host, port, ai->ai_family, fd);
        break;
    }
    freeaddrinfo(res);
    return fd;
}

static int netOnDialConnect(int handle, const void* data, size_t len) {
    if (!data || len == 0) return -1;
    char buf[96];
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, data, n);
    buf[n] = '\0';
    char* colon = strrchr(buf, ':');
    if (!colon) return -1;
    *colon = '\0';
    int port = atoi(colon + 1);
    if (port <= 0 || port > 65535) return -1;

    int fd = netDialSync(buf, (uint16_t)port, 8000);
    if (fd < 0) return -1;

    int ci = -1;
    for (int i = 0; i < NET_MAX_CLIENTS; i++)
        if (netClients[i].fd < 0) { ci = i; break; }
    if (ci < 0) { close(fd); return -1; }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
    int idle = 30; setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    int intvl = 10; setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    int cnt = 3;   setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));

    netClients[ci] = { fd, nullptr, handle, -1, nullptr };
    return ci;
}

static void netOnDialDisconnect(int ref) {
    if (ref < 0 || ref >= NET_MAX_CLIENTS) return;
    auto& c = netClients[ref];
    if (c.fd >= 0) close(c.fd);
    c.fd = -1;
    c.tlsConn = nullptr;
    c.itsHandle = -1;
}

/* ---- WiFi event handler ---- */

static void wifi_event_handler(void* arg, esp_event_base_t base,
                               int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    staConnected = false;
  else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
    /* Kick off IPv6: forms the link-local now and, with SLAAC
     * (CONFIG_LWIP_IPV6_AUTOCONFIG), a global address from router RAs — needed
     * to reach v6-only hosts like play.rop.nl off-VPN. */
    esp_netif_create_ip6_linklocal(sta_netif);
  }
  else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    staConnected = true;
    xSemaphoreGive(wifiConnectedSem);
  }
  else if (base == IP_EVENT && id == IP_EVENT_GOT_IP6) {
    ip_event_got_ip6_t* e = (ip_event_got_ip6_t*)data;
    info("net: IPv6 " IPV6STR "\n", IPV62STR(e->ip6_info.ip));
  }
}

/* ---- WiFi helpers ---- */

static void wifiNetifInit() {
  esp_netif_init();
  esp_event_loop_create_default();
  sta_netif = esp_netif_create_default_wifi_sta();
  ap_netif  = esp_netif_create_default_wifi_ap();
  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr);
  esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, &wifi_event_handler, nullptr);
}

static void wifiHwStart(wifi_mode_t mode = WIFI_MODE_STA) {
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(mode);
  esp_wifi_start();
  esp_sleep_enable_wifi_wakeup();
}

/** Number of configured STA networks (from s.net.wifi.nets array). */
static int staNetCount() { return storageArrayCount("s.net.wifi.nets"); }

/** Read a field from s.net.wifi.nets[idx]. */
static void staNetGet(int idx, const char* field, char* out, size_t len, const char* def = "") {
  char key[48];
  snprintf(key, sizeof(key), "s.net.wifi.nets.%d.%s", idx, field);
  storageGetStr(key, out, len, def);
}

static int staNetFindBySsid(const char* ssid);   /* defined below; used by the task loop */

/** Remove known network [idx], shifting later entries down and dropping the
 *  tail — the array-correct delete (a bare delete of an index would leave a
 *  hole). Shared by the `net delete` CLI verb and the on-device `wifi.cmd.del`
 *  sentinel (the LCD WiFi pane writes that; the browser rewrites the array). */
static void staNetDeleteIdx(int idx) {
  int total = staNetCount();
  if (idx < 0 || idx >= total) return;
  storageBegin();
  for (int i = idx; i < total - 1; i++) {
    char src[64], dst[64];
    for (const char* f : { "ssid", "pass", "ip", "gw", "mask", "dns", "mac" }) {
      snprintf(src, sizeof(src), "s.net.wifi.nets.%d.%s", i + 1, f);
      snprintf(dst, sizeof(dst), "s.net.wifi.nets.%d.%s", i, f);
      std::string v = storageGetStr(src, "");
      if (v.empty()) storageUnset(dst);
      else           storageSet(dst, v.c_str());
    }
  }
  char tail[64];
  snprintf(tail, sizeof(tail), "s.net.wifi.nets.%d", total - 1);
  storageDeleteTree(tail);
  storageEnd();
}

static void logNetworks() {
  int n = staNetCount();
  info("known networks: %d\n", n);
  for (int i = 0; i < n; i++) {
    char ssid[33];
    staNetGet(i, "ssid", ssid, sizeof(ssid));
    info("  [%d] '%s'\n", i, ssid);
  }
}

static int scanForKnown() {
  info("scanning...\n");
  wifi_scan_config_t scan_config = {};
  esp_err_t e = esp_wifi_scan_start(&scan_config, true);
  if (e != ESP_OK) { info("scan start failed: %s\n", esp_err_to_name(e)); return -1; }
  uint16_t ap_count = 0;
  esp_wifi_scan_get_ap_num(&ap_count);
  if (ap_count == 0) { info("scan found 0 networks\n"); return -1; }
  info("scan found %d networks\n", ap_count);
  wifi_ap_record_t* ap_list = (wifi_ap_record_t*)gp_alloc(ap_count * sizeof(wifi_ap_record_t));
  if (!ap_list) return -1;
  esp_wifi_scan_get_ap_records(&ap_count, ap_list);
  for (int i = 0; i < ap_count; i++)
    dbg("  '%s' ch%d %ddBm\n", (const char*)ap_list[i].ssid, ap_list[i].primary, ap_list[i].rssi);
  int bestIdx = -1;
  int nNets = staNetCount();
  logNetworks();
  for (int s = 0; s < nNets; s++) {
    char ssid[33];
    staNetGet(s, "ssid", ssid, sizeof(ssid));
    for (int i = 0; i < ap_count; i++)
      if (strcmp((const char*)ap_list[i].ssid, ssid) == 0) { bestIdx = s; goto found; }
  }
found:
  free(ap_list);
  return bestIdx;
}

/* ---- WiFi state machine ---- */

enum wifi_state_t { ST_OFF, ST_SCANNING, ST_STA_CONNECTED, ST_AP };
static volatile wifi_state_t wifiState = ST_OFF;
/* s_linkUp / upstreamUp are declared up by netRegister() so it can replay
 * current state to late subscribers. */

/** Sync upstream state (NET_EV_UPSTREAM_UP/DOWN events) to whether we're
 *  STA-connected. Idempotent — only fires on real transitions. */
static void setUpstream(bool up) {
  if (upstreamUp == up) return;
  upstreamUp = up;
  /* Ephemeral readiness flag the rns boot barrier waits on (only when WiFi is
   * configured — see net.want). Decoupled by storage key so rns has no net
   * dependency and net-less builds simply never set it. */
  storageSet("net.up", up ? 1 : 0);
  fireEvent(up ? NET_EV_UPSTREAM_UP : NET_EV_UPSTREAM_DOWN);
}

/** Perform a WiFi scan and publish results to wifi.scanned as a JSON array.
 *  Each element: {ssid, bssid, rssi, locked}. Sorted by RSSI (strongest first). */
static void publishScanResults() {
  wifi_scan_config_t scan_config = {};
  esp_err_t e = esp_wifi_scan_start(&scan_config, true);
  if (e != ESP_OK) { info("scan failed: %s\n", esp_err_to_name(e)); return; }
  uint16_t ap_count = 0;
  esp_wifi_scan_get_ap_num(&ap_count);
  if (ap_count == 0) {
    storageSetTree("wifi.scanned", cJSON_CreateArray());
    return;
  }
  wifi_ap_record_t* ap_list = (wifi_ap_record_t*)gp_alloc(ap_count * sizeof(wifi_ap_record_t));
  if (!ap_list) return;
  esp_wifi_scan_get_ap_records(&ap_count, ap_list);

  /* Sort by RSSI descending (strongest first) */
  for (int i = 0; i < ap_count - 1; i++)
    for (int j = i + 1; j < ap_count; j++)
      if (ap_list[j].rssi > ap_list[i].rssi) {
        wifi_ap_record_t tmp = ap_list[i];
        ap_list[i] = ap_list[j];
        ap_list[j] = tmp;
      }

  cJSON* arr = cJSON_CreateArray();
  for (int i = 0; i < ap_count; i++) {
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "ssid", (const char*)ap_list[i].ssid);
    char bssid[18];
    snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
             ap_list[i].bssid[0], ap_list[i].bssid[1], ap_list[i].bssid[2],
             ap_list[i].bssid[3], ap_list[i].bssid[4], ap_list[i].bssid[5]);
    cJSON_AddStringToObject(obj, "bssid", bssid);
    cJSON_AddNumberToObject(obj, "rssi", ap_list[i].rssi);
    cJSON_AddNumberToObject(obj, "locked", ap_list[i].authmode != WIFI_AUTH_OPEN ? 1 : 0);
    cJSON_AddItemToArray(arr, obj);
  }
  free(ap_list);
  storageSetTree("wifi.scanned", arr);
}

/** Publish current WiFi status as ephemeral keys.
 *  wifi.sta.state — "off", "connecting", "connected"
 *  wifi.sta.*     — station info (ssid, ip, router, etc.)
 *  wifi.ap.state  — "off", "active"
 *  wifi.ap.*      — access point info
 *  wifi.mac       — STA MAC address */
static void publishWifiStatus() {
  storageBegin();

  /* STA MAC address (always available — base MAC from efuse) */
  uint8_t mac[6];
  if (wifiState == ST_OFF || esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK)
    esp_efuse_mac_get_default(mac);
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  storageSet("wifi.mac", macStr);

  char inBuf[16], outBuf[16];
  fmtSize(trafficIn, inBuf, sizeof(inBuf));
  fmtSize(trafficOut, outBuf, sizeof(outBuf));
  storageSet("wifi.traffic_in", inBuf);
  storageSet("wifi.traffic_out", outBuf);

  /* STA status */
  bool connecting = (wifiState == ST_SCANNING);
  storageSet("wifi.sta.state", wifiState == ST_STA_CONNECTED ? "connected"
                               : connecting ? "connecting" : "off");
  if (wifiState == ST_STA_CONNECTED) {
    wifi_ap_record_t ap_info = {};
    const char* ssid = "";
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) ssid = (const char*)ap_info.ssid;
    storageSet("wifi.sta.ssid", ssid);
    storageSet("wifi.sta.rssi", (int)ap_info.rssi);
    storageSet("wifi.sta.channel", (int)ap_info.primary);

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(sta_netif, &ip_info);
    char ip[16], gw[16], mask[16];
    esp_ip4addr_ntoa(&ip_info.ip, ip, sizeof(ip));
    esp_ip4addr_ntoa(&ip_info.gw, gw, sizeof(gw));
    esp_ip4addr_ntoa(&ip_info.netmask, mask, sizeof(mask));
    storageSet("wifi.sta.ip", ip);
    storageSet("wifi.sta.router", gw);
    storageSet("wifi.sta.netmask", mask);

    esp_netif_dns_info_t dns1 = {};
    char dns1s[16];
    esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns1);
    esp_ip4addr_ntoa(&dns1.ip.u_addr.ip4, dns1s, sizeof(dns1s));
    storageSet("wifi.sta.dns", dns1s);
    storageSet("wifi.sta.up", 1);
  } else {
    storageSet("wifi.sta.ssid", "");
    storageSet("wifi.sta.ip", "");
    storageSet("wifi.sta.router", "");
    storageSet("wifi.sta.netmask", "");
    storageSet("wifi.sta.dns", "");
    storageSet("wifi.sta.rssi", 0);
    storageSet("wifi.sta.channel", 0);
    storageSet("wifi.sta.up", 0);
  }

  /* AP status — active in ST_AP or during APSTA transitions */
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (wifiState != ST_OFF) esp_wifi_get_mode(&mode);
  bool apActive = (wifiState == ST_AP) || (mode == WIFI_MODE_APSTA);
  storageSet("wifi.ap.state", apActive ? "active" : "off");
  if (apActive) {
    char ssid[33];
    storageGetStr("s.net.wifi.ap.ssid", ssid, sizeof(ssid), WIFI_AP_SSID);
    storageSet("wifi.ap.ssid", ssid);

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(ap_netif, &ip_info);
    char ip[16], mask[16];
    esp_ip4addr_ntoa(&ip_info.ip, ip, sizeof(ip));
    esp_ip4addr_ntoa(&ip_info.netmask, mask, sizeof(mask));
    storageSet("wifi.ap.ip", ip);
    storageSet("wifi.ap.netmask", mask);
    storageSet("wifi.ap.up", 1);
  } else {
    storageSet("wifi.ap.ssid", "");
    storageSet("wifi.ap.ip", "");
    storageSet("wifi.ap.netmask", "");
    storageSet("wifi.ap.up", 0);
  }
  storageEnd();
}

static bool connectSta(int idx) {
  char ssid[33], pass[65], ip[16], gw[16], mask[16], dns[16], macStr[18];
  staNetGet(idx, "ssid", ssid, sizeof(ssid));
  staNetGet(idx, "pass", pass, sizeof(pass));
  staNetGet(idx, "ip",   ip,   sizeof(ip));
  staNetGet(idx, "gw",   gw,   sizeof(gw));
  staNetGet(idx, "mask", mask, sizeof(mask));
  staNetGet(idx, "dns",  dns,  sizeof(dns));
  staNetGet(idx, "mac",  macStr, sizeof(macStr));
  info("connecting to '%s' pass(%d chars)\n", ssid, (int)strlen(pass));
  esp_wifi_disconnect();
  delay(100);
  esp_wifi_set_mode(WIFI_MODE_STA);

  /* Custom MAC: set if configured, restore default if a custom one was active */
  static bool customMacActive = false;
  if (macStr[0]) {
    uint8_t mac[6];
    if (sscanf(macStr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6) {
      esp_wifi_set_mac(WIFI_IF_STA, mac);
      customMacActive = true;
      info("custom MAC %s\n", macStr);
    }
  } else if (customMacActive) {
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    esp_wifi_set_mac(WIFI_IF_STA, mac);
    customMacActive = false;
  }
  delay(100);
  if (ip[0]) {
    esp_netif_dhcpc_stop(sta_netif);
    esp_netif_ip_info_t ip_info = {};
    ip_info.ip.addr      = ipaddr_addr(ip);
    ip_info.gw.addr      = ipaddr_addr(gw);
    ip_info.netmask.addr = ipaddr_addr(mask);
    esp_netif_set_ip_info(sta_netif, &ip_info);
    esp_netif_dns_info_t dns_info = {};
    if (dns[0])
      dns_info.ip.u_addr.ip4.addr = ipaddr_addr(dns);
    else
      dns_info.ip.u_addr.ip4.addr = ip_info.gw.addr;
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns_info);
  } else {
    esp_netif_dhcpc_start(sta_netif);
  }
  wifi_config_t wifi_config = {};
  strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
  if (pass[0])
    strncpy((char*)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
  esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
  xSemaphoreTake(wifiConnectedSem, 0);
  staConnected = false;
  esp_wifi_connect();
  bool ok;
  if (ip[0]) {
    uint32_t t = millis();
    while (!staConnected && millis() - t < 25000) delay(200);
    wifi_ap_record_t ap_info;
    ok = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
  } else {
    ok = (xSemaphoreTake(wifiConnectedSem, pdMS_TO_TICKS(25000)) == pdTRUE);
  }
  if (ok) {
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(sta_netif, &ip_info);
    char ip_str[16];
    esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
    esp_netif_dns_info_t dns_info;
    esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns_info);
    char dns_str[16];
    esp_ip4addr_ntoa(&dns_info.ip.u_addr.ip4, dns_str, sizeof(dns_str));
    info("%s ip %s dns %s\n", ssid, ip_str, dns_str);
    return true;
  }
  info("%s connect failed\n", ssid);
  esp_wifi_disconnect();
  return false;
}

static bool startAP() {
  if (storageGetInt("s.net.wifi.ap.disable")) { info("AP disabled\n"); return false; }
  char ssid[33], pass[65], ip[16], mask[16];
  storageGetStr("s.net.wifi.ap.ssid", ssid, sizeof(ssid), WIFI_AP_SSID);
  storageGetStr("s.net.wifi.ap.pass", pass, sizeof(pass), WIFI_AP_PASS);
  storageGetStr("s.net.wifi.ap.ip",   ip,   sizeof(ip),   WIFI_AP_IP);
  storageGetStr("s.net.wifi.ap.mask", mask, sizeof(mask),  WIFI_AP_MASK);
  /* Configure AP IP before setting mode — prevents DHCP auto-start on default 192.168.4.1 */
  esp_netif_dhcps_stop(ap_netif);
  esp_netif_ip_info_t ip_info = {};
  ip_info.ip.addr      = ipaddr_addr(ip);
  ip_info.gw.addr      = ipaddr_addr(ip);
  ip_info.netmask.addr = ipaddr_addr(mask);
  esp_netif_set_ip_info(ap_netif, &ip_info);
  esp_wifi_set_mode(WIFI_MODE_AP);
  esp_netif_dhcps_start(ap_netif);
  wifi_config_t wifi_config = {};
  strncpy((char*)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid));
  wifi_config.ap.ssid_len = strlen(ssid);
  if (pass[0]) {
    strncpy((char*)wifi_config.ap.password, pass, sizeof(wifi_config.ap.password));
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
  } else {
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;
  }
  wifi_config.ap.max_connection = 4;
  esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
  char ip_str[16]; esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
  info("AP ssid=%s ip=%s\n", ssid, ip_str);
  return true;
}

static void setDhcpHostname() {
  char hostname[32];
  storageGetStr("s.net.hostname", hostname, sizeof(hostname), "");
  esp_netif_set_hostname(sta_netif, hostname);
}

static void doUp(wifi_state_t newState) {
  /* Sync global so publishWifiStatus below sees the new state. The main loop
   * also assigns `wifiState = state` after we return, but subscribers to
   * wifi.{sta,ap}.up read the value published here. */
  wifiState = newState;
  s_linkUp = true;         /* set before fireEvent so late-replay is consistent */
  esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
  connectTimeMs = millis();
  trafficIn = trafficOut = 0;
  lastActivityMs = millis();
  epOpenAll();
  info("net up\n");
  fireEvent(NET_EV_UP);
  setUpstream(newState == ST_STA_CONNECTED);
  publishWifiStatus();
}

static void doDown(wifi_state_t& state) {
  info("shutting down\n");
  s_linkUp = false;        /* clear before fireEvent so late-replay is consistent */
  setUpstream(false);
  fireEvent(NET_EV_DOWN);
  epCloseAll();
  delay(200);
  esp_wifi_disconnect();
  esp_wifi_stop();
  esp_wifi_deinit();
  state = ST_OFF;
  wifiState = ST_OFF;  /* sync before publishWifiStatus, so wifi.{sta,ap}.up=0 */
  pmLockRelease(netDeepLock);
  publishWifiStatus();
}

static volatile bool cmdDisconnect = false;
static volatile uint32_t lastBrowserScanMs = 0;

static void netCmdHandler(TaskHandle_t, const void* data, size_t len) {
  if (len < 1) return;
  uint8_t cmd = *(const uint8_t*)data;
  switch (cmd) {
    case NET_CMD_UP:         cmdUp = true; break;
    case NET_CMD_DOWN:       cmdDown = true; break;
    case NET_CMD_FORCE_DOWN: cmdForceDown = true; break;
    case NET_CMD_DISCONNECT: cmdDisconnect = true; break;
    case NET_CMD_CONNECT:
      if (len >= 2) cmdConnectIdx = ((const uint8_t*)data)[1];
      break;
    case NET_CMD_WIFI_ADD: {
      /* payload after the cmd byte is "<ssid>\t<pass>" (not NUL-terminated). */
      size_t n = (len > 1) ? len - 1 : 0;
      if (n >= sizeof(cmdWifiAddBuf)) n = sizeof(cmdWifiAddBuf) - 1;
      memcpy(cmdWifiAddBuf, (const uint8_t*)data + 1, n);
      cmdWifiAddBuf[n] = '\0';
      cmdWifiAdd = true;
      break;
    }
    case NET_CMD_WIFI_DEL:
      if (len >= 2) { cmdWifiDelIdx = ((const uint8_t*)data)[1]; cmdWifiDel = true; }
      break;
  }
}


static void netTaskFn(void* arg) {
  /* Allocate the proxy buffer in task context so heap tracking attributes it
     to net, not the main task that spawned us. */
  netProxyBuf = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
  itsClientInit(NET_MAX_CLIENTS);
  itsServerInit();
  itsServerPortOpen(NET_PORT_TCP_DIAL, /*packetBased=*/false,
                    /*maxHandles=*/NET_MAX_CLIENTS,
                    /*toSize=*/4096, /*fromSize=*/4096);
  itsServerOnConnect(NET_PORT_TCP_DIAL, netOnDialConnect);
  itsServerOnDisconnect(NET_PORT_TCP_DIAL, netOnDialDisconnect);
  itsOnAux(NET_PORT_REG_PORT, netOnAux);
  itsOnAux(NET_CMD_PORT, netCmdHandler);

  /* Pre-register the core CLI/log TCP endpoints. spangapInit() brought those
   * tasks up before this straddle's init hook ran, so xTaskGetHandle resolves
   * them now; epOpenAll() opens them (if configured) once net is up. */
  netRegisterCorePorts();

  /* Run /state/net_up on every upstream-up — net policy, not the consumer's. */
  netRegister(NET_EV_UPSTREAM_UP, netOnUpstreamUp);

  /* Tell the rns boot barrier whether to wait for the network: 1 if any STA
   * network is configured (so net.up is expected), else 0 so a WiFi-less node
   * doesn't stall the barrier waiting for an IP that will never come. Ephemeral
   * key keeps rns decoupled from net (and net-less builds never set it). */
  storageSet("net.want", staNetCount() > 0 ? 1 : 0);

  /* Net's own config: re-open endpoints when ports/host change. */
  storageSubscribeChanges("s.net.", ON_CHANGE {
    if (!netIsUp()) return;
    fireEvent(NET_EV_CFG_CHANGED, key);
    epOpenAll();
  });

  /* Re-broadcast specific prefixes/keys via NET_EV_CFG_CHANGED for module
   * helpers that don't have their own task (wg, ntp). Narrow on purpose —
   * wildcarding "s." floods this task's inbox during burst writes (boot
   * script, firmware default install, etc.) and the broad scope buys
   * nothing: wgOnCfg/ntpOnCfg already filter by key prefix anyway. */
  static auto rebroadcast = [](const char* key, const char*) {
    if (!netIsUp()) return;
    fireEvent(NET_EV_CFG_CHANGED, key);
  };
  storageSubscribeChanges("s.wg.",        rebroadcast);
  storageSubscribeChanges("secrets.wg.",  rebroadcast);
  storageSubscribeChanges("s.ntp.",       rebroadcast);
  storageSubscribeChanges("wg.keygen",    rebroadcast);
  storageSubscribeChanges("sys.time.set", rebroadcast);

  /* Browser WiFi panel triggers */
  storageSubscribeChanges("wifi.scan", ON_CHANGE {
    if (strcmp(key, "wifi.scan") != 0) return;  /* don't match wifi.scanned */
    if (atoi(val) == 1) lastBrowserScanMs = 0;  /* trigger immediate scan */
  });
  storageSubscribeChanges("wifi.connect", ON_CHANGE {
    if (strcmp(key, "wifi.connect") != 0) return;  /* prefix also matches wifi.connecting etc */
    int idx = atoi(val);
    if (idx >= 0 && idx < MAX_STA_NETWORKS) {
      uint8_t buf[2] = { NET_CMD_CONNECT, (uint8_t)idx };
      itsSendAuxByTaskHandle(netHandle, NET_CMD_PORT, buf, 2, pdMS_TO_TICKS(100));
    }
  });
  storageSubscribeChanges("wifi.disconnect", ON_CHANGE {
    if (atoi(val) == 1) {
      uint8_t cmd = NET_CMD_DISCONNECT;
      itsSendAuxByTaskHandle(netHandle, NET_CMD_PORT, &cmd, 1, pdMS_TO_TICKS(100));
    }
  });
  /* On-device WiFi add/delete (LCD WiFi pane). Forward to the net task over ITS
   * (like connect) so the array writes happen there, not on the storage actor. */
  storageSubscribeChanges("wifi.cmd.add", ON_CHANGE {
    if (strcmp(key, "wifi.cmd.add") != 0 || !val || !*val) return;
    uint8_t buf[1 + sizeof(cmdWifiAddBuf)];
    buf[0] = NET_CMD_WIFI_ADD;
    size_t n = strlen(val);
    if (n > sizeof(cmdWifiAddBuf) - 1) n = sizeof(cmdWifiAddBuf) - 1;
    memcpy(buf + 1, val, n);
    itsSendAuxByTaskHandle(netHandle, NET_CMD_PORT, buf, 1 + n, pdMS_TO_TICKS(100));
  });
  storageSubscribeChanges("wifi.cmd.del", ON_CHANGE {
    if (strcmp(key, "wifi.cmd.del") != 0 || !val || !*val) return;
    uint8_t buf[2] = { NET_CMD_WIFI_DEL, (uint8_t)atoi(val) };
    itsSendAuxByTaskHandle(netHandle, NET_CMD_PORT, buf, 2, pdMS_TO_TICKS(100));
  });

  /* Master WiFi switch. s.net.wifi.enable was a defined config key (default 1)
   * that nothing consumed — setting it 0 did nothing, the radio kept scanning.
   * Bring net down/up to match; cold boot also seeds rtcWantUp from it (netInit).
   * netDown/netUp here post a command to our own inbox, picked up next loop. */
  storageSubscribeChanges("s.net.wifi.enable", ON_CHANGE {
    if (strcmp(key, "s.net.wifi.enable") != 0) return;
    if (atoi(val) == 0) { info("wifi.enable=0 → bringing net down\n"); netDown(true); }
    else                { info("wifi.enable=1 → bringing net up\n");   netUp(); }
  });

  wifiNetifInit();

  xSemaphoreGive(readySem);  /* unblock netInit — task is running */

  int searchTimeout = storageGetInt("s.net.wifi.timeout");
  wifi_state_t state = ST_OFF;
  uint32_t scanStartMs = millis();
  uint32_t lastApRetryMs = 0;
  uint32_t lastStatusMs = 0;
  /* Consecutive failed connects to a *seen* known network in ST_SCANNING.
   * Every exit from scanning (connected / AP fallback) resets it to 0. */
  int connectFails = 0;
  /* AP retry interval read from s.net.wifi.ap.retry (default 300s) */

  if (wantUp()) {
    pmLockAcquire(netDeepLock);
    setDhcpHostname();
    wifiHwStart(WIFI_MODE_STA);
    state = ST_SCANNING;
    if (staNetCount() == 0) {
      if (startAP()) { state = ST_AP; doUp(state); }
      else { state = ST_OFF; pmLockRelease(netDeepLock); }
    }
  }

  for (;;) {
    bool connected = (state == ST_STA_CONNECTED || state == ST_AP);

    /* Sleep when off; drain ITS (non-blocking when connected — select handles timing) */
    { TickType_t t = (state == ST_OFF) ? portMAX_DELAY : 0;
      while (itsPoll(t)) { t = 0; } }

    /* Process command flags set by aux handler */
    if (cmdForceDown) {
      cmdForceDown = false;
      rtcWantUp = false;
      if (state != ST_OFF) { doDown(state); wifiState = state; }
      continue;
    }

    if (cmdUp) {
      cmdUp = false;
      rtcWantUp = true;
      if (state == ST_OFF) {
        info("coming up\n");
        pmLockAcquire(netDeepLock);
        setDhcpHostname();
        wifiHwStart(WIFI_MODE_STA);
        state = ST_SCANNING;
        scanStartMs = millis();
        wifiState = state;
      }
    }

    if (cmdDown) {
      cmdDown = false;
      rtcWantUp = false;
      if (connected) {
        info("going down (waiting for idle)\n");
      } else if (state == ST_SCANNING) {
        doDown(state); wifiState = state; continue;
      }
    }

    /* Browser-triggered disconnect */
    if (cmdDisconnect) {
      cmdDisconnect = false;
      storageDeleteTree("wifi.disconnect");
      if (state == ST_STA_CONNECTED) {
        info("browser disconnect\n");
        fireEvent(NET_EV_DOWN);
        epCloseAll();
        esp_wifi_disconnect();
        if (startAP()) { state = ST_AP; doUp(state); }
        else { state = ST_SCANNING; scanStartMs = millis(); }
        wifiState = state;
        publishWifiStatus();
        continue;
      }
    }

    /* Browser-triggered connect to specific known network */
    if (cmdConnectIdx >= 0) {
      int idx = cmdConnectIdx;
      cmdConnectIdx = -1;
      storageDeleteTree("wifi.connect");
      if (idx < staNetCount()) {
        info("browser connect to net %d\n", idx);
        if (state == ST_STA_CONNECTED || state == ST_AP) {
          fireEvent(NET_EV_DOWN);
          epCloseAll();
        }
        /* Use APSTA if we're in AP mode so browser stays connected */
        if (state == ST_AP)
          esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (connectSta(idx)) {
          state = ST_STA_CONNECTED;
          doUp(state);
        } else {
          /* Connect failed — stay in or start AP */
          if (state != ST_AP) {
            if (startAP()) { state = ST_AP; doUp(state); }
            else { state = ST_SCANNING; scanStartMs = millis(); }
          } else {
            esp_wifi_set_mode(WIFI_MODE_AP);
            doUp(state);
          }
        }
        wifiState = state;
        publishWifiStatus();
        continue;
      }
    }

    /* On-device WiFi add (the LCD WiFi pane writes wifi.cmd.add="<ssid>\t<pass>";
     * the browser rewrites s.net.wifi.nets[] directly). Captured once by the
     * subscriber, applied here in the net task so the array writes run in a safe
     * context. The join is emitted as a SEPARATE wifi.connect op: storage ops
     * commit in emit order, so ssid/pass are committed before the connect
     * handler reads them — the same ordered add-then-connect the browser uses. */
    if (cmdWifiAdd) {
      cmdWifiAdd = false;
      storageDeleteTree("wifi.cmd.add");
      std::string add = cmdWifiAddBuf;
      size_t tab = add.find('\t');
      std::string ssid = add.substr(0, tab == std::string::npos ? add.size() : tab);
      std::string pass = tab == std::string::npos ? "" : add.substr(tab + 1);
      if (!ssid.empty()) {
        int idx = staNetFindBySsid(ssid.c_str());
        if (idx < 0) idx = staNetCount();
        char k[64], v[8];
        snprintf(k, sizeof(k), "s.net.wifi.nets.%d.ssid", idx); storageSet(k, ssid.c_str());
        snprintf(k, sizeof(k), "s.net.wifi.nets.%d.pass", idx); storageSet(k, pass.c_str());
        snprintf(v, sizeof(v), "%d", idx); storageSet("wifi.connect", v);
        info("on-device add '%s' at %d — joining\n", ssid.c_str(), idx);
      }
      continue;
    }
    /* On-device WiFi delete: wifi.cmd.del="<index>" → array-correct remove. */
    if (cmdWifiDel) {
      cmdWifiDel = false;
      storageDeleteTree("wifi.cmd.del");
      staNetDeleteIdx(cmdWifiDelIdx);
      continue;
    }

    /* Graceful shutdown: want_up=0 + connected → wait for idle */
    if (!wantUp() && connected) {
      if (millis() - lastActivityMs >= WIFI_IDLE_TIMEOUT_MS) {
        info("idle, shutting down\n");
        doDown(state); wifiState = state; continue;
      }
    }

    /* Periodic status publishing (~30s) */
    if (connected && millis() - lastStatusMs >= 30000) {
      lastStatusMs = millis();
      uint32_t t0 = millis();
      publishWifiStatus();
      uint32_t dt = millis() - t0;
      if (dt > 200) verb("publishWifiStatus took %ums\n", (unsigned)dt);
    }

    /* Browser-triggered WiFi scan (every 20s while wifi.scan=1) */
    if (connected && storageGetInt("wifi.scan") == 1 &&
        millis() - lastBrowserScanMs >= 20000) {
      lastBrowserScanMs = millis();
      if (state == ST_AP)
        esp_wifi_set_mode(WIFI_MODE_APSTA);
      publishScanResults();
      if (state == ST_AP)
        esp_wifi_set_mode(WIFI_MODE_AP);
    }

    switch (state) {
      case ST_OFF: break;
      case ST_SCANNING: {
        int idx = scanForKnown();
        if (idx >= 0) {
          /* A known network is in range. Give it WIFI_CONNECT_RETRIES attempts
           * before falling back to AP — a flaky AP or slow DHCP shouldn't bump
           * us off the network the user actually wants on the first miss. */
          if (connectSta(idx)) {
            connectFails = 0;
            state = ST_STA_CONNECTED;
            doUp(state);
          } else if (++connectFails >= WIFI_CONNECT_RETRIES) {
            info("connect to known network failed %dx, falling back to AP\n", connectFails);
            connectFails = 0;
            if (startAP()) { state = ST_AP; doUp(state); }
            else { state = ST_OFF; pmLockRelease(netDeepLock); }
          } else {
            info("connect failed (attempt %d/%d), retrying\n", connectFails, WIFI_CONNECT_RETRIES);
            delay(2000);
          }
        } else if (millis() - scanStartMs >= (uint32_t)(searchTimeout * 1000)) {
          /* No known network even visible within the search window → AP. */
          connectFails = 0;
          if (startAP()) { state = ST_AP; doUp(state); }
          else { state = ST_OFF; pmLockRelease(netDeepLock); }
        } else {
          delay(2000);
        }
        break;
      }
      case ST_STA_CONNECTED:
        netPollOnce();
        if (!staConnected) {
          info("disconnected, scanning...\n");
          fireEvent(NET_EV_DOWN);
          epCloseAll();
          esp_wifi_disconnect();
          /* Keep PM lock — still want_up, will reconnect */
          state = ST_SCANNING;
          scanStartMs = millis();
        }
        break;
      case ST_AP:
        netPollOnce();
        { uint32_t apRetryMs = (uint32_t)storageGetInt("s.net.wifi.ap.retry", 300) * 1000;
          if (staNetCount() > 0 && wantUp() && millis() - lastApRetryMs >= apRetryMs) {
            lastApRetryMs = millis();
            /* Non-disruptive scan: APSTA keeps AP running for connected clients */
            esp_wifi_set_mode(WIFI_MODE_APSTA);
            delay(100);
            int idx = scanForKnown();
            if (idx >= 0) {
              /* Found a known network — tear down AP and connect */
              fireEvent(NET_EV_DOWN);
              epCloseAll();
              if (connectSta(idx)) {
                state = ST_STA_CONNECTED;
                doUp(state);
              } else {
                startAP();
                doUp(state);
              }
            } else {
              /* Nothing found — back to pure AP, no disruption */
              esp_wifi_set_mode(WIFI_MODE_AP);
            }
          }
        }
        break;
    }
    wifiState = state;

    /* Sync upstream marker against the loop's resolved state. setUpstream is
     * idempotent, so this runs once per real transition and is a no-op
     * otherwise — saves having to instrument every state-change site. */
    setUpstream(state == ST_STA_CONNECTED);
  }
}

/* ---- ICMP ping (CLI) — raw socket on caller task; avoids esp_ping extra task (often ESP_ERR_NO_MEM / 257) ---- */

static int pingRecvEchoReply(int sock, uint16_t wantId, uint16_t wantSeq, uint8_t* ttlOut, uint32_t* payloadOut) {
    char buf[128];
    for (;;) {
        struct sockaddr_storage from{};
        socklen_t fromlen = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<struct sockaddr*>(&from), &fromlen);
        if (len <= 0) return len;
        if (from.ss_family != AF_INET) continue;
        if (len < (int)sizeof(struct ip_hdr)) continue;
        struct ip_hdr* iphdr = reinterpret_cast<struct ip_hdr*>(buf);
        int iphLen = IPH_HL_BYTES(iphdr);
        if (len < iphLen + (int)sizeof(struct icmp_echo_hdr)) continue;
        auto* iecho = reinterpret_cast<struct icmp_echo_hdr*>(buf + iphLen);
        if (iecho->type != ICMP_ER) continue;
        if (iecho->id != wantId || iecho->seqno != wantSeq) continue;
        *ttlOut = IPH_TTL(iphdr);
        *payloadOut = (uint32_t)(lwip_ntohs(IPH_LEN(iphdr)) - (uint16_t)iphLen - sizeof(struct icmp_echo_hdr));
        return len;
    }
}

static void pingCliCmd(const char* args) {
    if (cliWantsHelp(args)) {
        cliPrintf("%-*s ICMP echo (default target=router, count=4)\n", CLI_HELP_COL, "ping [ip] [count]");
        return;
    }
    if (!netIsUp()) {
        cliPrintf("ping: no network\n");
        return;
    }

    esp_netif_t* nif = (wifiState == ST_AP) ? ap_netif : sta_netif;
    if (!nif) {
        cliPrintf("ping: no interface\n");
        return;
    }

    ip_addr_t target{};
    uint32_t count = 4;

    while (*args == ' ' || *args == '\t') args++;
    if (!*args) {
        esp_netif_ip_info_t ipi{};
        if (esp_netif_get_ip_info(nif, &ipi) != ESP_OK || ipi.gw.addr == 0) {
            cliPrintf("ping: no gateway\n");
            return;
        }
        ip_addr_set_ip4_u32_val(target, ipi.gw.addr);
    } else {
        char host[48];
        const char* p = args;
        const char* q = p;
        while (*q && !std::isspace((unsigned char)*q)) q++;
        size_t len = (size_t)(q - p);
        if (len >= sizeof(host)) {
            cliPrintf("ping: address too long\n");
            return;
        }
        memcpy(host, p, len);
        host[len] = '\0';
        while (*q == ' ' || *q == '\t') q++;
        if (*q) {
            char* end = nullptr;
            unsigned long c = std::strtoul(q, &end, 10);
            if (end != q && c > 0 && c <= 32) count = (uint32_t)c;
            else cliPrintf("ping: bad count, using 4\n");
        }
        uint32_t raw = ipaddr_addr(host);
        if (raw == IPADDR_NONE || raw == 0) {
            cliPrintf("ping: invalid IPv4 address\n");
            return;
        }
        ip_addr_set_ip4_u32_val(target, raw);
    }

    const char* tstr = ipaddr_ntoa(&target);
    cliPrintf("PING %s (%s): 56 data bytes\n", tstr ? tstr : "?", tstr ? tstr : "?");

    constexpr uint32_t kDataSize = 56;
    constexpr uint32_t kIntervalMs = 1000;
    constexpr uint32_t kTimeoutMs = 2000;
    const size_t icmpTotal = sizeof(struct icmp_echo_hdr) + kDataSize;
    std::unique_ptr<uint8_t[]> pkt(new (std::nothrow) uint8_t[icmpTotal]);
    if (!pkt) {
        cliPrintf("ping: out of memory\n");
        return;
    }

    int sock = socket(AF_INET, SOCK_RAW, IP_PROTO_ICMP);
    if (sock < 0) {
        cliPrintf("ping: socket(AF_INET, SOCK_RAW) failed errno=%d\n", errno);
        return;
    }
    struct timeval tv;
    tv.tv_sec = kTimeoutMs / 1000;
    tv.tv_usec = (kTimeoutMs % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in to{};
    to.sin_family = AF_INET;
    inet_addr_from_ip4addr(&to.sin_addr, ip_2_ip4(&target));

    auto* echo = reinterpret_cast<struct icmp_echo_hdr*>(pkt.get());
    echo->type = ICMP_ECHO;
    echo->code = 0;
    echo->id = (uint16_t)(esp_random() & 0xFFFF);
    echo->seqno = 0;
    char* data = reinterpret_cast<char*>(pkt.get()) + sizeof(struct icmp_echo_hdr);
    for (uint32_t i = 0; i < kDataSize; i++) data[i] = static_cast<char>('A' + (i % 26));

    uint32_t tx = 0, rx = 0;
    struct timeval t0{}, t1{};
    gettimeofday(&t0, nullptr);

    for (uint32_t n = 0; n < count; n++) {
        echo->seqno = static_cast<uint16_t>(echo->seqno + 1);
        echo->chksum = 0;
        echo->chksum = inet_chksum(echo, icmpTotal);

        struct timeval ts{};
        gettimeofday(&ts, nullptr);
        ssize_t sent = sendto(sock, pkt.get(), icmpTotal, 0, reinterpret_cast<struct sockaddr*>(&to), sizeof(to));
        if (sent != (ssize_t)icmpTotal) {
            cliPrintf("ping: sendto failed errno=%d\n", errno);
            break;
        }
        tx++;

        uint8_t ttl = 0;
        uint32_t pay = 0;
        int r = pingRecvEchoReply(sock, echo->id, echo->seqno, &ttl, &pay);
        struct timeval te{};
        gettimeofday(&te, nullptr);
        uint32_t ms = (uint32_t)((te.tv_sec - ts.tv_sec) * 1000 + (te.tv_usec - ts.tv_usec) / 1000);

        if (r > 0) {
            rx++;
            cliPrintf("%u bytes from %s: icmp_seq=%u ttl=%u time=%ums\n",
                      (unsigned)(sizeof(struct icmp_echo_hdr) + pay), tstr ? tstr : "?",
                      (unsigned)echo->seqno, (unsigned)ttl, (unsigned)ms);
        } else {
            cliPrintf("Request timeout for icmp_seq %u\n", (unsigned)echo->seqno);
        }
        if (n + 1 < count) delay(kIntervalMs);
    }

    gettimeofday(&t1, nullptr);
    uint32_t dur = (uint32_t)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000);
    unsigned lossPct = tx ? (unsigned)((100U * (tx - rx)) / tx) : 0;
    cliPrintf("--- %u packets transmitted, %u received, %u%% packet loss, time %ums\n",
              (unsigned)tx, (unsigned)rx, lossPct, (unsigned)dur);
    close(sock);
}

/* ---- Public API ---- */

/* Find the index of a known STA network by SSID, or -1. */
static int staNetFindBySsid(const char* ssid) {
    int n = staNetCount();
    for (int i = 0; i < n; i++) {
        char s[33]; staNetGet(i, "ssid", s, sizeof(s));
        if (strcmp(s, ssid) == 0) return i;
    }
    return -1;
}

/* Tokenise `in` into outv[], in-place into `scratch` (NUL-separated). Tokens
 * are whitespace-delimited; "..."-quoted runs include literal whitespace and
 * are stored without the surrounding quotes. No escapes inside quotes.
 *
 * Returns:
 *    >= 0  number of tokens parsed (≤ maxOut)
 *      -1  bad quoting / scratch overflow
 *      -2  more than maxOut tokens present */
static int parseArgs(const char* in, char* scratch, size_t scratchLen,
                     char* outv[], int maxOut) {
    int argc = 0;
    char* w = scratch;
    char* end = scratch + scratchLen;
    for (;;) {
        while (*in == ' ' || *in == '\t') in++;
        if (!*in) return argc;
        if (argc >= maxOut) return -2;
        outv[argc++] = w;
        if (*in == '"') {
            in++;
            while (*in && *in != '"') {
                if (w >= end - 1) return -1;
                *w++ = *in++;
            }
            if (*in != '"') return -1;   /* unterminated */
            in++;
        } else {
            while (*in && *in != ' ' && *in != '\t') {
                if (w >= end - 1) return -1;
                *w++ = *in++;
            }
        }
        if (w >= end) return -1;
        *w++ = '\0';
    }
}

static void netCliCmd(const char* args) {
    if (strcmp(args, "help") == 0) { cliPrintf("%-*s WiFi status; list/up/down/add/join/delete\n", CLI_HELP_COL, "net [...]"); return; }
    if (cliWantsHelp(args)) {
        cliPrintf("%-*s WiFi control / status\n", CLI_HELP_COL, "net [up|down|down!]");
        cliPrintf("%-*s list stored WiFi networks\n", CLI_HELP_COL, "net list");
        cliPrintf("%-*s save a WiFi network (quote spaces)\n", CLI_HELP_COL, "net add <ssid> [pass]");
        cliPrintf("%-*s force-join a known network\n", CLI_HELP_COL, "net join <ssid>");
        cliPrintf("%-*s remove + disconnect\n",     CLI_HELP_COL, "net delete <ssid>");
        return;
    }
    if (strcmp(args, "up") == 0) { netUp(); return; }
    if (strcmp(args, "down!") == 0) { netDown(true); return; }
    if (strcmp(args, "down") == 0) { netDown(); return; }

    if (strcmp(args, "list") == 0) {
        int n = staNetCount();
        if (n == 0) { cliPrintf("no stored networks\n"); return; }
        /* Mark the one we're currently associated to, if any. */
        char cur[33] = "";
        if (wifiState == ST_STA_CONNECTED) {
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
                safeStrncpy(cur, (const char*)ap_info.ssid, sizeof(cur));
        }
        for (int i = 0; i < n; i++) {
            char ssid[33], pass[65];
            staNetGet(i, "ssid", ssid, sizeof(ssid));
            staNetGet(i, "pass", pass, sizeof(pass));
            cliPrintf("%s[%d] %s%s\n", (cur[0] && strcmp(cur, ssid) == 0) ? "* " : "  ",
                      i, ssid, pass[0] ? "" : " (open)");
        }
        return;
    }

    if (strncmp(args, "add ", 4) == 0 || strcmp(args, "add") == 0) {
        char scratch[160]; char* argv[2];
        int n = parseArgs(args + 3, scratch, sizeof(scratch), argv, 2);
        if (n == -1) { cliPrintf("bad quoting (use \"...\" for spaces)\n"); return; }
        if (n == -2) { cliPrintf("too many arguments — quote spaces with \"...\"\n"); return; }
        if (n < 1)   { cliPrintf("usage: net add <ssid> [<password>]\n"); return; }
        const char* ssid = argv[0];
        const char* pass = (n == 2) ? argv[1] : "";
        if (!*ssid) { cliPrintf("usage: net add <ssid> [<password>]\n"); return; }
        int idx = staNetFindBySsid(ssid);
        if (idx < 0) idx = staNetCount();
        char k[64];
        snprintf(k, sizeof(k), "s.net.wifi.nets.%d.ssid", idx); storageSet(k, ssid);
        snprintf(k, sizeof(k), "s.net.wifi.nets.%d.pass", idx); storageSet(k, pass);
        cliPrintf("saved '%s' at index %d%s\n", ssid, idx, *pass ? "" : " (open)");
        /* Join immediately when we're not already on a real station — i.e.
         * sitting on the built-in AP, scanning, or off. A freshly added
         * network should come up without a separate `net join`. An existing
         * STA connection is left undisturbed. */
        if (wifiState != ST_STA_CONNECTED) {
            cliPrintf("joining '%s'…\n", ssid);
            netDown(true);
            netUp();
        }
        return;
    }

    if (strncmp(args, "join ", 5) == 0 || strcmp(args, "join") == 0) {
        char scratch[80]; char* argv[1];
        int n = parseArgs(args + 4, scratch, sizeof(scratch), argv, 1);
        if (n == -1) { cliPrintf("bad quoting (use \"...\" for spaces)\n"); return; }
        if (n == -2) { cliPrintf("too many arguments — quote spaces with \"...\"\n"); return; }
        if (n != 1)  { cliPrintf("usage: net join <ssid>\n"); return; }
        const char* ssid = argv[0];
        int idx = staNetFindBySsid(ssid);
        if (idx < 0) {
            cliPrintf("unknown network '%s' — add it first with `net add`\n", ssid);
            return;
        }
        cliPrintf("joining '%s'…\n", ssid);
        netDown(true);
        netUp();
        return;
    }

    if (strncmp(args, "delete ", 7) == 0 || strcmp(args, "delete") == 0) {
        char scratch[80]; char* argv[1];
        int n = parseArgs(args + 6, scratch, sizeof(scratch), argv, 1);
        if (n == -1) { cliPrintf("bad quoting (use \"...\" for spaces)\n"); return; }
        if (n == -2) { cliPrintf("too many arguments — quote spaces with \"...\"\n"); return; }
        if (n != 1)  { cliPrintf("usage: net delete <ssid>\n"); return; }
        const char* ssid = argv[0];
        int idx = staNetFindBySsid(ssid);
        if (idx < 0) { cliPrintf("no such network: '%s'\n", ssid); return; }
        int total = staNetCount();
        /* Are we currently associated to it? If so, disconnect after removal. */
        bool wasJoined = false;
        if (wifiState != ST_OFF && wifiState != ST_AP) {
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK &&
                strcmp((const char*)ap_info.ssid, ssid) == 0) wasJoined = true;
        }
        staNetDeleteIdx(idx);   /* array-correct shift+drop (shared with wifi.cmd.del) */
        cliPrintf("removed '%s' (%d remain)\n", ssid, total - 1);
        if (wasJoined) {
            cliPrintf("disconnecting (was the current STA)…\n");
            netDown(true);
            netUp();
        }
        return;
    }

    if (*args) { cliPrintf("usage: net [up|down|down!|add|join|delete]\n"); return; }

    /* Show status — four states: down, connecting, up, going down */
    if (wifiState == ST_OFF) { cliPrintf("wifi: down\n"); return; }
    if (wifiState == ST_SCANNING) { cliPrintf("wifi: connecting\n"); return; }
    bool goingDown = !wantUp();

    uint32_t upSecs = (millis() - connectTimeMs) / 1000;
    char elapsed[32];
    fmtElapsed(upSecs, elapsed, sizeof(elapsed));

    if (wifiState == ST_AP) {
        cliPrintf("wifi: %s (AP) - %s\n\n", goingDown ? "going down" : "up", elapsed);
        char ssid[33];
        storageGetStr("s.net.wifi.ap.ssid", ssid, sizeof(ssid), WIFI_AP_SSID);
        esp_netif_ip_info_t ip_info;
        esp_netif_get_ip_info(ap_netif, &ip_info);
        char ip[16], mask[16];
        esp_ip4addr_ntoa(&ip_info.ip, ip, sizeof(ip));
        esp_ip4addr_ntoa(&ip_info.netmask, mask, sizeof(mask));
        cliPrintf("SSID:    %s\n", ssid);
        cliPrintf("IP:      %s\n", ip);
        cliPrintf("netmask: %s\n", mask);
    } else {
        cliPrintf("wifi: %s - %s\n\n", goingDown ? "going down" : "up", elapsed);
        wifi_ap_record_t ap_info;
        const char* ssid = "?";
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) ssid = (const char*)ap_info.ssid;
        esp_netif_ip_info_t ip_info;
        esp_netif_get_ip_info(sta_netif, &ip_info);
        char ip[16], gw[16], mask[16];
        esp_ip4addr_ntoa(&ip_info.ip, ip, sizeof(ip));
        esp_ip4addr_ntoa(&ip_info.gw, gw, sizeof(gw));
        esp_ip4addr_ntoa(&ip_info.netmask, mask, sizeof(mask));
        esp_netif_dns_info_t dns1 = {}, dns2 = {};
        char dns1s[16], dns2s[16];
        esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns1);
        esp_ip4addr_ntoa(&dns1.ip.u_addr.ip4, dns1s, sizeof(dns1s));
        esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_BACKUP, &dns2);
        esp_ip4addr_ntoa(&dns2.ip.u_addr.ip4, dns2s, sizeof(dns2s));
        cliPrintf("SSID:    %s\n", ssid);
        cliPrintf("IP:      %s\n", ip);
        cliPrintf("router:  %s\n", gw);
        cliPrintf("netmask: %s\n", mask);
        if (strcmp(dns2s, "0.0.0.0") != 0)
            cliPrintf("DNS:     %s, %s\n", dns1s, dns2s);
        else
            cliPrintf("DNS:     %s\n", dns1s);
    }
    char inBuf[16], outBuf[16];
    fmtSize(trafficIn, inBuf, sizeof(inBuf));
    fmtSize(trafficOut, outBuf, sizeof(outBuf));
    cliPrintf("traffic: in %s, out %s\n", inBuf, outBuf);
}

/* Module config version. Bump when adding/changing defaults. See duckdns.cpp.
 * net owns its sub-domains: s.net.{hostname,*_port,mdns,wifi.*,dns.*}. */
#define NET_VERSION 1

void netInit() {
  int v = storageGetInt("s.net.version", 0);
  if (v < NET_VERSION) {
    /* hostname and AP SSID interpolate CONFIG_SPANGAP_FW_HOSTNAME (the
     * straddle's default_hostname), so a fresh device seeds its mutable
     * hostname from firmware identity instead of hardcoded legacy strings. */
    storageDefaultTree("s.net", R"({
      "hostname": ")" CONFIG_SPANGAP_FW_HOSTNAME R"(",
      "http_port":   80,
      "https_port":  443,
      "rtsp_port":   554,
      "log_port":    0,
      "cli_port":    0,
      "webrtc_port": 4433,
      "mdns": { "http": 80, "https": 443 },
      "dns":  { "fqdn": "" },
      "wifi": {
        "enable": 1,
        "timeout": 20,
        "ap": {
          "pass": "",
          "ip":   "192.168.1.1",
          "mask": "255.255.255.0",
          "retry": 300
        },
        "nets": []
      }
    })");
    /* Per-device AP SSID: "<hostname>_<last 2 MAC bytes hex>" so a fleet of
     * units doesn't all present an identical AP. Computed from code here
     * (storageDefaultTree just defaulted hostname) and stored once;
     * user-editable afterwards and persisted. */
    {
      char host[32];
      storageGetStr("s.net.hostname", host, sizeof(host),
                    CONFIG_SPANGAP_PROJECT_NAME);
      uint8_t mac[6] = {};
      esp_efuse_mac_get_default(mac);
      char apssid[40];
      snprintf(apssid, sizeof(apssid), "%s_%02x%02x", host, mac[4], mac[5]);
      storageDefault("s.net.wifi.ap.ssid", apssid);
    }
    storageSet("s.net.version", NET_VERSION);
  }

  /* Suppress noisy WiFi driver block-ack renegotiation logs */
  esp_log_level_set("wifi", ESP_LOG_WARN);

  pmLockCreate(PM_NO_DEEP_SLEEP, "net", &netDeepLock);
  cliRegisterCmd("net", netCliCmd);
  cliRegisterCmd("ping", pingCliCmd);
  void wgetRegister();   /* wget.cpp — download CLI verb */
  wgetRegister();

  for (int i = 0; i < NET_MAX_CLIENTS; i++) {
    netClients[i].fd = -1;
    netClients[i].tlsConn = nullptr;
    netClients[i].itsHandle = -1;
  }
  /* Master switch s.net.wifi.enable (default 1). Cold boot: seed rtcWantUp from
   * it. Warm boot / deep-sleep wake: preserve the runtime state (rtcWantUp lives
   * in RTC RAM) EXCEPT enable=0 always forces down — an explicit "off" must
   * survive a reset, and the change handler may not have run this boot (e.g. the
   * value was set in a config/boot script before net subscribed). Previously
   * this only seeded on cold boot, so a warm boot kept the preserved rtcWantUp=1
   * and scanned + brought up AP despite enable=0. */
  bool wifiEnabled = storageGetInt("s.net.wifi.enable", 1) != 0;
  if (!rtcRamValid())    rtcWantUp = wifiEnabled;
  else if (!wifiEnabled) rtcWantUp = false;

  readySem = xSemaphoreCreateBinary();
  wifiConnectedSem = xSemaphoreCreateBinary();
  netHandle = spawnTask(netTaskFn, "net", 8192, nullptr, 2, 0);
  xSemaphoreTake(readySem, portMAX_DELAY);  /* wait until task is running */
}

void netUp() {
  /* Master switch chokepoint: enable=0 means the radio stays off, period — every
   * caller (the wifi.enable change handler, `net up`, add-network helpers) is a
   * no-op while disabled. This is what stops a spurious/duplicate bring-up (the
   * cmdUp path doesn't consult wantUp()) from overriding an explicit disable. */
  if (storageGetInt("s.net.wifi.enable", 1) == 0) {
    info("netUp() ignored — wifi.enable=0\n");
    return;
  }
  rtcWantUp = true;
  if (!netHandle) return;  /* net task will pick up rtcWantUp on start */
  uint8_t cmd = NET_CMD_UP;
  itsSendAuxByTaskHandle(netHandle, NET_CMD_PORT, &cmd, 1, pdMS_TO_TICKS(100));
}

void netDown(bool force) {
  rtcWantUp = false;
  if (!netHandle) return;
  uint8_t cmd = force ? NET_CMD_FORCE_DOWN : NET_CMD_DOWN;
  itsSendAuxByTaskHandle(netHandle, NET_CMD_PORT, &cmd, 1, pdMS_TO_TICKS(100));
}

bool netIsUp() {
  return wifiState == ST_STA_CONNECTED || wifiState == ST_AP;
}

bool netIsStaConnected() {
  return wifiState == ST_STA_CONNECTED;
}

void netActivity() {
  lastActivityMs = millis();
}

void netTrafficIn(uint32_t bytes) { trafficIn += bytes; }
void netTrafficOut(uint32_t bytes) { trafficOut += bytes; }

void netGetLocalIp(char* out, size_t len) {
  if (!sta_netif || !staConnected) { out[0] = '\0'; return; }
  esp_netif_ip_info_t ip_info;
  esp_netif_get_ip_info(sta_netif, &ip_info);
  esp_ip4addr_ntoa(&ip_info.ip, out, len);
}

/* ---- TCP connection control ---- */

void netForceClose(int itsHandle) {
    net_client_t* c = netFindClient(itsHandle);
    if (!c) return;
    struct linger lg = { 1, 0 };
    setsockopt(c->fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    netClientClose(*c);
}

