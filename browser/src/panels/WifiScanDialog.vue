<template>
  <q-dialog :model-value="modelValue" @update:model-value="$emit('update:modelValue', $event)" persistent>
    <q-card dark class="scan-card">
      <!-- Header -->
      <q-card-section class="row items-center q-pb-sm">
        <div class="text-subtitle1 text-weight-medium">{{ manualMode ? 'Add Network' : 'Available Networks' }}</div>
        <q-space />
        <q-btn flat round dense @click="close" style="color:rgba(255,255,255,0.5);font-size:16px">&times;</q-btn>
      </q-card-section>

      <!-- Scan results list -->
      <q-card-section v-if="!manualMode" class="q-pt-none scan-list-section">
        <div v-if="scanned.length === 0" class="text-caption text-center q-pa-md" style="opacity:0.5">
          Scanning...
        </div>
        <div
          v-for="(net, idx) in scanned" :key="idx"
          class="scan-item"
          :class="{ selected: selectedScan === idx }"
          @click="selectedScan = idx"
          @dblclick="selectScanned(idx)"
        >
          <div class="scan-lock">
            <svg v-if="net.locked" width="14" height="14" viewBox="0 0 24 24" fill="currentColor" opacity="0.5">
              <path d="M18 8h-1V6c0-2.76-2.24-5-5-5S7 3.24 7 6v2H6c-1.1 0-2 .9-2 2v10c0 1.1.9 2 2 2h12c1.1 0 2-.9 2-2V10c0-1.1-.9-2-2-2zm-6 9c-1.1 0-2-.9-2-2s.9-2 2-2 2 .9 2 2-.9 2-2 2zm3.1-9H8.9V6c0-1.71 1.39-3.1 3.1-3.1s3.1 1.39 3.1 3.1v2z"/>
            </svg>
            <svg v-else width="14" height="14" viewBox="0 0 24 24" fill="currentColor" opacity="0.25">
              <path d="M12 17c1.1 0 2-.9 2-2s-.9-2-2-2-2 .9-2 2 .9 2 2 2zm6-9h-1V6c0-2.76-2.24-5-5-5-1.96 0-3.66 1.13-4.49 2.77l1.76.77C9.79 3.6 10.82 3 12 3c1.66 0 3 1.34 3 3v2H6c-1.1 0-2 .9-2 2v10c0 1.1.9 2 2 2h12c1.1 0 2-.9 2-2V10c0-1.1-.9-2-2-2z"/>
            </svg>
          </div>
          <div class="scan-ssid">{{ net.ssid || '(hidden)' }}</div>
          <div class="scan-signal">{{ signalBars(net.rssi) }}</div>
        </div>

        <div class="row q-mt-md q-gutter-x-sm">
          <q-btn dense no-caps label="Select" :disable="selectedScan < 0"
            style="font-size:13px;background:white;color:#111" @click="selectScanned(selectedScan)" />
          <q-space />
          <q-btn dense no-caps flat label="Add another network" style="font-size:13px;opacity:0.7"
            @click="manualMode = true; manualSsid = ''; manualPass = ''" />
        </div>
      </q-card-section>

      <!-- Manual entry mode -->
      <q-card-section v-else class="q-pt-none">
        <div class="q-gutter-y-sm">
          <q-input v-model="manualSsid" label="Network name (SSID)" dense outlined dark autofocus
            autocomplete="off" autocorrect="off" autocapitalize="off" spellcheck="false" />
          <q-input v-model="manualPass" label="Password (leave empty if open)" dense outlined dark
            type="password" autocomplete="off" />
        </div>
        <div class="row q-mt-md q-gutter-x-sm">
          <q-btn dense no-caps label="Add" :disable="!manualSsid.trim()"
            style="font-size:13px;background:white;color:#111"
            @click="addManual" />
          <q-btn dense no-caps flat label="Back to scan" style="font-size:13px;opacity:0.7"
            @click="manualMode = false" />
        </div>
      </q-card-section>

      <!-- Password prompt -->
      <q-card-section v-if="promptPassword" class="q-pt-none">
        <q-separator dark class="q-mb-md" />
        <div class="text-caption q-mb-sm">Password for <b>{{ promptSsid }}</b></div>
        <q-input v-model="promptPass" label="Password" dense outlined dark type="password"
          autocomplete="off" autofocus @keyup.enter="confirmPassword" />
        <div class="row q-mt-sm q-gutter-x-sm">
          <q-btn dense no-caps label="Connect" :disable="!promptPass"
            style="font-size:13px;background:white;color:#111" @click="confirmPassword" />
          <q-btn dense no-caps flat label="Cancel" style="font-size:13px;opacity:0.7"
            @click="promptPassword = false" />
        </div>
      </q-card-section>
    </q-card>
  </q-dialog>
