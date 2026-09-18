/*
 * rig_env.c — SHTC3 温湿度采样（适配自信使 H03；纯 ESP-IDF 新 I2C 驱动）。
 * 协议：地址 0x70；WAKEUP 0x3517 / SLEEP 0xB098 / MEAS_T_RH_POLLING 0x7866
 * （无时钟拉伸：测量期间 NACK 读，轮询窗口 ≤16ms 有界）；CRC-8 poly 0x31
 * init 0xFF；换算 T=-45+175*raw/65535, RH=100*raw/65535。
 */
#include "rig_env.h"

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

#define TAG "rig_env"

#define ENV_I2C_SDA   13
#define ENV_I2C_SCL   14
#define ENV_I2C_ADDR  0x70
#define ENV_CMD_WAKEUP 0x3517
#define ENV_CMD_SLEEP  0xB098
#define ENV_CMD_MEAS   0x7866 /* MEAS_T_RH_POLLING（无时钟拉伸） */

static i2c_master_dev_handle_t s_dev;

static esp_err_t send_cmd(uint16_t cmd)
{
    uint8_t b[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
    return i2c_master_transmit(s_dev, b, sizeof b, pdMS_TO_TICKS(20));
}

static uint8_t crc8(const uint8_t *d, int len)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

esp_err_t rig_env_init(void)
{
    i2c_master_bus_config_t bc = {
        .i2c_port = -1, /* 自动分配空闲端口 */
        .sda_io_num = ENV_I2C_SDA,
        .scl_io_num = ENV_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
    };
    i2c_master_bus_handle_t bus;
    esp_err_t err = i2c_new_master_bus(&bc, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus failed: %s", esp_err_to_name(err));
        return err;
    }
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ENV_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(bus, &dc, &s_dev);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "shtc3 ready (i2c sda%d scl%d addr 0x%02x)",
                 ENV_I2C_SDA, ENV_I2C_SCL, ENV_I2C_ADDR);
    }
    return err;
}

esp_err_t rig_env_read(float *temp_c, float *rh_pct)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (send_cmd(ENV_CMD_WAKEUP) != ESP_OK) {
        send_cmd(ENV_CMD_SLEEP); /* 唤醒都 NACK：仍补发睡眠（尽力） */
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(1)); /* 唤醒时延 ≤240µs，取 1ms */

    if (send_cmd(ENV_CMD_MEAS) != ESP_OK) {
        send_cmd(ENV_CMD_SLEEP);
        return ESP_FAIL;
    }
    uint8_t b[6];
    bool ok = false;
    vTaskDelay(pdMS_TO_TICKS(10)); /* 正常测量 ~12ms，测量期读=NACK */
    for (int i = 0; i < 6 && !ok; i++) {
        if (i2c_master_receive(s_dev, b, sizeof b, pdMS_TO_TICKS(20)) == ESP_OK) {
            ok = true;
        } else {
            vTaskDelay(pdMS_TO_TICKS(1)); /* 轮询窗口合计 ≤16ms 有界 */
        }
    }
    send_cmd(ENV_CMD_SLEEP); /* 每次尝试后必睡（µA 级休眠态） */
    if (!ok) {
        return ESP_FAIL;
    }
    if (crc8(b, 2) != b[2] || crc8(b + 3, 2) != b[5]) {
        return ESP_ERR_INVALID_CRC;
    }
    uint16_t traw = (uint16_t)((b[0] << 8) | b[1]);
    uint16_t hraw = (uint16_t)((b[3] << 8) | b[4]);
    if (temp_c != NULL) *temp_c = -45.0f + 175.0f * traw / 65535.0f;
    if (rh_pct != NULL) *rh_pct = 100.0f * hraw / 65535.0f;
    return ESP_OK;
}
