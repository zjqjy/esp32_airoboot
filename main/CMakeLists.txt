# S4 起切换 LVGL+esp_lcd 显示栈:
#   lcd_init.c / lcd.c / cat_player.c / cat_assets.c 移出编译、保留仓库
#   (git 历史可回溯; 避免双驱动抢 SPI 总线)
idf_component_register(
    SRCS "main.c" "led_strip_encoder.c"
         "bsp/lcd_esp/lcd_esp.c"
         "bsp/lcd/touch.c"
         "bsp/led/led_status.c"
         "cat/cat_player_lvgl.c" "cat/cat_lvgl_assets.c"
    INCLUDE_DIRS "." "bsp" "bsp/lcd" "bsp/lcd_esp" "bsp/led" "cat"
    PRIV_REQUIRES esp_driver_rmt esp_driver_spi esp_driver_gpio esp_driver_i2c
                  esp_timer heap esp_lcd lvgl__lvgl espressif__esp_lvgl_port
)
