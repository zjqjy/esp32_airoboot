idf_component_register(
    SRCS "main.c" "led_strip_encoder.c"
         "bsp/lcd/lcd_init.c" "bsp/lcd/lcd.c"
         "bsp/lcd/touch.c"
         "bsp/led/led_status.c"
         "cat/cat_player.c" "cat/cat_assets.c"
    INCLUDE_DIRS "." "bsp" "bsp/lcd" "bsp/led" "cat"
    PRIV_REQUIRES esp_driver_rmt esp_driver_spi esp_driver_gpio esp_driver_i2c esp_timer heap
)
