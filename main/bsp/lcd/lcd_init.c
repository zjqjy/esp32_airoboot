/*
 * ST7789 裸驱动实现 (S2-A)
 * 学习要点:
 *  1. 4 线 SPI: DC 线区分"指令/参数", CS 由 spi_master 驱动托管
 *  2. 初始化序列每一条都有物理含义, 逐条注释 → 视频讲解底稿
 *  3. 像素搬运: 短参数走 polling(≤64B 限制), 行像素走 DMA
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lcd_init.h"

static const char *TAG = "lcd_init";

static spi_device_handle_t spi;
static uint8_t *line_buf;    /* 一行像素的 DMA 缓冲, 按字节大端组装 (LCD_W*2) */

/* ---------- 底层三件套 ---------- */

/* 发送前统一走这里: 先切 DC, 再交 SPI. cmd=短收尾(1B), 参数=DMA 视长度 */
static inline void spi_send(const void *buf, size_t len, bool dma)
{
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = buf,
    };
    esp_err_t err = dma ? spi_device_transmit(spi, &t)
                        : spi_device_polling_transmit(spi, &t);
    ESP_ERROR_CHECK(err);
}

void LCD_WriteCmd(uint8_t cmd)
{
    gpio_set_level(LCD_DC_PIN, 0);          /* DC=0: 这是指令 */
    spi_send(&cmd, 1, false);
}

void LCD_WriteData8(uint8_t d)
{
    gpio_set_level(LCD_DC_PIN, 1);          /* DC=1: 这是参数 */
    spi_send(&d, 1, false);
}

void LCD_WriteData16(uint16_t d)
{
    uint8_t b[2] = { d >> 8, d & 0xFF };    /* SPI 大端: 高字节在前 */
    gpio_set_level(LCD_DC_PIN, 1);
    spi_send(b, 2, false);
}

void LCD_SendPixels(const uint16_t *px, uint32_t n)
{
    gpio_set_level(LCD_DC_PIN, 1);
    spi_send(px, n * 2, true);              /* 行像素走 DMA */
}

/* ---------- 显存窗口 ----------
 * ST7789 显存 240x320, 屏幕只露出其中一块. 设窗口=告诉控制器
 * 接下来的像素流按 [x1..x2]x[y1..y2] 逐行填充. 这是所有刷屏的基础.
 */
void LCD_SetWindow(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    y1 += LCD_Y_OFFSET;                     /* 可视窗偏移: 玻璃不在显存 0 行开始 */
    y2 += LCD_Y_OFFSET;
    LCD_WriteCmd(0x2A);                     /* CASET: 列地址 */
    LCD_WriteData16(x1);
    LCD_WriteData16(x2);
    LCD_WriteCmd(0x2B);                     /* RASET: 行地址 */
    LCD_WriteData16(y1);
    LCD_WriteData16(y2);
    LCD_WriteCmd(0x2C);                     /* RAMWR: 开始写显存 */
}

/* ---------- 初始化序列 ----------
 * 移植自例程 LCD_Init(), 这组参数已被 P169H002 实际点亮验证.
 */
