file(GLOB NET_SRCS "src/*.cpp")

# CMake REQUIRES is broader than straddle.yaml's logical requires: it covers
# the actual #include relationships in source. spangap-lcd is here because
# net.cpp / spangap_mdns.cpp register on-device Settings panes via
# lcdSetting* helpers (gated by CONFIG_SPANGAP_LCD). Plan-aligned future
# moves that registration into an esp-idf/lcd/ slice; until then this CMake
# REQUIRES is the build-time bridge.
idf_component_register(
    SRCS ${NET_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES
        spangap-core
        spangap-lcd
        esp_event
        esp_wifi
        esp_netif
        nvs_flash
        lwip
        mbedtls
        espressif__mdns
        esp_timer
        json
)
