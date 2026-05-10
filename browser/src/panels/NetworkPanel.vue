<template>
  <div class="q-gutter-y-md">
    <PanelHeading>WiFi</PanelHeading>
    <div class="q-gutter-y-sm">
      <SettingToggle label="Enable" k="s.net.wifi.enable" />

      <!-- STA status -->
      <div v-if="staUp" class="status-block q-mt-sm">
        <div class="status-row"><span class="status-label">Status</span><span class="text-positive">Connected</span></div>
        <div class="status-row"><span class="status-label">SSID</span><span>{{ device.get('wifi.sta.ssid') || '—' }}</span></div>
        <div class="status-row"><span class="status-label">IP</span><span>{{ device.get('wifi.sta.ip') || '—' }}</span></div>
        <div class="status-row"><span class="status-label">Router</span><span>{{ device.get('wifi.sta.router') || '—' }}</span></div>
        <div class="status-row"><span class="status-label">Netmask</span><span>{{ device.get('wifi.sta.netmask') || '—' }}</span></div>
        <div class="status-row"><span class="status-label">DNS</span><span>{{ device.get('wifi.sta.dns') || '—' }}</span></div>
        <div class="status-row"><span class="status-label">MAC</span><span style="font-family:monospace;font-size:12px">{{ device.get('wifi.mac') || '—' }}</span></div>
        <div class="status-row"><span class="status-label">Signal</span><span>{{ rssiLabel }}</span></div>
        <div class="status-row"><span class="status-label">Traffic</span><span>in {{ device.get('wifi.traffic_in') || '0' }}, out {{ device.get('wifi.traffic_out') || '0' }}</span></div>
      </div>
      <div v-else-if="staState === 'connecting'" class="status-block q-mt-sm">
        <div class="status-row"><span class="status-label">Status</span><span class="text-grey-6">Connecting...</span></div>
        <div class="status-row"><span class="status-label">MAC</span><span style="font-family:monospace;font-size:12px">{{ device.get('wifi.mac') || '—' }}</span></div>
      </div>
    </div>

    <q-separator dark />

    <!-- Known Networks -->
    <PanelHeading>Known Networks</PanelHeading>
    <div class="q-gutter-y-sm">
      <div v-if="nets.length === 0" class="text-caption" style="opacity:0.5">No networks configured</div>
      <div
        v-for="(net, idx) in nets" :key="idx"
        class="net-item"
        :class="{
          selected: selectedIdx === idx,
          connected: connectedIdx === idx,
          visible: isVisible(net.ssid),
        }"
        draggable="true"
        @click="selectedIdx = idx"
        @dragstart="onDragStart(idx, $event)"
        @dragover.prevent="onDragOver(idx, $event)"
        @drop="onDrop(idx)"
        @dragend="dragIdx = -1"
      >
        <div class="net-drag-handle">&#x2630;</div>
        <div class="net-ssid">{{ net.ssid }}</div>
        <div v-if="connectedIdx === idx" class="net-badge">connected</div>
      </div>

      <div class="row q-gutter-x-sm q-mt-xs">
        <q-btn dense no-caps label="+" class="net-btn" @click="showScanDialog = true" />
        <q-btn dense no-caps label="&minus;" class="net-btn" :disable="selectedIdx < 0 || selectedIdx === connectedIdx" @click="removeNetwork" />
      </div>

      <!-- Per-network settings for selected network -->
      <template v-if="selNet">
        <q-separator dark class="q-mt-sm" />
        <div class="text-caption q-mt-xs" style="opacity:0.7;font-weight:600">{{ selNet.ssid }}</div>

        <!-- Connect / Disconnect -->
        <q-btn v-if="connectedIdx === selectedIdx" dense no-caps label="Disconnect"
          class="q-mt-xs" style="font-size:13px;background:#c62828;color:white" @click="doDisconnect" />
        <q-btn v-else dense no-caps label="Connect"
          class="q-mt-xs" style="font-size:13px;background:white;color:#111" @click="doConnect(selectedIdx)" />

        <!-- DHCP / Manual -->
        <div class="q-mt-sm">
          <q-radio :model-value="selNet.ip ? 'manual' : 'dhcp'" val="dhcp" label="DHCP" dark dense class="text-caption" @update:model-value="setDhcpMode('dhcp')" />
          <q-radio :model-value="selNet.ip ? 'manual' : 'dhcp'" val="manual" label="Manual" dark dense class="text-caption q-ml-md" @update:model-value="setDhcpMode('manual')" />
        </div>
        <template v-if="selNet.ip">
          <NetField label="IP" :value="selNet.ip" @change="setNetField('ip', $event)" />
          <NetField label="Netmask" :value="selNet.mask" @change="setNetField('mask', $event)" />
          <NetField label="Gateway" :value="selNet.gw" @change="setNetField('gw', $event)" />
          <NetField label="DNS" :value="selNet.dns" @change="setNetField('dns', $event)" />
        </template>

        <!-- Custom MAC -->
        <div class="q-mt-sm">
          <q-radio :model-value="selNet.mac ? 'custom' : 'default'" val="default" label="Default MAC" dark dense class="text-caption" @update:model-value="setMacMode('default')" />
          <q-radio :model-value="selNet.mac ? 'custom' : 'default'" val="custom" label="Custom MAC" dark dense class="text-caption q-ml-md" @update:model-value="setMacMode('custom')" />
        </div>
        <NetField v-if="selNet.mac" label="MAC" :value="selNet.mac" @change="setNetField('mac', $event)" />
      </template>
    </div>

    <q-separator dark />

    <!-- Access Point -->
    <PanelHeading>Access Point</PanelHeading>
    <div class="q-gutter-y-sm">
      <div class="row items-center no-wrap">
        <div class="col-4 text-caption">Enable AP</div>
        <div class="col">
          <q-toggle :model-value="apEnabled" dense color="primary" @update:model-value="onApToggle" />
        </div>
      </div>

      <!-- AP status -->
      <div v-if="apUp" class="status-block">
        <div class="status-row"><span class="status-label">Status</span><span class="text-warning">Active</span></div>
        <div class="status-row"><span class="status-label">SSID</span><span>{{ device.get('wifi.ap.ssid') || '—' }}</span></div>
        <div class="status-row"><span class="status-label">IP</span><span>{{ device.get('wifi.ap.ip') || '—' }}</span></div>
        <div class="status-row"><span class="status-label">Netmask</span><span>{{ device.get('wifi.ap.netmask') || '—' }}</span></div>
      </div>

      <template v-if="apEnabled">
        <SettingText label="SSID" k="s.net.wifi.ap.ssid" />
        <SettingText label="Password" k="s.net.wifi.ap.pass" />
        <SettingText label="IP" k="s.net.wifi.ap.ip" />
        <SettingText label="Netmask" k="s.net.wifi.ap.mask" />
      </template>
    </div>

    <q-separator dark />

    <PanelHeading>mDNS</PanelHeading>
    <div class="q-gutter-y-sm">
      <SettingToggle label="Enable" k="s.net.mdns" />
    </div>

    <!-- Scan dialog -->
    <WifiScanDialog v-model="showScanDialog" @add="onNetworkAdded" />

    <!-- AP disable warning dialog -->
    <q-dialog v-model="showApWarning" persistent>
      <q-card dark class="q-pa-md" style="min-width:280px">
        <q-card-section>
          <div class="text-subtitle1 text-weight-medium">Disable Access Point?</div>
          <div class="text-caption q-mt-sm" style="opacity:0.8">
            {{ !staUp
              ? 'You are not connected to another network. Disabling the AP will make this device unreachable. Better to leave this on as a fallback so you can get in.'
              : 'The access point will be disabled. You can re-enable it from the settings if needed.'
            }}
          </div>
        </q-card-section>
        <q-card-actions align="right">
          <q-btn flat label="Cancel" @click="showApWarning = false" />
          <q-btn flat label="Disable" color="negative" @click="confirmApDisable" />
        </q-card-actions>
      </q-card>
    </q-dialog>
  </div>
