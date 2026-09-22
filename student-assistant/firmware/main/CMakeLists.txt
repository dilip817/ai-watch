idf_component_register(
    SRCS "main.c"
         "wifi_manager.c"
         "audio_pipeline.c"
         "audio_capture.c"
         "api_client.c"
         "sd_queue.c"
         "ui_screens.c"
         "display_power.c"
         "battery_monitor.c"
         "splash_screen.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_http_client esp_http_server esp_netif
             driver fatfs json mbedtls esp_timer
             esp_lcd esp_adc sdmmc spi_flash spiffs
)

# LVGL and minimp3 are pulled in via idf_component.yml (managed components).
# After first `idf.py reconfigure`, the component manager downloads them
# and they become available as REQUIRES dependencies automatically.
