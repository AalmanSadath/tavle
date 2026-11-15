idf_component_register(
    SRCS "main.c" "display.c" "layout.c" "widgets.c" "settings.c" "net.c" "web.c" "ui.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "layout.json" "web/setup.html"
    REQUIRES esp_driver_spi esp_driver_gpio esp_lcd esp_timer json nvs_flash esp_wifi
             esp_netif esp_event esp_http_server app_update esp_app_format mbedtls dns_server
)
