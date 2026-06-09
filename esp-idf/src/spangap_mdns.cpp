/**
 * mDNS — service advertisement via ESP-IDF mdns component.
 *
 * Reads `s.net.mdns.<name> = <port>` config tree: each entry is advertised as
 * `_<name>._tcp`. The value is either a literal port or the name of the config
 * key that holds the live port (e.g. "s.net.https_port") — the latter is
 * resolved at advertise time so the advertisement follows the service's actual
 * port. A resolved port <= 0 (or a dropped entry) advertises nothing. Empty
 * tree → no mDNS services. Hostname comes from `s.net.hostname`.
 *
 * net owns only the mechanism and the `s.net.mdns_enable` master switch — the
 * service entries belong to whoever serves them: spangap-web seeds http/https
 * (→ s.net.{http,https}_port), sshd seeds ssh (→ s.sshd.port), each in its own
 * init.
 */
#include "spangap_mdns.h"
#include "storage.h"
#include "net.h"
#include "log.h"
#include "mdns.h"
#include "esp_event.h"
#include "esp_wifi.h"

static void mdnsAdvertiseEntry(const char* key, const char* val) {
    /* key looks like "s.net.mdns.http"; we want the last segment ("http"). */
    const char* dot = strrchr(key, '.');
    if (!dot || !dot[1]) return;
    /* val is either a literal port ("443") or the name of the config key that
     * holds the live port ("s.net.https_port"). Resolving the reference at
     * advertise time (we re-run on every NET_EV_UP) keeps the advertisement in
     * step with the service's actual configured port instead of a stale copy. */
    int port = (val[0] >= '0' && val[0] <= '9') ? atoi(val)
                                                : storageGetInt(val, 0);
    if (port <= 0) return;
    char service[16];
    snprintf(service, sizeof(service), "_%s", dot + 1);
    mdns_service_add(NULL, service, "_tcp", port, NULL, 0);
}

static void mdnsStart(const char*) {
    if (!storageGetInt("s.net.mdns_enable", 1)) return;  /* master switch */
    char hostname[32];
    storageGetStr("s.net.hostname", hostname, sizeof(hostname), "");
    if (!hostname[0]) return;       /* no hostname → no mDNS */
    mdns_init();
    mdns_hostname_set(hostname);
    mdns_instance_name_set(hostname);
    storageForEach("s.net.mdns", mdnsAdvertiseEntry);

    /* The mdns component enables its per-interface responder only when its own
     * event handler *catches* the interface coming up — for the SoftAP that's
     * WIFI_EVENT_AP_START, and that handler is only registered inside
     * mdns_init() just above. On a factory-reset boot the net task has no
     * stored networks, so it switches straight to the built-in AP within
     * milliseconds of boot — long before this init runs — and AP_START has
     * already fired and been missed. The result: mDNS is initialised but the
     * AP responder is never enabled, so <hostname>.local is dead for anyone
     * joined to the device's AP (the symptom on a fresh device). STA usually
     * escapes this because the scan+associate delay leaves mdns time to
     * register before GOT_IP. Enable the AP responder now that the handler
     * exists, mirroring exactly what mdns's own WIFI_EVENT_AP_START handler
     * does — post_enable_pcb(MDNS_IF_AP, IPv4) — but driving mdns directly via
     * its public API. We must NOT re-post the global WIFI_EVENT_AP_START:
     * esp_netif_create_default_wifi_ap() also installs a default handler for
     * that event which calls esp_netif_action_start() → esp_netif_start() →
     * netif_add() on the already-started AP netif, tripping lwIP's "netif
     * already added" assert. mdns_netif_action resolves the predefined
     * WIFI_AP_DEF interface and enabling an already-enabled pcb is idempotent. */
    if (netIsUp() && !netIsStaConnected()) {
        esp_netif_t* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (ap) mdns_netif_action(ap, MDNS_EVENT_ENABLE_IP4);
    }
}

static void mdnsStop(const char*) {
    mdns_free();
}

void mdnsInit() {
    storageDefault("s.net.mdns_enable", 1);   /* master switch, default on */

    netRegister(NET_EV_UP,   mdnsStart);
    netRegister(NET_EV_DOWN, mdnsStop);
}
