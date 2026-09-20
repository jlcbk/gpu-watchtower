/*
 * rig_audio.c — 板载音频子系统休眠（P5Y：外围省电第一仗，2026-09-19）。
 *
 * 监控屏永不使用音频，但 ES8311 codec / ES7210 ADC / 喇叭功放三件上电后
 * 处于默认态持续耗电（估 3-8mA，占 P5X 时代 ~15mA「去向不明地板」大头）。
 * 本模块开机即把三家全部送进软件关断：
 *   - ES8311 (I2C 0x18)：官方 esp_codec_dev es8311_suspend 的 15 步掉电时序
 *   - ES7210 (I2C 0x40)：官方 es7210_stop 的 9 步（麦克风供电+时钟+芯片级 power down）
 *   - 功放 PA_EN (GPIO46)：拉低关断（微雪 codec_board/board_cfg.h: pa=46）
 * 寄存器时序逐字取自 Waveshare ESP32-S3-RLCD-4.2 官方仓库内置的
 * esp_codec_dev（Apache-2.0），未凭记忆杜撰。
 *
 * A/B 判据：电流表 mAh 均值法，差值 >2mA 固化，否则查其余外围。
 * 将来音频功能（警报发声等）从本模块的 wake 路径反向展开。
 */
#include "rig_audio.h"

#include <stdint.h>

#include "freertos/FreeRTOS.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

#include "rig_env.h"
#include "rig_ev.h"

#define TAG "rig_audio"

#define ES8311_ADDR 0x18
#define ES7210_ADDR 0x40
#define AUDIO_PA_GPIO 46 /* 板载功放使能（高=开）；拉低省 2-4mA */

/* ES8311 软件掉电（源：esp_codec_dev/device/es8311/es8311.c es8311_suspend） */
static const uint8_t s_es8311_down[][2] = {
    { 0x32, 0x00 }, /* DAC_REG32 */
    { 0x17, 0x00 }, /* ADC_REG17 */
    { 0x0E, 0xFF }, /* SYSTEM_REG0E */
    { 0x12, 0x02 }, /* SYSTEM_REG12 */
    { 0x14, 0x00 }, /* SYSTEM_REG14 */
    { 0x0D, 0xFA }, /* SYSTEM_REG0D */
    { 0x15, 0x00 }, /* ADC_REG15 */
    { 0x02, 0x10 }, /* CLK_MANAGER_REG02 */
    { 0x00, 0x00 }, /* RESET_REG00 */
    { 0x00, 0x1F }, /* RESET_REG00 */
    { 0x01, 0x30 }, /* CLK_MANAGER_REG01 */
    { 0x01, 0x00 }, /* CLK_MANAGER_REG01 */
    { 0x45, 0x00 }, /* GP_REG45 */
    { 0x0D, 0xFC }, /* SYSTEM_REG0D */
    { 0x02, 0x00 }, /* CLK_MANAGER_REG02 */
};

/* ES7210 停机（源：esp_codec_dev/device/es7210/es7210.c es7210_stop） */
static const uint8_t s_es7210_down[][2] = {
    { 0x47, 0xFF }, /* MIC1_POWER */
    { 0x48, 0xFF }, /* MIC2_POWER */
    { 0x49, 0xFF }, /* MIC3_POWER */
    { 0x4A, 0xFF }, /* MIC4_POWER */
    { 0x4B, 0xFF }, /* MIC12_POWER */
    { 0x4C, 0xFF }, /* MIC34_POWER */
    { 0x40, 0xC0 }, /* ANALOG_REG40 */
    { 0x01, 0x7F }, /* CLOCK_OFF_REG01 */
    { 0x06, 0x07 }, /* POWER_DOWN_REG06（芯片级） */
};

static esp_err_t write_seq(i2c_master_dev_handle_t dev, const uint8_t (*seq)[2], int n)
{
    esp_err_t ret = ESP_OK;
    for (int i = 0; i < n; i++) {
        uint8_t b[2] = { seq[i][0], seq[i][1] };
        esp_err_t err = i2c_master_transmit(dev, b, sizeof b, pdMS_TO_TICKS(20));
        if (err != ESP_OK) {
            ret = err;
        }
    }
    return ret;
}

esp_err_t rig_audio_sleep(void)
{
    i2c_master_bus_handle_t bus = rig_env_i2c_bus();
    if (bus == NULL) {
        return ESP_ERR_INVALID_STATE; /* 总线没起来（rig_env_init 失败）→ 放弃 */
    }

    /* 功放先关（独立于 codec 的 GPIO 路径，两路互不拖累） */
    gpio_config_t pa = {
        .pin_bit_mask = 1ULL << AUDIO_PA_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pa);
    gpio_set_level((gpio_num_t)AUDIO_PA_GPIO, 0);
    int pa_ok = 1;

    i2c_master_dev_handle_t es8311 = NULL, es7210 = NULL;
    i2c_device_config_t dc8311 = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8311_ADDR,
        .scl_speed_hz = 100000,
    };
    i2c_device_config_t dc7210 = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES7210_ADDR,
        .scl_speed_hz = 100000,
    };
    i2c_master_bus_add_device(bus, &dc8311, &es8311);
    i2c_master_bus_add_device(bus, &dc7210, &es7210);

    int es8311_ok = 0, es7210_ok = 0;
    if (es8311 != NULL && write_seq(es8311, s_es8311_down,
                                    sizeof s_es8311_down / sizeof s_es8311_down[0]) == ESP_OK) {
        es8311_ok = 1;
    }
    if (es7210 != NULL && write_seq(es7210, s_es7210_down,
                                    sizeof s_es7210_down / sizeof s_es7210_down[0]) == ESP_OK) {
        es7210_ok = 1;
    }

    ESP_LOGI(TAG, "audio subsystem down: es8311=%d es7210=%d pa_gpio%d=0",
             es8311_ok, es7210_ok, AUDIO_PA_GPIO);
    rig_ev("audio", "down=%d%d%d", es8311_ok, es7210_ok, pa_ok);
    return (es8311_ok && es7210_ok && pa_ok) ? ESP_OK : ESP_FAIL;
}
