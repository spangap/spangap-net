/**
 * mDNS — service advertisement via ESP-IDF mdns component.
 *
 * Reads `s.net.mdns.<name> = <port>` config tree: each non-zero entry is
 * advertised as `_<name>._tcp` on the given port. Drop / set to 0 to stop
 * advertising. Empty tree → no mDNS services. Hostname comes from
 * `s.net.hostname`.
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
    int port = atoi(val);
    if (port <= 0) return;
    char service[16];
    snprintf(service, sizeof(service), "_%s", dot + 1);
    mdns_service_add(NULL, service, "_tcp", port, NULL, 0);
}

static void mdnsStart(const char*) {
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
     * register before GOT_IP. Re-post AP_START now that the handler exists so
     * it binds the already-up AP interface. AP_START carries no event data and
     * net's own wifi_event_handler ignores it, so the re-post is side-effect
     * free; mdns enabling an already-enabled pcb is idempotent. */
    if (netIsUp() && !netIsStaConnected())
        esp_event_post(WIFI_EVENT, WIFI_EVENT_AP_START, nullptr, 0, 0);
}

static void mdnsStop(const char*) {
    mdns_free();
}

#define MDNS_VERSION 1

void mdnsInit() {
    if (storageGetInt("s.net.mdns_version", 0) < MDNS_VERSION) {
        storageDefault("s.net.mdns.http", 80);
        storageDefault("s.net.mdns.https", 443);
        storageSet("s.net.mdns_version", MDNS_VERSION);
    }

    netRegister(NET_EV_UP,   mdnsStart);
    netRegister(NET_EV_DOWN, mdnsStop);
}
