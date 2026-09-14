/*
 * CST816 实现要点:
 *  1. 复位时序: RST 低 10ms → 高 100ms (参考代码同款)
 *  2. 坐标: 从 0x03 连读 4 字节, 12-bit (高字节掩 0x0F)
 *  3. ⚠️ 隐藏偏移: Y +5 (参考代码 TOUCH_OFFSET_Y, 触摸玻璃与 LCD 错位)
 *  4. 触摸判断: FingerNum(0x02) > 0
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "touch.h"

static const char *TAG = "cst816";

static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t i2c_dev;
static uint8_t chip_id;
static SemaphoreHandle_t int_sem;                /* INT 中断 → 任务 的通知桥 */

/* ISR 三原则: 短 / 不阻塞 / IRAM. 只发信号量, 干活留给任务 */
static void IRAM_ATTR touch_isr(void *arg)
{
    BaseType_t hpw = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)arg, &hpw);
    if (hpw) portYIELD_FROM_ISR();
}

/* 写 reg 地址后连读 len 字节 (I2C 复合事务: 写地址+重启+读) */
static esp_err_t reg_read(uint8_t reg, uint8_t *buf, uint8_t len)
{
    return i2c_master_transmit_receive(i2c_dev, &reg, 1, buf, len, 100);
}

/* I2C 总线死锁恢复: 从机拉死 SDA 时手动补 9 个时钟逼其释放, 再补 STOP.
 * 触发场景: 软复位/反复烧录时从机正卡在应答中途 (调试期高发).
 * 必须在 i2c 外设接管引脚之前做 ( peripheral 配置后 GPIO 不归我们管) */
static void i2c_bus_recovery(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << TOUCH_SDA_PIN) | (1ULL << TOUCH_SCL_PIN),
        .mode = GPIO_MODE_OUTPUT_OD,            /* 开漏, 模拟 I2C 电气特性 */
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    gpio_set_level(TOUCH_SCL_PIN, 1);
    gpio_set_level(TOUCH_SDA_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(1));

    if (gpio_get_level(TOUCH_SDA_PIN) == 0) {   /* SDA 被拉死 = 总线僵住 */
        ESP_LOGW(TAG, "I2C SDA 拉死, 执行 9-clock 总线恢复");
        for (int i = 0; i < 9 && gpio_get_level(TOUCH_SDA_PIN) == 0; i++) {
            gpio_set_level(TOUCH_SCL_PIN, 0);
            esp_rom_delay_us(5);
            gpio_set_level(TOUCH_SCL_PIN, 1);
            esp_rom_delay_us(5);
        }
        /* 补一个 STOP: SCL 高电平期间 SDA 由低到高 */
        gpio_set_level(TOUCH_SDA_PIN, 0);
        esp_rom_delay_us(5);
        gpio_set_level(TOUCH_SCL_PIN, 1);
        esp_rom_delay_us(5);
        gpio_set_level(TOUCH_SDA_PIN, 1);
        esp_rom_delay_us(5);
        ESP_LOGI(TAG, "总线恢复完成, SDA=%d", gpio_get_level(TOUCH_SDA_PIN));
    }
}

esp_err_t touch_init(void)
{
    /* 0. 总线死锁恢复 (必须在外设接管前) */
    i2c_bus_recovery();

    /* 1. RST 脚输出并复位 (时序照抄参考代码) */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << TOUCH_RST_PIN,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(TOUCH_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TOUCH_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* 2. 硬件 I2C0: 100kHz, 使能内部上拉 (模块端另有上拉更稳) */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0,
        .sda_io_num = TOUCH_SDA_PIN,
        .scl_io_num = TOUCH_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CST816_ADDR,
        .scl_speed_hz = 100 * 1000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &dev_cfg, &i2c_dev));

    /* 2.5 INT 引脚: 下降沿中断 (CST816 检测到触摸时拉低脉冲) */
    int_sem = xSemaphoreCreateBinary();
    io.pin_bit_mask = 1ULL << TOUCH_INT_PIN;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;          /* 模块端有上拉, 双保险 */
    gpio_config(&io);
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_set_intr_type(TOUCH_INT_PIN, GPIO_INTR_NEGEDGE));
    ESP_ERROR_CHECK(gpio_isr_handler_add(TOUCH_INT_PIN, touch_isr, int_sem));

    /* 3. 读 ChipID 验证通信: CST816S 预期 0xB4 */
    uint8_t reg = CST816_REG_CHIPID;
    esp_err_t err = i2c_master_transmit_receive(i2c_dev, &reg, 1, &chip_id, 1, 100);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ChipID 读取失败: 0x%02X 未响应 (查接线/SDA-TSDA 是否插反)", CST816_ADDR);
        return err;
    }
    ESP_LOGI(TAG, "CST816 ChipID=0x%02X (0xB4=S/0xB5=D 均正常), 触摸 OK", chip_id);
    return ESP_OK;
}

uint8_t touch_chip_id(void)
{
    return chip_id;
}

int touch_int_level(void)                        /* 诊断用: INT 脚原始电平 */
{
    return gpio_get_level(TOUCH_INT_PIN);
}

bool touch_wait(uint32_t timeout_ms)
{
    /* 空闲时任务挂在这, CPU 让给别人, I2C 总线零流量 */
    return xSemaphoreTake(int_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

bool touch_read(uint16_t *x, uint16_t *y)
{
    uint8_t dat[5];
    if (reg_read(CST816_REG_FINGER, dat, 5) != ESP_OK) return false;
    if (dat[0] == 0) return false;              /* FingerNum=0: 无触摸 */

    uint16_t rx = ((dat[1] & 0x0F) << 8) | dat[2];
    uint16_t ry = ((dat[3] & 0x0F) << 8) | dat[4];
    *x = rx;
    *y = ry + 5;                                /* 玻璃错位补偿 (参考代码同款) */
    return true;
}
