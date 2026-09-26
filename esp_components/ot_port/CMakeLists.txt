# ESP-IDF component picked up via TUYAOS_EXTRA_COMPONENT_DIRS (tos.py sets it
# to <app>/esp_components). Holds the timing-critical, IDF-specific parts; the
# app talks to it through the plain-C header in include/.
idf_component_register(
    SRCS "ot_port.c" "hw_port.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_gpio esp_driver_rmt esp_timer esp_system app_update freertos
    WHOLE_ARCHIVE
)
