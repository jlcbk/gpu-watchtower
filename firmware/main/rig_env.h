/*
 * rig_env.h — 板载 SHTC3 温湿度（2026-09-18 用户需求：底栏显示，低刷新率）。
 * 协议常量冻结自信使 H03（vendor 示例逐条核对 + CRC 注入测试过的实现）。
 */
#ifndef RIG_ENV_H
#define RIG_ENV_H

#include "driver/i2c_master.h"
#include "esp_err.h"

/* 初始化 I2C1（SDA=13/SCL=14，与 RTC 共线的板载总线）+ SHTC3 设备句柄。 */
esp_err_t rig_env_init(void);

/* 板载 I2C 总线句柄（rig_env_init 成功后有效；音频子系统等共线设备用）。 */
i2c_master_bus_handle_t rig_env_i2c_bus(void);

/* 单次测量：唤醒 → MEAS_T_RH_POLLING(~12ms) → 读 6B（CRC 校验）→ 转换 → 睡。
 * 成功写出温度 °C 与湿度 %。每次尝试后必发 SLEEP（传感器 µA 级休眠态）。 */
esp_err_t rig_env_read(float *temp_c, float *rh_pct);

#endif /* RIG_ENV_H */
