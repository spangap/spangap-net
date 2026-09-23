/**
 * net_relay — the event bus and the TCP call center.
 *
 * Everything here is about sockets and nothing about how the device got an
 * address: the event registry modules subscribe to, the endpoint table tasks
 * register their ports in, the listen sockets, the one select() that accepts
 * and proxies bytes between those sockets and ITS, and the outbound dial a
 * task asks for on its own behalf.
 *
 * A link backend owns the loop and calls in here — WiFi on a chip (net.cpp),
 * the host's loopback on a station process (host/net_host.cpp). The shared
 * surface is net_priv.h.
 */
#include "net_priv.h"

#include "spangap.h"
#include "mem.h"
#include "storage.h"
#include "compat.h"
#include "pm.h"
#include "log.h"
#include "cli.h"
#include "fs.h"
#include "its.h"
#include "tls.h"
#include "auth.h"

#include "esp_heap_caps.h"
#include "esp_mac.h"

#include <fcntl.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <freertos/semphr.h>

#if !CONFIG_IDF_TARGET_LINUX
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#endif

/* ---- Shared state ---- */

uint32_t netTrafficInBytes = 0, netTrafficOutBytes = 0;
volatile uint32_t netLastActivityMs = 0;
uint32_t netBindAddrV4 = INADDR_ANY;

/* ---- Event callback registry ---- */

#define NET_MAX_CBS 8

static struct {
    net_event_cb_t cbs[NET_MAX_CBS];
    int count;
} evRegistry[NET_EV_COUNT] = {};

/* Current link state, so a handler that registers AFTER the backend has
 * already brought the link up still gets the UP edge — registration order
 * vs. bring-up must not matter (this is what made mDNS flaky on fresh
 * AP-only boots: net task fired NET_EV_UP before mdnsInit registered). */
bool netLinkUp   = false;  /* true between the backend's up and its down */
bool netUpstreamUp = false;  /* true iff a real upstream is reachable    */

void netRegister(int event, net_event_cb_t cb) {
    if (event < 0 || event >= NET_EV_COUNT) return;
    auto& r = evRegistry[event];
    if (r.count < NET_MAX_CBS) r.cbs[r.count++] = cb;
    /* Level-replay the UP edges to a late subscriber. DOWN/CFG/POLL stay
     * edge-only (replaying a teardown to a handler that never set up would
     * be wrong). Note: cb may run synchronously here, on the caller's task
     * rather than the net task — UP handlers must be idempotent. */
    if      (event == NET_EV_UP          && netLinkUp)     cb(nullptr);
    else if (event == NET_EV_UPSTREAM_UP && netUpstreamUp) cb(nullptr);
}

void fireEvent(int event, const char* arg) {
    auto& r = evRegistry[event];
    for (int i = 0; i < r.count; i++) r.cbs[i](arg);
}

/** Sync upstream state (NET_EV_UPSTREAM_UP/DOWN events) to whether a real
 *  upstream is reachable. Idempotent — only fires on real transitions. */
void setUpstream(bool up) {
  if (netUpstreamUp == up) return;
  netUpstreamUp = up;
  /* Ephemeral readiness flag the rns boot barrier waits on (only when WiFi is
   * configured — see net.want). Decoupled by storage key so rns has no net
   * dependency and net-less builds simply never set it. */
  storageSet("net.up", up ? 1 : 0);
  if (up) signalFlag("net.up");   /* wake the rns boot barrier blocked in waitForFlag */
  fireEvent(up ? NET_EV_UPSTREAM_UP : NET_EV_UPSTREAM_DOWN);
}

/* ---- TCP call center: endpoint table + client proxy ---- */

/* NET_MAX_ENDPOINTS is net.h's — a caller of netPublicPorts() sizes its array
 * from it. */
#define NET_MAX_CLIENTS   16

struct net_endpoint_t {
    TaskHandle_t task;
    uint16_t itsPort;
    int serverFd;
    int port;         /* currently open port */
    char nvsKey[16];
    int defaultPort;
    bool ownPort;     /* registrant manages the port: bind `port` directly (0 = closed),
                       * never consult s.net.<nvsKey>. See net_port_msg_t.ownPort. */
    int fixedPort;    /* desired port when ownPort — re-registration updates it */
    bool publicFacing; /* registrant wants this port reachable from the internet */
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
    /* ITS→socket staging (PSRAM, 4096, allocated once at init). Bytes
     * consumed from ITS but not yet accepted by the kernel wait here
     * across loop passes; [txOff, txLen) is the unsent remainder. The fd
     * joins the select() write set only while data is pending, so a peer
     * with a full send buffer parks its remainder instead of stalling
     * the net task. */
    uint8_t* txBuf;
    size_t   txLen;
    size_t   txOff;
};

