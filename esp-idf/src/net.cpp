/**
 * Net — WiFi state machine + TCP call center + event dispatch.
 *
 * WiFi: scan/connect/AP state machine (wifi1–wifi9 STA, wifi0 AP fallback).
 * TCP:  owns all server sockets, proxies bytes between sockets and ITS.
 * Events: modules register callbacks via netRegister() for UP/DOWN/CFG/POLL.
 */
#include "net.h"
#include "storage.h"
#include "compat.h"
#include "nvs_config.h"
#include "pm.h"
#include "log.h"
#include "cli.h"
#include "its.h"
#include "tls.h"
#include <lwip/sockets.h>
#include <fcntl.h>
#include "esp_wifi.h"
#include "esp_sleep.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "lwip/ip4_addr.h"
#include <cstring>
#include <cstdio>
#include "esp_heap_caps.h"

#define MAX_STA_NETWORKS 9

static TaskHandle_t netHandle = nullptr;
static SemaphoreHandle_t readySem = nullptr;

/* esp_netif handles */
static esp_netif_t* sta_netif = nullptr;
static esp_netif_t* ap_netif = nullptr;

/* Event-driven connection signaling */
static SemaphoreHandle_t wifiConnectedSem = nullptr;
static volatile bool staConnected = false;
static volatile bool wantDown = false;
static volatile bool cmdUp = false;
static volatile bool cmdForceDown = false;
static volatile uint32_t lastActivityMs = 0;
#define WIFI_IDLE_TIMEOUT_MS 30000

/* ITS aux command interface */
enum { NET_CMD_UP = 1, NET_CMD_DOWN, NET_CMD_FORCE_DOWN };
#define NET_CMD_PORT 1

/* Known STA networks loaded from config (s.wifi.1..9) */
static struct {
  char ssid[33];
  char pass[65];
  char ip[16], gw[16], mask[16], dns[16];
} staNet[MAX_STA_NETWORKS];
static int staCount = 0;

/* RTC state: survives deep sleep so wifi stays down after cron "net down" */
RTC_DATA_ATTR static bool rtcWifiUp = true;

static pm_lock_handle_t netDeepLock = nullptr;

/* ---- Event callback registry ---- */

#define NET_MAX_CBS 8

static struct {
    net_event_cb_t cbs[NET_MAX_CBS];
    int count;
} evRegistry[NET_EV_COUNT] = {};

