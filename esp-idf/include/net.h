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

/* Compiler-define fallbacks for AP mode (survive factory reset) */
#ifndef WIFI_AP_SSID
#define WIFI_AP_SSID "SECCAM"
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
    NET_EV_UP,           /* WiFi connected (STA got IP or AP started). arg = NULL */
    NET_EV_DOWN,         /* WiFi going down. arg = NULL */
    NET_EV_CFG_CHANGED,  /* config key changed. arg = key name */
    NET_EV_POLL,         /* periodic (~10ms when connected). arg = NULL */
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

/** Signal network activity (resets idle timer for graceful shutdown). */
void netActivity();

/** Get the local STA IP address. Returns "" if not connected. */
void netGetLocalIp(char* out, size_t len);

/* ---- ITS aux message: task → net TCP port registration ---- */

/** Tasks send this to "net" via itsSendAux to register a TCP endpoint.
 *  Net opens a server socket, accepts connections, and connects to the
 *  registering task via ITS with the given itsPort on each client. */
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

/** Incoming UDP packet notification delivered via ITS aux on the UDP port number.
 *  Packet data is in a shared ring buffer — read it in the handler, it may be
 *  overwritten by the next packet. */
#define NET_UDP_RING_SIZE  8
#define NET_UDP_MTU        1500

typedef struct {
    struct sockaddr_in from;  /* source address */
    uint16_t len;             /* packet length */
    uint8_t  slot;            /* ring buffer slot index */
} net_udp_packet_t;

/** Ring buffer for incoming UDP packets (read-only for consumers). */
extern uint8_t netUdpRing[NET_UDP_RING_SIZE][NET_UDP_MTU];

/** Create+bind a UDP socket, ask net to monitor it for incoming packets.
 *  Incoming packets delivered as ITS aux (net_udp_packet_t) on the same
 *  port number. Caller uses the returned fd for sendto() directly.
 *  Returns fd, or -1 on failure. */
int netUdpListen(uint16_t port);

/** Stop monitoring and close a UDP socket. */
void netUdpClose(int fd);

#endif