</template>

<script setup lang="ts">
import { computed, ref, watch } from 'vue'
import { useDeviceStore } from '../stores/device'
import WifiScanDialog from './WifiScanDialog.vue'

const device = useDeviceStore()

// ---- Inline field component (no SettingText — we write through the array) ----
const NetField = {
  props: { label: String, value: String },
  emits: ['change'],
  template: `<div class="row items-center no-wrap">
    <div class="col-4 text-caption">{{ label }}</div>
    <q-input class="col" :model-value="value" dense outlined debounce="500"
      autocomplete="off" autocorrect="off" autocapitalize="off" spellcheck="false"
      @update:model-value="$emit('change', $event)" />
  </div>`,
}

// ---- Status ----

const staState = computed(() => String(device.get('wifi.sta.state') ?? 'off'))
const apState = computed(() => String(device.get('wifi.ap.state') ?? 'off'))
const staUp = computed(() => staState.value === 'connected')
const apUp = computed(() => apState.value === 'active')

const connectedSsid = computed(() => staUp.value ? String(device.get('wifi.sta.ssid') ?? '') : '')
const connectedIdx = computed(() => {
  if (!connectedSsid.value) return -1
  return nets.value.findIndex(n => n.ssid === connectedSsid.value)
})

const rssiLabel = computed(() => {
  const rssi = Number(device.get('wifi.sta.rssi') ?? 0)
  if (rssi === 0) return '—'
  if (rssi >= -50) return `Excellent (${rssi} dBm)`
  if (rssi >= -60) return `Good (${rssi} dBm)`
  if (rssi >= -70) return `Fair (${rssi} dBm)`
  return `Weak (${rssi} dBm)`
})

