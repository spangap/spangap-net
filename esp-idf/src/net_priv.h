/**
 * net_priv.h — what the two halves of this straddle share.
 *
 * The relay (net_relay.cpp) is the part that has nothing to do with radios:
 * the event bus, the endpoint table, the listen sockets, the accept-and-proxy
 * loop and the outbound dial. A link backend drives it — WiFi on a chip
 * (net.cpp), the host's own loopback on a station process (host/net_host.cpp)
 * — and the two meet here.
 */
#pragma once

#include "net.h"

#include <stdint.h>

/* ---- State the relay publishes and a backend sets ---- */

/** True between the backend's bring-up and its teardown. netRegister() replays
 *  the UP edge to a late subscriber from this. */
extern bool netLinkUp;

/** True only while a real upstream is reachable. Same replay reason. */
extern bool netUpstreamUp;

/** Bytes through the relay's proxy since the last bring-up, and the millisecond
 *  stamp of the last byte either way — what a backend's idle timeout reads. */
extern uint32_t netTrafficInBytes, netTrafficOutBytes;
extern volatile uint32_t netLastActivityMs;

/** The address every listen socket binds. INADDR_ANY on a chip, where the one
 *  device owns its addresses; one station's own loopback address on a host,
 *  where several of them share the machine and the port numbers. */
extern uint32_t netBindAddrV4;

/* ---- The relay ---- */

/** Deliver an event to everything registered for it. */
void fireEvent(int event, const char* arg = nullptr);

/** Move the upstream marker, publishing `net.up` and firing the edge. */
void setUpstream(bool up);

/** Register the always-present core services (CLI, log) on their tasks' behalf. */
void netRegisterCorePorts(void);

/** Open every endpoint whose resolved port has moved, close those that went to
 *  zero, and fire NET_EV_PORTS_CHANGED once if the public set changed. */
void epOpenAll(void);

/** Drop every client and every listen socket. */
void epCloseAll(void);

/** One pass of the relay: poll ITS, refresh the endpoints, select, accept,
 *  proxy both ways, fire NET_EV_POLL. Blocks for at most ten milliseconds. */
void netPollOnce(void);

/** The relay's own task-context setup: proxy buffers, the ITS client and
 *  server, the dial port and the registration aux port. Called once from the
 *  backend's task before its loop. */
void netRelayTaskInit(void);

/** The part of bring-up that is neither radio nor host: the s.net config tree,
 *  the per-device AP SSID, the client table, the `hostname` command and the
 *  passwordless-device note. */
void netInitCommon(void);

/** Split a CLI argument string into at most `maxOut` words, honouring "quotes",
 *  into the caller's scratch buffer. Returns the count, -1 on bad quoting or a
 *  full buffer, -2 on more words than asked for. */
int netParseArgs(const char* in, char* scratch, size_t scratchLen,
                 char* outv[], int maxOut);