static net_endpoint_t netEps[NET_MAX_ENDPOINTS];
static int netEpCount = 0;
/* Set wherever the public-facing set moves — a socket opening or closing, a
 * registrant flipping its flag — and drained in epOpenAll, so the event fires
 * once per poll pass and never from inside the walk over netEps. */
static bool netPortsDirty = false;
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
                       int defaultPort, bool ownPort, int fixedPort,
                       bool publicFacing, bool tls, bool keepAlive, int backlog) {
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
    /* Re-registration with a new fixedPort is how an ownPort registrant changes
     * or closes its listen socket at runtime: epOpenPort (run every poll by
     * epOpenAll) sees the resolved port move and rebinds/closes. */
    ep->ownPort = ownPort;
    ep->fixedPort = fixedPort;
    /* Re-registration is also how the flag moves — a listener the operator has
     * just published to the internet, or withdrawn from it. */
    if (ep->publicFacing != publicFacing) {
        ep->publicFacing = publicFacing;
        netPortsDirty = true;
    }
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
               msg->ownPort != 0, msg->tcpPort, msg->publicFacing != 0,
               msg->tls, msg->keepAlive, msg->backlog);
}

/* Expose the core platform services (CLI + log) over TCP. They live in
 * spangap-core and know nothing about net; net resolves their tasks by name
 * (ITS names them "cli"/"log") and registers their endpoints. Both default to
 * port 0 (off) in net's config tree — exposed only when the user sets
 * s.net.cli_port / s.net.log_port. */
void netRegisterCorePorts() {
    epRegister(xTaskGetHandle("cli"), CLI_PORT_TCP, "cli_port", 0,
               /*ownPort=*/false, /*fixedPort=*/0, /*publicFacing=*/false,
               /*tls=*/false, /*keepAlive=*/false, /*backlog=*/0);
    epRegister(xTaskGetHandle("log"), LOG_PORT_TCP, "log_port", 0,
               /*ownPort=*/false, /*fixedPort=*/0, /*publicFacing=*/false,
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
    /* An ownPort registrant (e.g. the TCP inbound server, whose port lives in
     * s.tcp.server_port, not s.net.*) binds fixedPort directly — 0 closes the
     * socket, tracking the service's enabled state. Config-driven endpoints
     * (cli/log/http/…) resolve via s.net.<nvsKey>. */
    int newPort = ep.ownPort ? ep.fixedPort : storageGetInt(fullKey, ep.defaultPort);
    if (newPort == ep.port && (newPort <= 0 || ep.serverFd >= 0)) return;
    if (ep.serverFd >= 0) {
        info("closing port %d (%s)\n", ep.port, ep.nvsKey);
        close(ep.serverFd);
        ep.serverFd = -1;
        if (ep.publicFacing) netPortsDirty = true;
    }
    ep.port = newPort;
    if (newPort <= 0) return;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { err("port %d open failed\n", newPort); return; }
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = netBindAddrV4;
    addr.sin_port = htons(newPort);
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(s, ep.backlog) < 0) {
        err("port %d bind/listen failed\n", newPort);
        close(s); return;
    }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    ep.serverFd = s;
    if (ep.publicFacing) netPortsDirty = true;
    info("opening port %d (%s)\n", newPort, ep.nvsKey);
}

void epOpenAll() {
    for (int i = 0; i < netEpCount; i++) epOpenPort(netEps[i]);
    /* Fired after the walk, not inside it: a handler is free to register an
     * endpoint of its own, and that would move netEps under the loop. */
    if (netPortsDirty) {
        netPortsDirty = false;
        fireEvent(NET_EV_PORTS_CHANGED);
    }
}

int netPublicPorts(net_public_port_t* out, int max) {
    int n = 0;
    for (int i = 0; i < netEpCount && n < max; i++) {
        const net_endpoint_t& ep = netEps[i];
        if (!ep.publicFacing || ep.serverFd < 0 || ep.port <= 0) continue;
        out[n].port = (uint16_t)ep.port;
        safeStrncpy(out[n].nvsKey, ep.nvsKey, sizeof(out[n].nvsKey));
        n++;
    }
    return n;
}

static void netClientClose(net_client_t& c) {
    if (c.itsHandle >= 0) { itsDisconnect(c.itsHandle); c.itsHandle = -1; }
    if (c.tlsConn) tlsClose(c.tlsConn);
    else if (c.fd >= 0) close(c.fd);
    c.fd = -1;
    c.tlsConn = nullptr;
    c.txLen = 0;
    c.txOff = 0;
}

