/**
 * Net — WiFi + TCP call center.
 * Owns WiFi state and all TCP server sockets.
 * ITS client: connects to server tasks on behalf of TCP/TLS clients.
 * Event-driven: modules register callbacks via netRegister().
 */
#ifndef SECCAM_NET_H
#define SECCAM_NET_H

#include <stddef.h>
#include <lwip/ip_addr.h>
#include <lwip/sockets.h>

/* Compiler-define fallbacks for AP mode (survive factory reset).
 * AP SSID defaults to the project name (CONFIG_DIPTYCH_PROJECT_NAME) so a
 * fresh device on a new project doesn't broadcast a stale SSID from the
 * previously-flashed project. */
#ifndef WIFI_AP_SSID
#define WIFI_AP_SSID CONFIG_DIPTYCH_PROJECT_NAME
#endif
#ifndef WIFI_AP_PASS
#define WIFI_AP_PASS ""
#endif
#ifndef WIFI_AP_IP
#define WIFI_AP_IP "192.168.1.1"
#endif
#ifndef WIFI_AP_MASK
#define WIFI_AP_MASK "255.255.255.0"
#endif

/* ---- Event system ---- */

enum {
    NET_EV_UP,            /* WiFi connected (STA got IP or AP started). arg = NULL. */
    NET_EV_DOWN,          /* WiFi going down. arg = NULL */
    NET_EV_UPSTREAM_UP,   /* STA connected to a real upstream network — internet
                             reachable. AP-only does NOT fire this event. arg = NULL.
                             Modules that need internet (duckdns, ntp, wg, upnp,
                             cloud HTTP clients) should use this instead of UP.
                             For storage subscribers, equivalent to wifi.sta.up. */
    NET_EV_UPSTREAM_DOWN, /* STA leaving connected state. arg = NULL. Fires on
                             disconnect, reconnect-loop, or transition to AP-only. */
    NET_EV_CFG_CHANGED,   /* config key changed. arg = key name */
    NET_EV_POLL,          /* periodic (~10ms when connected). arg = NULL */
    NET_EV_COUNT
};

typedef void (*net_event_cb_t)(const char* arg);

/** Register a callback for a network event. Called from main at init.
 *  Multiple callbacks per event OK. Never unregistered. */
void netRegister(int event, net_event_cb_t cb);

/* ---- Lifecycle ---- */

/** Create net task. Blocks until task is running (not until WiFi connected). */
void netInit();

/** Bring WiFi up (scan/connect). */
void netUp();

/** Bring WiFi down. force=false waits for 30s idle, force=true is immediate. */
void netDown(bool force = false);

/** Returns true if WiFi is connected (STA or AP). */
bool netIsUp();

/** Returns true only when connected to an upstream network as STA.
 *  AP-only mode returns false (no upstream). */
bool netIsStaConnected();

/** Signal network activity (resets idle timer for graceful shutdown). */
void netActivity();

/** Accumulate traffic counters (bytes). Only for tasks with their own sockets
 *  (e.g. webrtc UDP) — TCP traffic through net's proxy is counted automatically. */
void netTrafficIn(uint32_t bytes);
void netTrafficOut(uint32_t bytes);

/** Get the local STA IP address. Returns "" if not connected. */
void netGetLocalIp(char* out, size_t len);

/* ---- ITS aux message: task → net TCP port registration ---- */

/** Net's aux ports. */
static constexpr uint16_t NET_PORT_REG_PORT = 0;   /* tasks register TCP endpoints */
static constexpr uint16_t NET_CMD_PORT      = 1;   /* CLI control: up/down */

/** Outbound TCP dial. Connect to net at this port with a "host:port" ASCII
 *  payload (NUL-terminated, ≤95 bytes). Net does DNS + connect on its own
 *  task and accepts the ITS connection on success. The accepted ITS handle
 *  IS the TCP byte stream from byte zero; net proxies bytes both ways
 *  using its existing select loop, same as inbound TCP. Closing the ITS
 *  handle closes the socket.
 *
 *  Stream-mode (NOT packet-mode): TCP is a byte stream. Callers needing
 *  framing (HDLC, length-prefix, line-delimited) layer it on top. */
static constexpr uint16_t NET_PORT_TCP_DIAL = 2;

/** Tasks send this to "net" on NET_PORT_REG_PORT via itsSendAux to register
 *  a TCP endpoint. Net opens a server socket, accepts connections, and
 *  connects to the registering task via ITS with the given itsPort on
 *  each client. */
typedef struct {
    uint16_t itsPort;     /* ITS port number (passed to onConnect) */
    uint16_t tcpPort;     /* TCP listen port (0 = use nvsKey/defaultPort) */
    uint8_t tls;          /* 1 = TLS termination on this port */
    uint8_t tcpNoDelay;   /* 1 = TCP_NODELAY (default 1) */
    uint8_t keepAlive;    /* 1 = SO_KEEPALIVE */
    uint8_t backlog;      /* listen backlog (0 = default 4) */
    char nvsKey[16];      /* config key for dynamic port (e.g. "s.net.rtsp_port") */
    int  defaultPort;     /* default if config key missing */
} net_port_msg_t;

/* ---- ITS connect payload: net/web → task ---- */

/** Sent as connect data for both TCP (from net) and WS (forwarded by web).
 *  Tasks check `ws` to determine connection type. */
typedef struct {
    uint8_t ws;               /* 1 = WebSocket (forwarded from web), 0 = raw TCP */
    uint8_t tls;              /* 1 = behind TLS */
    ip_addr_t clientAddr;     /* client's IP address */
} net_connect_t;

/** Force-close the TCP connection (RST). */
void netForceClose(int itsHandle);

/* ---- UDP socket management ---- */

#endif