// ---- Known networks (array — all writes go through sendJson) ----

interface KnownNet { ssid: string; pass: string; ip: string; gw: string; mask: string; dns: string; mac: string }

const nets = computed<KnownNet[]>(() => {
  const arr = device.get('s.net.wifi.nets')
  if (!Array.isArray(arr)) return []
  return arr.map((n: any) => ({
    ssid: String(n?.ssid ?? ''),
    pass: String(n?.pass ?? ''),
    ip: String(n?.ip ?? ''),
    gw: String(n?.gw ?? ''),
    mask: String(n?.mask ?? ''),
    dns: String(n?.dns ?? ''),
    mac: String(n?.mac ?? ''),
  }))
})

const selectedIdx = ref(-1)

const selNet = computed(() => {
  const i = selectedIdx.value
  return (i >= 0 && i < nets.value.length) ? nets.value[i] : null
})

watch(nets, (n) => {
  if (selectedIdx.value >= n.length) selectedIdx.value = n.length - 1
  if (selectedIdx.value < 0 && n.length > 0) selectedIdx.value = connectedIdx.value >= 0 ? connectedIdx.value : 0
}, { immediate: true })

// Scanned networks for visibility
const scannedSsids = computed(() => {
  const arr = device.get('wifi.scanned')
  if (!Array.isArray(arr)) return new Set<string>()
  return new Set(arr.map((n: any) => String(n?.ssid ?? '')))
})

function isVisible(ssid: string) { return scannedSsids.value.has(ssid) }

// ---- Write helpers: modify array then send whole thing ----

/** Strip empty-string fields before sending — server defaults absent keys to "". */
function writeNets(arr: any[]) {
  const clean = arr.map(n => {
    const out: Record<string, string> = {}
    for (const [k, v] of Object.entries(n))
      if (typeof v === 'string' && v !== '') out[k] = v
    return out
  })
  device.sendJson({ s: { net: { wifi: { nets: clean } } } })
  device.save()
}

function updateNet(idx: number, fields: Partial<KnownNet>) {
  const arr = [...nets.value]
  arr[idx] = { ...arr[idx], ...fields }
  writeNets(arr)
}

function setNetField(field: keyof KnownNet, val: string | number | null) {
  if (selectedIdx.value < 0) return
  updateNet(selectedIdx.value, { [field]: String(val ?? '') })
}

function setDhcpMode(mode: string) {
  if (selectedIdx.value < 0) return
  if (mode === 'dhcp') {
    updateNet(selectedIdx.value, { ip: '', gw: '', mask: '', dns: '' })
  } else {
    // Switch to manual — populate with placeholder so the fields appear
    updateNet(selectedIdx.value, { ip: '0.0.0.0', gw: '', mask: '255.255.255.0', dns: '' })
  }
}

