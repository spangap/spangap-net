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