void epCloseAll() {
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
    /* 1 s, not 100 ms: the backend (e.g. [web]) shares core 1 with storage and
     * lxmf, so an inbound-message burst can keep it off-CPU for the better part
     * of a second. A 100 ms ack window turned that transient into a dropped
     * browser connection; 1 s rides it out while still failing fast on a truly
     * dead backend. */
    int h = itsConnectByTaskHandle(ep.task, ep.itsPort, &cd, sizeof(cd),
                                    pdMS_TO_TICKS(1000), -1, nullptr, netItsDisconnect);
    if (h < 0) { if (conn) tlsClose(conn); else close(fd); return; }
    /* Field-wise, NOT aggregate: an aggregate assignment would zero txBuf,
     * the staging buffer allocated once at task start. */
    net_client_t& nc = netClients[ci];
    nc.fd = fd; nc.tlsConn = conn; nc.itsHandle = h;
    nc.epIdx = ei; nc.serverTask = ep.task;
    nc.txLen = 0; nc.txOff = 0;
}

/* Unified select on all server fds + client fds, then accept + proxy */
void netPollOnce() {
    while (itsPoll(0)) {}
    epOpenAll();

    fd_set rfds, wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    int maxFd = -1;

    for (int i = 0; i < netEpCount; i++) {
        int sfd = netEps[i].serverFd;
        if (sfd >= 0) { FD_SET(sfd, &rfds); if (sfd > maxFd) maxFd = sfd; }
    }
    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        auto& c = netClients[i];
        if (c.fd < 0) continue;
        /* Write set: only while there is something to push (a parked
         * remainder or fresh ITS bytes) — a bare TCP socket is almost
         * always writable, so including it unconditionally would make
         * select() return instantly and spin the task. */
        if (c.txLen > c.txOff ||
            (c.itsHandle >= 0 && itsBytesAvailable(c.itsHandle) > 0)) {
            FD_SET(c.fd, &wfds);
            if (c.fd > maxFd) maxFd = c.fd;
        }
        /* Read set — backpressure: when the ITS stream toward the owning
         * task is full (owner busy and not draining), leave the socket
         * unread — the kernel buffer fills and TCP flow control throttles
         * the remote sender. Previously the bytes were read anyway and
         * handed to itsSend(…, 0) with the result ignored, silently
         * discarding up-to-4 KB chunks mid-stream and corrupting the
         * owner's framing. Skipping FD_SET (rather than skipping just the
         * recv) keeps select() from returning instantly on the unread fd
         * and spinning the task hot for the whole stall. */
        if (c.itsHandle >= 0 && itsSpacesAvailable(c.itsHandle) == 0) continue;
        FD_SET(c.fd, &rfds);
        if (c.fd > maxFd) maxFd = c.fd;
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
        select(maxFd + 1, &rfds, &wfds, NULL, &tv);
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
        /* Clamp the read to what the ITS stream can absorb right now, so the
         * itsSend below can never overflow-drop (see the FD_SET backpressure
         * note above; this covers the tlsBytesAvail path and space shrinking
         * between the FD_SET pass and here). */
        size_t space = itsSpacesAvailable(c.itsHandle);
        /* A peer that has hung up is gone whether or not we have anywhere to
         * put its bytes. Reading is what discovers that, and a backpressured
         * socket is deliberately kept out of the read set — so without this it
         * is never discovered at all: the socket sits in CLOSE-WAIT holding a
         * client slot for as long as the owning task stays backed up, and
         * enough of those stop the endpoint accepting anyone. Hence a probe of
         * its own, off the read set, on the select timeout's beat. It consumes
         * nothing, so the data still arrives in order once there is room. */
        if (space == 0 && !c.tlsConn) {
            char probe;
            int n = recv(c.fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                netClientClose(c);
                continue;
            }
        }
        if (canRecv && space > 0) {
            size_t want = space < 4096 ? space : 4096;   /* netProxyBuf size */
            int n = c.tlsConn ? tlsRead(c.tlsConn, netProxyBuf, want)
                              : recv(c.fd, netProxyBuf, want, MSG_DONTWAIT);
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
            if (n > 0) { itsSend(c.itsHandle, netProxyBuf, n, 0); netActivity(); netTrafficInBytes += n; }
        }

        /* ITS → socket, gated on write-readiness so one peer's full send
         * buffer never stalls the whole net task. A would-block mid-chunk
         * parks the remainder in c.txBuf ([txOff, txLen)); the fd stays in
         * the select() write set while anything is pending, so we resume
         * the moment the kernel drains. tlsWrite maps WANT_READ/WANT_WRITE
         * to 0, raw send gives EAGAIN — neither is a dead peer. */
        if (FD_ISSET(c.fd, &wfds) && c.txBuf) {
            for (int rounds = 0; rounds < 4; rounds++) {
                if (c.txOff >= c.txLen) {
                    c.txOff = c.txLen = 0;
                    size_t n = itsRecv(c.itsHandle, c.txBuf, 4096, 0);
                    if (n == 0) break;
                    c.txLen = n;
                    netTrafficOutBytes += n;
                }
                while (c.txOff < c.txLen) {
                    int sent = c.tlsConn
                        ? tlsWrite(c.tlsConn, c.txBuf + c.txOff, c.txLen - c.txOff)
                        : send(c.fd, c.txBuf + c.txOff, c.txLen - c.txOff, MSG_DONTWAIT);
                    bool wouldBlock = c.tlsConn
                        ? (sent == 0)
                        : (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
                    if (wouldBlock) goto nextClient;   /* parked; wfds re-arms */
                    if (sent <= 0) { netClientClose(c); goto nextClient; }
                    c.txOff += sent;
                }
                netActivity();
            }
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

    /* Field-wise for the same reason as netAcceptOne: keep txBuf. */
    net_client_t& nc = netClients[ci];
    nc.fd = fd; nc.tlsConn = nullptr; nc.itsHandle = handle;
    nc.epIdx = -1; nc.serverTask = nullptr;
    nc.txLen = 0; nc.txOff = 0;
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

/* ---- Task-context setup ---- */

void netRelayTaskInit(void) {
  /* Allocate the proxy buffer in task context so heap tracking attributes it
     to net, not the main task that spawned us. */
  netProxyBuf = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
  /* Per-client ITS→socket staging buffers (see net_client_t::txBuf). */
  for (int i = 0; i < NET_MAX_CLIENTS; i++)
      netClients[i].txBuf = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
  itsClientInit(NET_MAX_CLIENTS);
  itsServerInit();
  itsServerPortOpen(NET_PORT_TCP_DIAL, /*packetBased=*/false,
                    /*maxHandles=*/NET_MAX_CLIENTS,
                    /*toSize=*/4096, /*fromSize=*/4096);
  itsServerOnConnect(NET_PORT_TCP_DIAL, netOnDialConnect);
  itsServerOnDisconnect(NET_PORT_TCP_DIAL, netOnDialDisconnect);
  itsOnAux(NET_PORT_REG_PORT, netOnAux);

  /* Run /state/net_up on every upstream-up — net policy, not the consumer's. */
  netRegister(NET_EV_UPSTREAM_UP, netOnUpstreamUp);
}

/* ---- The device's name ---- */

int netParseArgs(const char* in, char* scratch, size_t scratchLen,
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

/* The hostname is the device's name wherever it is asked for one, and none of
 * it is WiFi's: it is a setting, and every link backend carries it. */
static void hostnameCliCmd(const char* args) {
    if (cliWantsHelp(args)) {
        cliPrintf("%-*s show or set the device hostname\n", CLI_HELP_COL, "hostname [<name>]");
        return;
    }
    if (!*args) {
        char host[32];
        storageGetStr("s.net.hostname", host, sizeof(host), "");
        cliPrintf("%s\n", host);
        return;
    }
    char scratch[64]; char* argv[1];
    int n = netParseArgs(args, scratch, sizeof(scratch), argv, 1);
    if (n == -1) { cliPrintf("bad quoting (use \"...\" for spaces)\n"); return; }
    if (n == -2 || n != 1) { cliPrintf("usage: hostname [<name>]\n"); return; }
    /* Hostnames are DNS/mDNS labels: letters, digits, underscore only. */
    for (const char* p = argv[0]; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '_')) {
            cliPrintf("invalid hostname '%s' — use letters, digits, and _ only\n", argv[0]);
            return;
        }
    storageSet("s.net.hostname", argv[0]);
    cliPrintf("hostname set to '%s' (applies on next reconnect)\n", argv[0]);
}

/* ---- Bring-up shared by every backend ---- */

#define NET_VERSION 2

void netInitCommon() {
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

  /* Surface a passwordless device once at boot, for whoever is watching the
   * log. The admin realm ships unset until a password is chosen. Setup tooling
   * does not read this — it asks `auth -O`. */
  if (authRealmUnset("admin")) info("No device password set\n");

  cliRegisterCmd("hostname", hostnameCliCmd);

  for (int i = 0; i < NET_MAX_CLIENTS; i++) {
    netClients[i].fd = -1;
    netClients[i].tlsConn = nullptr;
    netClients[i].itsHandle = -1;
  }
}

/* ---- Public API ---- */

void netActivity() {
  netLastActivityMs = millis();
}

void netTrafficIn(uint32_t bytes) { netTrafficInBytes += bytes; }
void netTrafficOut(uint32_t bytes) { netTrafficOutBytes += bytes; }

/* ---- TCP connection control ---- */

void netForceClose(int itsHandle) {
    net_client_t* c = netFindClient(itsHandle);
    if (!c) return;
    struct linger lg = { 1, 0 };
    setsockopt(c->fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    netClientClose(*c);
}
