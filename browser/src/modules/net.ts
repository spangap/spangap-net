import { useMenuStore } from 'spangap-browser/stores/menu'
import NetworkPanel from '../panels/NetworkPanel.vue'
import MdnsPanel from '../panels/MdnsPanel.vue'

/**
 * Settings → Internet. The net straddle owns its own browser UI (parallel to
 * the firmware `netLcdRegister` / `mdnsLcdRegister` LCD panes): WiFi
 * (STA / AP / known networks) and mDNS. Panels for UPnP / WireGuard / DuckDNS /
 * ACME / SSH live in their own straddles and register under the same
 * `settings/network` submenu — the menu store merges containers by path. The
 * submenu keeps the `network` id (so those straddles need no change) but is
 * labelled "Internet"; WiFi is lifted to the front of the dropdown.
 */
export function registerNet() {
  const menu = useMenuStore()
  menu.setMenu('settings/network', { label: 'Internet', placement: 2 })
  menu.register('settings/network/wifi', 'WiFi', { type: 'panel', component: NetworkPanel }, { placement: 1 })
  menu.register('settings/network/mdns', 'mDNS', { type: 'panel', component: MdnsPanel })
}
