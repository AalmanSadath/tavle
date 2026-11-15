idf_component_register(
    SRCS "main.c" "display.c" "layout.c" "widgets.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "layout.json"
    REQUIRES esp_driver_spi esp_driver_gpio esp_lcd esp_timer json
)
