idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES driver lwip esp_wifi nvs_flash esp_event mqtt cjson esp_driver_gpio
)