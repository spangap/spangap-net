/**
 * net_host — the link backend for a station that is a process.
 *
 * There is no radio and nothing to associate with: the station's address is
 * the loopback address the board was given, it is up from the first instant
 * and it never goes down. So this file is the small half of the backend
 * contract — bring the link up, publish the address, and run the relay's loop
 * forever. Everything that carries bytes is net_relay.cpp's, shared verbatim
 * with the chip.
 */
#include "net_priv.h"

#include "cli.h"
#include "compat.h"
#include "its.h"
#include "log.h"
#include "storage.h"

#include "esp_mac.h"

#include <cstdio>
#include <cstring>
#include <freertos/semphr.h>

/* The station's own address and identity come from the board straddle. Weak,
 * so a host build assembled without one still links and answers for the whole
 * of loopback. */
extern "C" __attribute__((weak)) const char* hwLinuxBindAddr(void) { return "127.0.0.1"; }

static TaskHandle_t      s_task = nullptr;
static SemaphoreHandle_t s_ready = nullptr;

/* The board's descriptor wait: select() that also returns when this task is
 * notified, which is how ITS traffic toward a socket arrives. Weak and
 * possibly absent; without it the relay waits the way it does on a chip. */
extern "C" int hwLinuxWait(int nfds, fd_set* rfds, fd_set* wfds, fd_set* efds,
                           TickType_t ticks) __attribute__((weak));

/* A socket or ITS traffic, for as long as neither comes: a change under s.net
 * is ITS traffic too (the subscription in netHostTaskFn), and the pass it wakes
 * opens and closes the endpoints it names. A second at most while a client is
 * held for its owner, who makes room without a word to this task. */
int netRelayWait(int maxFd, fd_set* rfds, fd_set* wfds, bool held) {
  if (hwLinuxWait)
    return hwLinuxWait(maxFd + 1, rfds, wfds, nullptr,
                       held ? pdMS_TO_TICKS(1000) : portMAX_DELAY);
  if (maxFd >= 0) {
    struct timeval tv = { 0, 10000 };
    return select(maxFd + 1, rfds, wfds, NULL, &tv);
  }
  vTaskDelay(pdMS_TO_TICKS(10));
  return 0;
}

/* The relay's aux command port still has to exist — `net up` and friends send
 * to it — and every command is a no-op on a link that is always up. */
static void netCmdHandler(TaskHandle_t, const void*, size_t) {}

static void netHostTaskFn(void*) {
  netRelayTaskInit();
  itsOnAux(NET_CMD_PORT, netCmdHandler);

  /* Delivered through this task's inbox, so a changed port ends the relay's
   * wait and the next pass acts on it; the pass itself is the handler. */
  storageSubscribeChanges("s.net.", ON_CHANGE { (void)key; (void)val; });

  /* The core CLI and log endpoints, registered on their tasks' behalf exactly
   * as the WiFi backend does. */
  netRegisterCorePorts();

  /* The rns boot barrier asks whether to wait for an address at all, then
   * waits for the flag setUpstream raises. Both are true here from the start. */
  storageSet("net.want", 1);

  netLinkUp = true;
  epOpenAll();
  info("net up on %s\n", hwLinuxBindAddr());
  fireEvent(NET_EV_UP);
  setUpstream(true);

  xSemaphoreGive(s_ready);

  for (;;) netPollOnce();
}

static void netHostCliCmd(const char* args) {
  if (cliWantsHelp(args)) {
    cliPrintf("%-*s show the station's address and open ports\n", CLI_HELP_COL, "net");
    return;
  }
  cliPrintf("address: %s\n", hwLinuxBindAddr());
  net_public_port_t pub[NET_MAX_ENDPOINTS];
  int n = netPublicPorts(pub, NET_MAX_ENDPOINTS);
  for (int i = 0; i < n; i++) cliPrintf("public: %s %u\n", pub[i].nvsKey, (unsigned)pub[i].port);
  char inBuf[16], outBuf[16];
  fmtSize(netTrafficInBytes, inBuf, sizeof(inBuf));
  fmtSize(netTrafficOutBytes, outBuf, sizeof(outBuf));
  cliPrintf("traffic: in %s, out %s\n", inBuf, outBuf);
}

void netInit() {
  netInitCommon();

  /* Every listener binds this station's own address, so several stations on
   * one machine keep the canonical port numbers instead of offsetting them. */
  netBindAddrV4 = inet_addr(hwLinuxBindAddr());

  storageSet("wifi.sta.up", 1);
  storageSet("wifi.sta.ip", hwLinuxBindAddr());
  {
    uint8_t mac[6] = {};
    esp_efuse_mac_get_default(mac);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    storageSet("wifi.mac", macStr);
  }

  cliRegisterCmd("net", netHostCliCmd);

  s_ready = xSemaphoreCreateBinary();
  s_task = spawnTask(netHostTaskFn, "net", 32768, nullptr, 2, 0);
  xSemaphoreTake(s_ready, portMAX_DELAY);
  vSemaphoreDelete(s_ready);
  s_ready = nullptr;
}

/* The link is the process's own loopback: it exists for as long as the process
 * does, and asking for it to go up or down is asking for nothing. */
void netUp() {}
void netDown(bool) {}
bool netIsUp() { return true; }
bool netIsStaConnected() { return true; }

void netMulticastRxAcquire() {}
void netMulticastRxRelease() {}

void netGetLocalIp(char* out, size_t len) {
  snprintf(out, len, "%s", hwLinuxBindAddr());
}

/* The traffic ring samples a WiFi interface's frame counters. There is no
 * interface here and no radio whose power draw the numbers would describe. */
void netTrafficInit(void) {}
int  netTrafficHistory(NetTrafSample*, int) { return 0; }
int  netTrafficAvgMa10(int) { return 0; }