function setMacMode(mode: string) {
  if (selectedIdx.value < 0) return
  if (mode === 'default') {
    updateNet(selectedIdx.value, { mac: '' })
  } else {
    updateNet(selectedIdx.value, { mac: '00:00:00:00:00:00' })
  }
}

// ---- Drag reorder ----

const dragIdx = ref(-1)

function onDragStart(idx: number, e: DragEvent) {
  dragIdx.value = idx
  e.dataTransfer!.effectAllowed = 'move'
}

function onDragOver(idx: number, e: DragEvent) {
  if (dragIdx.value < 0 || dragIdx.value === idx) return
  e.dataTransfer!.dropEffect = 'move'
}

function onDrop(targetIdx: number) {
  const fromIdx = dragIdx.value
  dragIdx.value = -1
  if (fromIdx < 0 || fromIdx === targetIdx) return
  const arr = [...nets.value]
  const [moved] = arr.splice(fromIdx, 1)
  arr.splice(targetIdx, 0, moved)
  writeNets(arr)
  selectedIdx.value = targetIdx
}

// ---- Add / Remove / Connect / Disconnect ----

function removeNetwork() {
  const idx = selectedIdx.value
  if (idx < 0 || idx === connectedIdx.value) return
  const arr = [...nets.value]
  arr.splice(idx, 1)
  writeNets(arr)
  if (selectedIdx.value >= arr.length) selectedIdx.value = arr.length - 1
}

const showScanDialog = ref(false)

function onNetworkAdded(net: { ssid: string; pass: string }) {
  const arr = [...nets.value]
  const existing = arr.findIndex((n: any) => n.ssid === net.ssid)
  if (existing >= 0) {
    arr[existing] = { ...arr[existing], pass: net.pass }
  } else {
    arr.push({ ssid: net.ssid, pass: net.pass } as KnownNet)
  }
  writeNets(arr)
  selectedIdx.value = existing >= 0 ? existing : arr.length - 1
  doConnect(selectedIdx.value)
}

function doConnect(idx: number) { device.set('wifi.connect', idx) }
function doDisconnect() { device.set('wifi.disconnect', 1) }

// ---- AP ----

const apEnabled = computed(() => !device.get('s.net.wifi.ap.disable'))
const showApWarning = ref(false)

function onApToggle(val: boolean) {
  if (!val) showApWarning.value = true
  else device.set('s.net.wifi.ap.disable', 0)
}

function confirmApDisable() {
  showApWarning.value = false
  device.set('s.net.wifi.ap.disable', 1)
}

// ---- Scanning lifecycle: only while scan dialog is open ----

watch(showScanDialog, (open) => {
  device.set('wifi.scan', open ? 1 : 0)
})
</script>

<style scoped>
.status-block {
  background: rgba(255,255,255,0.04);
  border-radius: 6px;
  padding: 8px 12px;
}
.status-row {
  display: flex;
  align-items: center;
  padding: 2px 0;
  font-size: 13px;
}
.status-label {
  width: 70px;
  flex-shrink: 0;
  opacity: 0.5;
  font-size: 12px;
}
.net-item {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 6px 10px;
  border-radius: 4px;
  cursor: pointer;
  user-select: none;
  color: rgba(255,255,255,0.7);
  transition: background 0.1s;
}
.net-item:hover { background: rgba(255,255,255,0.06); }
.net-item.selected { background: rgba(255,255,255,0.1); outline: 1px solid rgba(255,255,255,0.2); }
.net-item.visible .net-ssid { color: #4caf50; }
.net-item.connected .net-ssid { color: #4caf50; font-weight: 700; }
.net-drag-handle {
  cursor: grab;
  opacity: 0.3;
  font-size: 14px;
  flex-shrink: 0;
}
.net-ssid { flex: 1; font-size: 14px; }
.net-badge {
  font-size: 11px;
  opacity: 0.5;
  font-style: italic;
}
.net-btn {
  font-size: 16px !important;
  min-width: 32px !important;
  padding: 2px 10px !important;
  background: rgba(255,255,255,0.08) !important;
  color: rgba(255,255,255,0.7) !important;
}
.net-btn:hover { background: rgba(255,255,255,0.16) !important; }
</style>
