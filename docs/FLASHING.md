# gpu-watchtower 刷机/恢复指南（写给 Agent）

> 场景：板子被刷成别的固件后要刷回本项目（或全新环境首次部署）。
> 本文假设执行环境 = cui 的 Mac mini（ESP-IDF 已装）。照抄命令即可，不要即兴发挥。
> 硬件：Waveshare ESP32-S3-RLCD-4.2（独板，ESP32-S3，16MB flash）。

## 0. 版本指纹（验证用，不是装饰）

boot 事件（服务端 events.jsonl）带 `fw=Pxxx` 指纹。**当前产线版本：`fw=P5Y`**。
演化史：P5s（哨兵节拍）→ P5L（listen_interval）→ P5U（轻睡总开关）→ P5X（ISR 风暴修复）→ **P5Y（+外围休眠）**。
刷完看事件流里 `fw=` 串就知道板上跑的是哪版——两版同串无法区分的教训已用指纹根治。

## 1. 前置条件（缺一不可）

```sh
# ESP-IDF v5.5.5（本机固定路径）
ls /Users/cui/esp/esp-idf-v5.5.5/export.sh

# 凭据文件（gitignored，不随仓库走；本机已就位）
ls firmware/main/rig_net_config.h
# 若从全新 clone 出发：cp firmware/main/rig_net_config.h.template firmware/main/rig_net_config.h
# 并按模板注释填 WiFi/服务器 IP/token（真实值只在本机文件里，绝不进仓库/日志）

# vendor 依赖（LVGL，不入库）
ls vendor/lvgl >/dev/null 2>&1 || tools/fetch_vendor.sh
```

## 2. 构建

```sh
cd firmware && source /Users/cui/esp/esp-idf-v5.5.5/export.sh && idf.py build
```

产物 `firmware/build/rk_device.bin`（约 4.1MB，分区占用 ~33%）。构建零 error 即可。

## 3. 烧录（本节每一条都是实战换来的，勿跳读）

### 3.1 端口人格

- `/dev/cu.usbmodem1301` = **USB-Serial-JTAG 硬件通道，唯一可烧录人格**；
- `/dev/cu.usbmodem1605` = TinyUSB CDC 应用串口，esptool 打不开，**不能烧**；
- 只见 1605 不见 1301 = 接口掉线（本项目轻睡与 USB 枚举的已知冲突），走 3.2 恢复。

### 3.2 最可靠的烧录入口 = 冷启动（首选）

让用户操作：**拔掉板端 USB → 取出 18650 电池等 10 秒 → 只插回 USB（不按任何键）**。
冷启动后 1301+1605 双双以干净状态枚举，立刻执行 3.3 一次成功。
（BOOT 舞——按住 BOOT 拔插——是备选，但在轻睡固件下命中率不稳；复位卡死态下唯一解永远是物理断电。）

### 3.3 刷写命令（cd 与 esptool 必须同一条命令——shell cwd 会跨调用重置）

```sh
cd /Users/cui/Documents/Projects/rig-lookout/firmware/build \
  && source /Users/cui/esp/esp-idf-v5.5.5/export.sh \
  && python -m esptool --chip esp32s3 -p /dev/cu.usbmodem1301 -b 460800 \
     --before default_reset --after hard_reset write_flash "@flash_args"
```

成功标志：≥3 行 `Hash of data verified` + `Hard resetting via RTS pin...`。
刷前查占用只允许 `lsof -t /dev/cu.usbmodem1301`（**绝不许裸 `lsof -i :端口`**——历史事故）。

### 3.4 全灭救援（可选弹药）

芯片被搞进怪状态（刷了不 boot、复位无效）：`erase_flash` 全片擦除后再 3.3。
本项目凭据编译在固件里、NVS 无持久数据，全擦无损。

## 4. 刷后验证（红线：没做完不算刷完）

1. 等 ~60-90 秒，服务端看信标与指纹：

```sh
ssh cui@192.168.1.12 \
  'grep -a "fw=" /opt/rig-stats/logs/events.jsonl | tail -1; tail -2 /opt/rig-stats/logs/board.jsonl'
```

2. 三项全过才算收工：新 boot 事件 `fw=P5Y`（或当期版本）｜信标 10-11s 间隔恢复｜电压值正常（电池 3.7-4.2V / USB ~4.1V）。
3. 首个事件批还应出现 `audio down=111`（外围关断三位状态，P5Y 起）。
4. 任何 esptool 操作后必须走完本节——「写完就走」曾导致板子滞留下载模式 7 分钟的实录。

## 5. 故障速查

| 症状 | 处置 |
|---|---|
| 只见 1605 | 3.2 冷启动；或 BOOT 舞（拔 USB → 按住 BOOT → 插回 → 1 秒后松开） |
| `No serial data received` | BOOT 舞后芯片已在下载模式却跑复位序列所致：改用 `--before no_reset` 直接写 |
| 刷了不 boot / 信标断流不归 | 3.4 全片擦除重刷；再不行 = 物理断电（拔 USB+取电池 10s）后重来 |
| 控制台 0 字节 | esptool 操作后 USB-SJTAG 控制台失联是已知怪癖，与固件健康无关——验证一律走服务端 |

## 6. 装回电池前的提示

固件行为与供电方式无关（USB 回灌电池槽 ~4.1V，固件按「电池在位」处理，轻睡常开）。
电池模式独有行为：电压 <3.65V×3 → 深睡（屏显「休眠中」，1h 复查/BOOT 即时复查，≥3.75V 复活）。
省电栈现状：闲时 USB 电流 ~11-12mA（≈电池 ~15mA，2500mAh 续航 ~5 天）；探针每 60s 上报 `cpu run=% isr=`，事件通道可远程观察健康度。
