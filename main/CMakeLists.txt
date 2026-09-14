idf_component_register(
    SRCS "main.c" "led_strip_encoder.c"
         "bsp/lcd/lcd_init.c" "bsp/lcd/lcd.c"
         "bsp/lcd/touch.c"
    INCLUDE_DIRS "." "bsp" "bsp/lcd"
    PRIV_REQUIRES esp_driver_rmt esp_driver_spi esp_driver_gpio esp_driver_i2c
)
