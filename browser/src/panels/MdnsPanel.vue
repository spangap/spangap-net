<!-- Settings → Internet → mDNS. Advertises the device on the LAN (.local) and
     the HTTP/HTTPS ports it publishes. Mirrors the on-device Internet/mDNS
     pane (mdns_lcd.cpp): enable + the two service ports. -->
<template>
  <div class="q-gutter-y-md">
    <PanelHeading>mDNS</PanelHeading>
    <div class="q-gutter-y-sm">
      <SettingToggle label="Enable" k="s.net.mdns_enable" />
      <SettingText label="HTTP port" k="s.net.mdns.http" />
      <SettingText label="HTTPS port" k="s.net.mdns.https" />
    </div>
  </div>
</template>
