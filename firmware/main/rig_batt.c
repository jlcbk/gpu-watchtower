/*
 * rig_batt.c — 电池电压采样（适配 Waveshare 官方 ESP-IDF 03_ADC_Test
 * adc_bsp.cpp 的校准路径：ADC1_CH3 / DB_12 / 12bit / curve-fitting / ×3）。
 * 采样与判定节律沿用信使 H02（4 读平均；<2.5V=USB 电压推断，无独立在位检测）。
 */
#include "rig_batt.h"

#include "esp_log.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#define TAG "rig_batt"

#define BATT_ADC_CHANNEL ADC_CHANNEL_3 /* GPIO4 = ADC1_CH3，1:3 分压 */
#define BATT_DIVIDER_X3  3
#define BATT_READS       4

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;

esp_err_t rig_batt_init(void)
{
    adc_cali_curve_fitting_config_t cc = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    esp_err_t err = adc_cali_create_scheme_curve_fitting(&cc, &s_cali);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cali scheme failed: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_unit_init_cfg_t init = {
        .unit_id = ADC_UNIT_1,
    };
    err = adc_oneshot_new_unit(&init, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc unit failed: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t ch = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12,
    };
    err = adc_oneshot_config_channel(s_adc, BATT_ADC_CHANNEL, &ch);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "batt adc ready (ADC1_CH3, x3 divider, %d-read avg)", BATT_READS);
    }
    return err;
}

int32_t rig_batt_read_mv(void)
{
    if (s_adc == NULL || s_cali == NULL) {
        return -1;
    }
    int64_t sum = 0;
    int ok = 0;
    for (int i = 0; i < BATT_READS; i++) {
        int raw = 0, mv = 0;
        if (adc_oneshot_read(s_adc, BATT_ADC_CHANNEL, &raw) == ESP_OK &&
            adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK && mv > 0) {
            sum += mv;
            ok++;
        }
    }
    if (ok == 0) {
        return -1;
    }
    return (int32_t)((sum / ok) * BATT_DIVIDER_X3);
}
