/**
 * mDNS — service advertisement wrapper.
 * Registers NET_EV_UP/DOWN callbacks to start/stop mDNS.
 */
#ifndef SPANGAP_MDNS_H
#define SPANGAP_MDNS_H

/** Register mDNS net event callbacks. Call from main after netInit(). */
void mdnsInit();

#endif
