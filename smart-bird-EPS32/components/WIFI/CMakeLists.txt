# 注册 WiFi 联网组件。
idf_component_register(
    SRCS "WIFI.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi nvs_flash esp_event esp_netif StatusLED
)
