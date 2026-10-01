/*
 * esp_lcd 面板层实现 (S4-A)
 * 厂商初始化序列来源: S2-A lcd_init.c (P169H002 例程, 已上板验证), 逐条注释见彼处.
 * esp_lcd st7789 驱动只发 SLPOUT/MADCTL/COLMOD/RAMCTRL, gamma/porch 等
 * 厂商参数在 panel_init 后用 tx_param 补发 — 时序与 S2-A 等价:
 *   S2-A: SW序列 → SLPOUT → MADCTL(0x00) → COLMOD(0x05) → 厂商段 → INVON → DISPON → 背光
 *   S4:   RST → init(SLPOUT+MADCTL+COLMOD+RAMCTRL小端) → 厂商段 → INVON → gap → DISPON → 背光
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "lcd_esp.h"

static const char *TAG = "lcd_esp";

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;

/* S2-A 验证过的厂商参数段 (LCD_Init 第 134-171 行逐字节照搬) */
static void vendor_tune(esp_lcd_panel_io_handle_t io)
{
    const uint8_t gam_p[14] = { 0xD0,0x08,0x0E,0x09,0x09,0x05,0x31,
                                0x33,0x48,0x17,0x14,0x15,0x31,0x34 };
    const uint8_t gam_n[14] = { 0xD0,0x08,0x0E,0x09,0x09,0x15,0x31,
                                0x33,0x48,0x17,0x14,0x15,0x31,0x34 };
    esp_lcd_panel_io_tx_param(io, 0xB2, (uint8_t[]) { 0x0C, 0x0C, 0x00, 0x33, 0x33 }, 5);
    esp_lcd_panel_io_tx_param(io, 0xB7, (uint8_t[]) { 0x35 }, 1);
    esp_lcd_panel_io_tx_param(io, 0xBB, (uint8_t[]) { 0x32 }, 1);   /* VCOMS 1.35V */
    esp_lcd_panel_io_tx_param(io, 0xC2, (uint8_t[]) { 0x01 }, 1);
    esp_lcd_panel_io_tx_param(io, 0xC3, (uint8_t[]) { 0x15 }, 1);   /* GVDD 4.8V */
    esp_lcd_panel_io_tx_param(io, 0xC4, (uint8_t[]) { 0x20 }, 1);
    esp_lcd_panel_io_tx_param(io, 0xC6, (uint8_t[]) { 0x0F }, 1);   /* 60Hz */
    esp_lcd_panel_io_tx_param(io, 0xD0, (uint8_t[]) { 0xA4, 0xA1 }, 2);
    esp_lcd_panel_io_tx_param(io, 0xE0, gam_p, 14);
    esp_lcd_panel_io_tx_param(io, 0xE1, gam_n, 14);
}

void lcd_esp_backlight(bool on)
{
    gpio_set_level(LCD_ESP_BLK_PIN, on ? 1 : 0);
}

esp_err_t lcd_esp_init(void)
{
    /* 1. 背光脚 (最后才拉高, 避免看到初始化花屏) */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << LCD_ESP_BLK_PIN,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "blk gpio");

    /* 2. SPI 总线: 只写无 MISO, DMA 自动 (与 S2-A 同参) */
    spi_bus_config_t bus = {
        .mosi_io_num = LCD_ESP_MOSI_PIN,
        .miso_io_num = -1,
        .sclk_io_num = LCD_ESP_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_ESP_W * 80 * 2,  /* LVGL 双缓冲单块 240x80 上限 */
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_ESP_SPI_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG, "spi bus");

    /* 3. panel_io: 8bit 命令/参数, 异步队列 10 深.
     *    on_color_trans_done 留空 — esp_lvgl_port 在 add_disp 时自行注册回调 */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_ESP_CS_PIN,
        .dc_gpio_num = LCD_ESP_DC_PIN,
        .spi_mode = 0,
        .pclk_hz = LCD_ESP_SPI_MHZ * 1000 * 1000,
        .trans_queue_depth = 10,
        .on_color_trans_done = NULL,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_ESP_SPI_HOST,
                                                 &io_cfg, &s_io), TAG, "panel io");

    /* 4. st7789 面板: MADCTL=RGB(等效 S2-A 的 0x00)。
     *    字节序: 本屏控制器忽略 RAMCTRL(0xB0) 小端位 (S4 上板实测: 粉猫变绿 =
     *    字节交换特征; data_endian=LITTLE 无效), 面板按大端解释 →
     *    LVGL 原生小端缓冲由 esp_lvgl_port 的 swap_bytes 在 flush 时软件交换 */
    esp_lcd_panel_dev_config_t dev_cfg = {
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
        .reset_gpio_num = LCD_ESP_RES_PIN,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_io, &dev_cfg, &s_panel), TAG, "st7789");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");      /* SLPOUT+时序 */
    vendor_tune(s_io);                                                  /* S2-A 厂商段 */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invon");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, 0, LCD_ESP_Y_GAP), TAG, "gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "dispon");

    lcd_esp_backlight(true);
    ESP_LOGI(TAG, "ST7789 ready (esp_lcd, SPI%d @%dMHz, gap_y=%d)",
             LCD_ESP_SPI_HOST + 1, LCD_ESP_SPI_MHZ, LCD_ESP_Y_GAP);
    return ESP_OK;
}

esp_lcd_panel_io_handle_t lcd_esp_io(void)
{
    return s_io;
}

esp_lcd_panel_handle_t lcd_esp_panel(void)
{
    return s_panel;
}