void netRegister(int event, net_event_cb_t cb) {
    if (event < 0 || event >= NET_EV_COUNT) return;
    auto& r = evRegistry[event];
    if (r.count < NET_MAX_CBS) r.cbs[r.count++] = cb;
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

/* ITS aux callback: tasks register TCP endpoints */
static void netOnAux(TaskHandle_t sender, uint16_t port, const void* data, size_t len) {
    if (len < sizeof(net_port_msg_t)) return;
    auto* msg = (const net_port_msg_t*)data;
    net_endpoint_t* ep = epFindByKey(msg->nvsKey);
    if (!ep) {
        if (netEpCount >= NET_MAX_ENDPOINTS) return;
        ep = &netEps[netEpCount++];
        memset(ep, 0, sizeof(*ep));
        ep->serverFd = -1;
        ep->port = 0;
    }
    ep->task = sender;
    ep->itsPort = msg->itsPort;
    strncpy(ep->nvsKey, msg->nvsKey, sizeof(ep->nvsKey) - 1);
    ep->defaultPort = msg->defaultPort;
    ep->tls = msg->tls;
    ep->tcpNoDelay = true;
    ep->keepAlive = msg->keepAlive;
    ep->backlog = msg->backlog > 0 ? msg->backlog : 4;
}

static void epOpenPort(net_endpoint_t& ep) {
    int newPort = storageGetInt(ep.nvsKey, ep.defaultPort);
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

static void netAcceptOne(int ei, int fd, tls_conn_t* conn, struct sockaddr_in* peer) {
    auto& ep = netEps[ei];
    int ci = -1;
    for (int i = 0; i < NET_MAX_CLIENTS; i++)
        if (netClients[i].fd < 0) { ci = i; break; }
    if (ci < 0) { if (conn) tlsClose(conn); else close(fd); return; }
    net_connect_t cd = { 0, (uint8_t)(conn ? 1 : 0), {} };
    ip_addr_set_ip4_u32_val(cd.clientAddr, peer->sin_addr.s_addr);
    int h = itsConnectByHandle(ep.task, ep.itsPort, &cd, sizeof(cd), pdMS_TO_TICKS(100));
    if (h < 0) { if (conn) tlsClose(conn); else close(fd); return; }
    netClients[ci] = { fd, conn, h, ei, ep.task };
}

/* Unified select on all server fds + client fds, then accept + proxy */
static void netPollOnce() {
    itsPoll();
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
            if (!tlsReady()) continue;
            tls_conn_t* conn = tlsAccept(ep.serverFd);
            if (!conn) continue;
            int fd = tlsFd(conn);
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
            if (c.tlsConn ? (n < 0) : (n == 0)) { netClientClose(c); continue; }
            if (n > 0) { itsSend(c.itsHandle, netProxyBuf, n, 0); netActivity(); }
        }

        for (int rounds = 0; rounds < 4; rounds++) {
            size_t n = itsRecv(c.itsHandle, netProxyBuf, 4096, 0);
            if (n == 0) break;
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

/* ---- WiFi event handler ---- */

static void wifi_event_handler(void* arg, esp_event_base_t base,
                               int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    staConnected = false;
  else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    staConnected = true;
    xSemaphoreGive(wifiConnectedSem);
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
}

static void wifiHwStart() {
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_start();
  esp_sleep_enable_wifi_wakeup();
}

static void loadNetworks() {
  staCount = 0;
  for (int i = 1; i <= MAX_STA_NETWORKS; i++) {
    char key[24];
    snprintf(key, sizeof(key), "s.wifi.%d.ssid", i);
    storageGetStr(key, staNet[staCount].ssid, sizeof(staNet[0].ssid));
    if (!staNet[staCount].ssid[0]) break;
    snprintf(key, sizeof(key), "s.wifi.%d.pass", i);
    storageGetStr(key, staNet[staCount].pass, sizeof(staNet[0].pass));
    snprintf(key, sizeof(key), "s.wifi.%d.ip", i);
    storageGetStr(key, staNet[staCount].ip, sizeof(staNet[0].ip));
    snprintf(key, sizeof(key), "s.wifi.%d.gw", i);
    storageGetStr(key, staNet[staCount].gw, sizeof(staNet[0].gw));
    snprintf(key, sizeof(key), "s.wifi.%d.mask", i);
    storageGetStr(key, staNet[staCount].mask, sizeof(staNet[0].mask));
    snprintf(key, sizeof(key), "s.wifi.%d.dns", i);
    storageGetStr(key, staNet[staCount].dns, sizeof(staNet[0].dns));
    info("loaded wifi%d ssid='%s'(%d) pass='%s'(%d)\n", i,
         staNet[staCount].ssid, (int)strlen(staNet[staCount].ssid),
         staNet[staCount].pass, (int)strlen(staNet[staCount].pass));
    staCount++;
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
  wifi_ap_record_t* ap_list = (wifi_ap_record_t*)malloc(ap_count * sizeof(wifi_ap_record_t));
  if (!ap_list) return -1;
  esp_wifi_scan_get_ap_records(&ap_count, ap_list);
  for (int i = 0; i < ap_count; i++)
    dbg("  '%s' ch%d %ddBm\n", (const char*)ap_list[i].ssid, ap_list[i].primary, ap_list[i].rssi);
  int bestIdx = -1;
  info("known networks: %d\n", staCount);
  for (int s = 0; s < staCount; s++)
    info("  [%d] '%s'\n", s, staNet[s].ssid);
  for (int s = 0; s < staCount; s++)
    for (int i = 0; i < ap_count; i++)
      if (strcmp((const char*)ap_list[i].ssid, staNet[s].ssid) == 0) { bestIdx = s; goto found; }
found:
  free(ap_list);
  return bestIdx;
}

static bool connectSta(int idx) {
  info("connecting to '%s' pass(%d chars)\n", staNet[idx].ssid, (int)strlen(staNet[idx].pass));
  esp_wifi_disconnect();
  delay(100);
  esp_wifi_set_mode(WIFI_MODE_STA);
  delay(100);
  if (staNet[idx].ip[0]) {
    esp_netif_dhcpc_stop(sta_netif);
    esp_netif_ip_info_t ip_info = {};
    ip_info.ip.addr      = ipaddr_addr(staNet[idx].ip);
    ip_info.gw.addr      = ipaddr_addr(staNet[idx].gw);
    ip_info.netmask.addr = ipaddr_addr(staNet[idx].mask);
    esp_netif_set_ip_info(sta_netif, &ip_info);
    esp_netif_dns_info_t dns_info = {};
    if (staNet[idx].dns[0])
      dns_info.ip.u_addr.ip4.addr = ipaddr_addr(staNet[idx].dns);
    else
      dns_info.ip.u_addr.ip4.addr = ip_info.gw.addr;
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns_info);
  } else {
    esp_netif_dhcpc_start(sta_netif);
  }
  wifi_config_t wifi_config = {};
  strncpy((char*)wifi_config.sta.ssid, staNet[idx].ssid, sizeof(wifi_config.sta.ssid));
  if (staNet[idx].pass[0])
    strncpy((char*)wifi_config.sta.password, staNet[idx].pass, sizeof(wifi_config.sta.password));
  esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
  xSemaphoreTake(wifiConnectedSem, 0);
  staConnected = false;
  esp_wifi_connect();
  bool ok;
  if (staNet[idx].ip[0]) {
    uint32_t t = millis();
    while (!staConnected && millis() - t < 15000) delay(200);
    wifi_ap_record_t ap_info;
    ok = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
  } else {
    ok = (xSemaphoreTake(wifiConnectedSem, pdMS_TO_TICKS(15000)) == pdTRUE);
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
    info("%s ip %s dns %s\n", staNet[idx].ssid, ip_str, dns_str);
    return true;
  }
  info("%s connect failed\n", staNet[idx].ssid);
  esp_wifi_disconnect();
  return false;
}

static bool startAP() {
  if (storageGetInt("s.wifi.0.disable")) { info("AP disabled\n"); return false; }
  char ssid[33], pass[65], ip[16], mask[16];
  storageGetStr("s.wifi.0.ssid", ssid, sizeof(ssid), WIFI_AP_SSID);
  storageGetStr("s.wifi.0.pass", pass, sizeof(pass), WIFI_AP_PASS);
  storageGetStr("s.wifi.0.ip",   ip,   sizeof(ip),   WIFI_AP_IP);
  storageGetStr("s.wifi.0.mask", mask, sizeof(mask),  WIFI_AP_MASK);
  esp_wifi_set_mode(WIFI_MODE_AP);
  esp_netif_dhcps_stop(ap_netif);
  esp_netif_ip_info_t ip_info = {};
  ip_info.ip.addr      = ipaddr_addr(ip);
  ip_info.gw.addr      = ipaddr_addr(ip);
  ip_info.netmask.addr = ipaddr_addr(mask);
  esp_netif_set_ip_info(ap_netif, &ip_info);
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
  if (storageGetInt("s.net.mdns", 1)) {
    char hostname[32];
    storageGetStr("s.net.hostname", hostname, sizeof(hostname), "seccam");
    esp_netif_set_hostname(sta_netif, hostname);
  } else {
    esp_netif_set_hostname(sta_netif, "");
  }
}

/* ---- WiFi state machine ---- */

enum wifi_state_t { ST_OFF, ST_SCANNING, ST_STA_CONNECTED, ST_AP };
static volatile wifi_state_t wifiState = ST_OFF;

static void doUp() {
  esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
  lastActivityMs = millis();
  epOpenAll();
  info("net up\n");
  storageSet("net.up", 1);
  fireEvent(NET_EV_UP);
}

static void doDown(wifi_state_t& state) {
  info("shutting down\n");
  rtcWifiUp = false;
  wantDown = false;
  fireEvent(NET_EV_DOWN);
  epCloseAll();
  storageSet("net.up", 0);
  delay(200);
  esp_wifi_disconnect();
  esp_wifi_stop();
  esp_wifi_deinit();
  if (state == ST_STA_CONNECTED || state == ST_AP)
    pmLockRelease(netDeepLock);
  state = ST_OFF;
}

static void netCmdHandler(TaskHandle_t, uint16_t, const void* data, size_t len) {
  if (len < 1) return;
  uint8_t cmd = *(const uint8_t*)data;
  switch (cmd) {
    case NET_CMD_UP:         cmdUp = true; break;
    case NET_CMD_DOWN:       wantDown = true; info("down requested\n"); break;
    case NET_CMD_FORCE_DOWN: cmdForceDown = true; break;
  }
}

static void netTaskFn(void* arg) {
  itsClientInit(NET_MAX_CLIENTS, netItsDisconnect);
  itsOnAux(netOnAux);
  itsOnAux(netCmdHandler, NET_CMD_PORT);

  storageSubscribeChanges("s.", ON_CHANGE {
    if (!netIsUp()) return;
    fireEvent(NET_EV_CFG_CHANGED, key);
    epOpenAll();
  });

  wifiNetifInit();

  xSemaphoreGive(readySem);  /* unblock netInit — task is running */

  int searchTimeout = storageGetInt("s.wifi.timeout");
  wifi_state_t state = ST_OFF;
  uint32_t scanStartMs = millis();
  uint32_t lastApRetryMs = 0;
  const uint32_t AP_RETRY_MS = 5 * 60 * 1000;

  if (rtcWifiUp) {
    setDhcpHostname();
    wifiHwStart();
    esp_wifi_set_mode(WIFI_MODE_STA);
    state = ST_SCANNING;
    if (staCount == 0) {
      if (startAP()) { state = ST_AP; pmLockAcquire(netDeepLock); }
      else state = ST_OFF;
    }
  } else {
    info("wifi disabled\n");
  }

  for (;;) {
    bool connected = (state == ST_STA_CONNECTED || state == ST_AP);

    /* Sleep when off; otherwise just drain ITS inbox */
    if (state == ST_OFF)
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (itsPoll()) {}  /* aux commands + config change subscriptions */

    /* Process command flags set by aux handler */
    if (cmdForceDown && state != ST_OFF) {
      cmdForceDown = false;
      doDown(state); wifiState = state; continue;
    }
    cmdForceDown = false;

    if (cmdUp) {
      cmdUp = false;
      if (wantDown) { wantDown = false; info("down cancelled\n"); }
      if (state == ST_OFF) {
        info("coming up\n");
        rtcWifiUp = true;
        loadNetworks();
        setDhcpHostname();
        wifiHwStart();
        esp_wifi_set_mode(WIFI_MODE_STA);
        state = ST_SCANNING;
        scanStartMs = millis();
        wifiState = state;
      }
    }
    if (wantDown && connected) {
      if (millis() - lastActivityMs >= WIFI_IDLE_TIMEOUT_MS) {
        info("idle timeout, shutting down\n");
        doDown(state); wifiState = state; continue;
      }
    }

    switch (state) {
      case ST_OFF: break;
      case ST_SCANNING: {
        int idx = scanForKnown();
        if (idx >= 0 && connectSta(idx)) {
          state = ST_STA_CONNECTED;
          pmLockAcquire(netDeepLock);
          doUp();
        } else if (millis() - scanStartMs >= (uint32_t)(searchTimeout * 1000)) {
          if (startAP()) { state = ST_AP; pmLockAcquire(netDeepLock); doUp(); }
          else state = ST_OFF;
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
          pmLockRelease(netDeepLock);
          state = ST_SCANNING;
          scanStartMs = millis();
        }
        break;
      case ST_AP:
        netPollOnce();
        if (staCount > 0 && !wantDown && millis() - lastApRetryMs >= AP_RETRY_MS) {
          lastApRetryMs = millis();
          fireEvent(NET_EV_DOWN);
          epCloseAll();
          pmLockRelease(netDeepLock);
          esp_wifi_set_mode(WIFI_MODE_STA);
          delay(100);
          int idx = scanForKnown();
          if (idx >= 0 && connectSta(idx)) {
            state = ST_STA_CONNECTED;
            pmLockAcquire(netDeepLock);
            doUp();
          } else {
            startAP();
            pmLockAcquire(netDeepLock);
            doUp();
          }
        }
        break;
    }
    wifiState = state;
  }
}

/* ---- Public API ---- */

static void netCliCmd(const char* args) {
    if (strcmp(args, "help") == 0) { cliPrintf("  %-*s WiFi control\n", CLI_HELP_COL, "net [up|down|down!]"); return; }
    if (strcmp(args, "up") == 0) netUp();
    else if (strcmp(args, "down!") == 0) netDown(true);
    else if (strcmp(args, "down") == 0) netDown();
    else cliPrintf("usage: net [up|down|down!]\n");
}

void netInit() {
  pmLockCreate(PM_NO_DEEP_SLEEP, "net", &netDeepLock);
  cliRegisterCmd("net", netCliCmd);

  for (int i = 0; i < NET_MAX_CLIENTS; i++) {
    netClients[i].fd = -1;
    netClients[i].tlsConn = nullptr;
    netClients[i].itsHandle = -1;
  }
  netProxyBuf = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);

  if (!rtcValid())
    rtcWifiUp = storageGetInt("s.wifi.enable", 1) != 0;

  loadNetworks();
  readySem = xSemaphoreCreateBinary();
  wifiConnectedSem = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCoreWithCaps(netTaskFn, "net", 8192, nullptr, 2, &netHandle, 0, MALLOC_CAP_SPIRAM);
  xSemaphoreTake(readySem, portMAX_DELAY);  /* wait until task is running */
}

void netUp() {
  if (!netHandle) return;
  uint8_t cmd = NET_CMD_UP;
  itsSendAuxByHandle(netHandle, &cmd, 1, pdMS_TO_TICKS(100), NET_CMD_PORT);
}

void netDown(bool force) {
  if (!netHandle) return;
  uint8_t cmd = force ? NET_CMD_FORCE_DOWN : NET_CMD_DOWN;
  itsSendAuxByHandle(netHandle, &cmd, 1, pdMS_TO_TICKS(100), NET_CMD_PORT);
}

bool netIsUp() {
  return wifiState == ST_STA_CONNECTED || wifiState == ST_AP;
}

void netActivity() {
  lastActivityMs = millis();
}

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
