/*
 * rig_audio.h — 板载音频子系统休眠（ES8311/ES7210/功放；监控屏零音频用途）。
 * 时序源：Waveshare ESP32-S3-RLCD-4.2 官方仓 esp_codec_dev（Apache-2.0）。
 * 调用前提：rig_env_init 已成功（共用 SDA13/SCL14 总线）。
 * 将来音频功能（警报发声）经本模块 wake 路径反向展开。
 */
#ifndef RIG_AUDIO_H
#define RIG_AUDIO_H

#include "esp_err.h"

/* 三家全部软件关断。返回 ESP_OK=全部成功；失败位经 audio 事件上报。 */
esp_err_t rig_audio_sleep(void);

#endif /* RIG_AUDIO_H */
