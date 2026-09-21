idf_component_register(
    SRCS "wlink_sta.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event
)
