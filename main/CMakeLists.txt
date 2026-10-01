idf_component_register(
    SRCS
        "app_main.c"
        "config_store.c"
        "wifi_mgr.c"
        "metrics.c"
        "mqtt_ha.c"
        "web_server.c"
        "weblog.c"
        "fs_storage.c"
        "hardware.c"
        "hw_battery.c"
        "hw_i2c.c"
        "hw_io.c"
        "hw_motor.c"
        "hw_charge.c"
        "hw_display.c"
        "ble_server.c"
    INCLUDE_DIRS "."
    EMBED_FILES
        "www/index.html"
        "www/app.js"
        "www/style.css"
    REQUIRES
        nvs_flash esp_wifi esp_netif esp_event esp_http_server mqtt
        json fatfs wear_levelling esp_timer esp_app_format app_update
        esp_adc driver spi_flash esp_hw_support bt lwip
)

# File-browser path joins can trip format-truncation on deep paths; truncation is
# handled gracefully (stat fails), so don't let it fail the build.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format-truncation)