</template>

<script setup lang="ts">
import { computed, ref, watch } from 'vue'
import { useDeviceStore } from '../stores/device'

const props = defineProps<{ modelValue: boolean }>()
const emit = defineEmits<{
  'update:modelValue': [val: boolean]
  'add': [net: { ssid: string; pass: string }]
}>()

const device = useDeviceStore()

// ---- Scan results ----

interface ScannedNet { ssid: string; bssid: string; rssi: number; locked: number }

const scanned = computed<ScannedNet[]>(() => {
  const arr = device.get('wifi.scanned')
  if (!Array.isArray(arr)) return []
  return arr.map((n: any) => ({
    ssid: String(n?.ssid ?? ''),
    bssid: String(n?.bssid ?? ''),
    rssi: Number(n?.rssi ?? -100),
    locked: Number(n?.locked ?? 0),
  }))
})

const selectedScan = ref(-1)

// Reset selection when dialog opens
watch(() => props.modelValue, (v) => {
  if (v) {
    selectedScan.value = -1
    manualMode.value = false
    promptPassword.value = false
  }
})

function signalBars(rssi: number): string {
  if (rssi >= -50) return '\u2588\u2588\u2588\u2588'  // full
  if (rssi >= -60) return '\u2588\u2588\u2588\u2591'
  if (rssi >= -70) return '\u2588\u2588\u2591\u2591'
  return '\u2588\u2591\u2591\u2591'
}

// ---- Selection flow ----

const promptPassword = ref(false)
const promptSsid = ref('')
const promptPass = ref('')

function selectScanned(idx: number) {
  if (idx < 0 || idx >= scanned.value.length) return
  const net = scanned.value[idx]
  if (net.locked) {
    promptSsid.value = net.ssid
    promptPass.value = ''
    promptPassword.value = true
  } else {
    emit('add', { ssid: net.ssid, pass: '' })
    close()
  }
}

function confirmPassword() {
  emit('add', { ssid: promptSsid.value, pass: promptPass.value })
  promptPassword.value = false
  close()
}

// ---- Manual entry ----

const manualMode = ref(false)
const manualSsid = ref('')
const manualPass = ref('')

function addManual() {
  if (!manualSsid.value.trim()) return
  emit('add', { ssid: manualSsid.value.trim(), pass: manualPass.value })
  close()
}

function close() {
  emit('update:modelValue', false)
}
</script>

<style scoped>
.scan-card {
  min-width: 320px;
  max-width: 400px;
  background: #1e1e1e;
}
.scan-list-section {
  max-height: 360px;
  overflow-y: auto;
}
.scan-item {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 8px 10px;
  border-radius: 4px;
  cursor: pointer;
  color: rgba(255,255,255,0.8);
  transition: background 0.1s;
}
.scan-item:hover { background: rgba(255,255,255,0.06); }
.scan-item.selected { background: rgba(255,255,255,0.1); outline: 1px solid rgba(255,255,255,0.2); }
.scan-lock { width: 18px; flex-shrink: 0; display: flex; align-items: center; justify-content: center; }
.scan-ssid { flex: 1; font-size: 14px; }
.scan-signal { font-size: 12px; opacity: 0.4; font-family: monospace; letter-spacing: -1px; }
</style>
