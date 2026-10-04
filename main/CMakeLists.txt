idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "."
    EMBED_FILES
        "www/index.html"
        "www/app.js"
        "www/style.css"
    REQUIRES
        nvs_flash esp_wifi esp_netif esp_event esp_http_server mqtt
        json esp_timer esp_app_format app_update
        esp_adc driver spi_flash esp_hw_support esp_pm bt lwip mbedtls
)
