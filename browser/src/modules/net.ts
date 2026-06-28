import { useMenuStore } from 'spangap-browser/stores/menu'
import NetworkPanel from '../panels/NetworkPanel.vue'

/**
 * Settings → Internet. The net straddle owns its own browser UI (parallel to
 * the firmware `netLcdRegister` LCD pane): WiFi (STA / AP / known networks).
 * The mDNS pane is generated from the straddle.yaml `settings:` block (LCD +
 * web + defaults from one source). Panels for UPnP / WireGuard / DuckDNS /
 * ACME / SSH live in their own straddles and register under the same
 * `settings/network` submenu — the menu store merges containers by path. The
 * submenu keeps the `network` id (so those straddles need no change) but is
 * labelled "Internet"; WiFi is lifted to the front of the dropdown.
 */
export function registerNet() {
  const menu = useMenuStore()
  menu.setMenu('settings/network', { label: 'Internet', placement: 2 })
  menu.register('settings/network/wifi', 'WiFi', { type: 'panel', component: NetworkPanel }, { placement: 1 })
  // mDNS pane is generated from this straddle's `settings:` block (see
  // straddles.gen.ts → registerGeneratedPanels), registered at
  // 'settings/network/mdns' — no hand-written panel here.
}
