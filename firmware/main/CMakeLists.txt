idf_component_register(
                       SRCS 
                            "main.c" 
                       PRIV_REQUIRES 
                            esp_driver_i2c 
                            esp_event
                            ssd1306
                            gfx
                            wlink
                            mqtt_svc
                            nvs_flash
                       INCLUDE_DIRS "."
)
