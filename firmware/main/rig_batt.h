/*
 * rig_batt.h — 板载 18650 电池电压读取（2026-09-17 用户需求：标题行显电压）。
 */
#ifndef RIG_BATT_H
#define RIG_BATT_H

#include <stdint.h>

#include "esp_err.h"

/* 初始化 ADC1_CH3（GPIO4，1:3 分压）+ curve-fitting 校准。 */
esp_err_t rig_batt_init(void);

/* 读一次（4 次平均、×3 分压还原），返回电芯电压 mV；读数无效返回 -1。
 * <2500mV = 无电池（USB 供电，信使项目同款判定）。 */
int32_t rig_batt_read_mv(void);

#endif /* RIG_BATT_H */