void LCD_Init(void)
{
    /* 1. DC/RES/BLK 输出脚 (CS 交给 SPI 驱动托管, 不能重复配置) */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LCD_DC_PIN) | (1ULL << LCD_RES_PIN) |
                        (1ULL << LCD_BLK_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);

    /* 2. SPI 总线: 只写无 MISO, DMA 自动分配 */
    spi_bus_config_t bus = {
        .mosi_io_num = LCD_MOSI_PIN,
        .miso_io_num = -1,
        .sclk_io_num = LCD_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    /* 3. 挂载 ST7789: mode0, 片选交给驱动 */
    spi_device_interface_config_t dev = {
        .clock_speed_hz = LCD_SPI_MHZ * 1000 * 1000,
        .mode = 0,
        .spics_io_num = LCD_CS_PIN,
        .queue_size = 7,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_SPI_HOST, &dev, &spi));

    /* 4. 行缓冲放内部 RAM( DMA 要求 ), 480 字节.
     *    ⚠️ 字节序坑: uint16_t 直接 DMA 会按小端发(低字节先),
     *    ST7789 要求大端(高字节先) → 缓冲用 uint8_t 手工组装 [H,L],
     *    错误症状: 红→蓝, 绿→红, 蓝→绿 (S2-A 实测踩坑) */
    line_buf = heap_caps_malloc(LCD_W * 2, MALLOC_CAP_DMA);
    assert(line_buf);

    /* 5. 硬复位 */
    gpio_set_level(LCD_RES_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(LCD_RES_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* 6. 初始化序列(例程同款, 每条注释物理含义) */
    LCD_WriteCmd(0x11);                     /* Sleep Out: 退出睡眠, 必须 delay 120ms */
    vTaskDelay(pdMS_TO_TICKS(120));

    LCD_WriteCmd(0x36);                     /* MADCTL: 扫描方向/镜像/RGB顺序 */
    LCD_WriteData8(0x00);                   /* 0x00 竖屏(对齐参考代码), 旋转表见 S2-D */

    LCD_WriteCmd(0x3A);                     /* COLMOD: 像素格式 */
    LCD_WriteData8(0x05);                   /* 0x05 = 16bit/px RGB565 */

    LCD_WriteCmd(0xB2);                     /* Porch: 前/后肩时序 */
    LCD_WriteData8(0x0C);
    LCD_WriteData8(0x0C);
    LCD_WriteData8(0x00);
    LCD_WriteData8(0x33);
    LCD_WriteData8(0x33);

    LCD_WriteCmd(0xB7);                     /* GCTRL: 栅极电压 VGH/VGL */
    LCD_WriteData8(0x35);

    LCD_WriteCmd(0xBB);                     /* VCOMS: 公共电压 1.35V */
    LCD_WriteData8(0x32);

    LCD_WriteCmd(0xC2);                     /* VRH: 源极基准 */
    LCD_WriteData8(0x01);

    LCD_WriteCmd(0xC3);                     /* GVDD: 4.8V, 过低发暗/过高发灰 */
    LCD_WriteData8(0x15);

    LCD_WriteCmd(0xC4);                     /* VDV: 0x20 = 0V */
    LCD_WriteData8(0x20);

    LCD_WriteCmd(0xC6);                     /* 帧率: 0x0F = 60Hz */
    LCD_WriteData8(0x0F);

    LCD_WriteCmd(0xD0);                     /* 电源控制: AVDD/AVCL 等 */
    LCD_WriteData8(0xA4);
    LCD_WriteData8(0xA1);

    LCD_WriteCmd(0xE0);                     /* Gamma 正极性 (发色/灰阶曲线) */
    const uint8_t gam_p[14] = { 0xD0,0x08,0x0E,0x09,0x09,0x05,0x31,
                                0x33,0x48,0x17,0x14,0x15,0x31,0x34 };
    for (int i = 0; i < 14; i++) LCD_WriteData8(gam_p[i]);

    LCD_WriteCmd(0xE1);                     /* Gamma 负极性 */
    const uint8_t gam_n[14] = { 0xD0,0x08,0x0E,0x09,0x09,0x15,0x31,
                                0x33,0x48,0x17,0x14,0x15,0x31,0x34 };
    for (int i = 0; i < 14; i++) LCD_WriteData8(gam_n[i]);

    LCD_WriteCmd(0x21);                     /* INVON: 反显(这块 IPS 屏必须开, 否则颜色发白) */
    LCD_WriteCmd(0x29);                     /* DISPON: 开显示 */

    LCD_Backlight(true);                    /* 最后开背光, 避免看到初始化花屏 */
    ESP_LOGI(TAG, "ST7789 init done (SPI%d, %dMHz, RST=%d)",
             LCD_SPI_HOST + 1, LCD_SPI_MHZ, LCD_RES_PIN);
}

uint8_t *LCD_GetLineBuf(void)
{
    return line_buf;
}

void LCD_Backlight(bool on)
{
    gpio_set_level(LCD_BLK_PIN, on ? 1 : 0);
}
