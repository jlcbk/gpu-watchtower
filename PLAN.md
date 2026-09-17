# 瞭望塔 Lookout — RTX 5060 Ti 主机监控屏

> 状态：策划已批准（2026-09-16），执行中。任务台账见 [STATUS.md](STATUS.md)，红线见 [AGENTS.md](AGENTS.md)。

## 1. 定位

一块常亮常显的桌面仪表屏（Waveshare ESP32-S3-RLCD-4.2，第三块板）：平时瞥一眼显卡温度，渲片夜班时是超温警报灯，顺手看 CPU/内存。沿用「固定骨架 + 状态区切换」设计语言与整屏反白警报模式（信使 Courier 同款视觉语系）。

## 2. 决策记录（2026-09-16 用户拍板）

| 决策点 | 结论 |
|---|---|
| 硬件 | **新购第三块板**，独立项目；1301（Codex 终端）/1605（信使）两块在役板不动 |
| 屏幕风格 | **混合双页**：首页显卡（温度大字+全量+走势线），次页 CPU/内存/系统 |
| 架构 | **ESP32 直连 Ubuntu 轮询**，无 Mac bridge（数据源本身在局域网；Mac 合盖不影响监控） |
| 项目代号 | 瞭望塔 Lookout，repo `rig-lookout` |

## 3. 远程机事实（P0 实测 2026-09-16）

- 主机 `cui-To-Be-Filled-By-O-E-M`，Ubuntu 26.04.1 LTS，kernel 7.0.0-31-generic，IP 192.168.1.12（未变）
- SSH 免密已重建（cui-mac-agent 公钥，密码登录仍开着）
- CPU 6 核，温度传感器 `coretemp`（另有 nvme 44°C 可读）；RAM 37Gi
- 盘：`/` 468G（373G 空闲）；`/mnt/model-library` 466G（129G 空闲，原模型盘）
- **NVIDIA 驱动已装**：595.91.07，RTX 5060 Ti 16G；实测时 GPU 76°C、RAM 已用 27Gi（机器在忙，未干预）
- Python 3.14.4；sudo 可用（密码 sudo）；7779 端口空闲；ufw 已安装（规则部署时处理）
- **网络仍走 USB WiFi**（`wlx502b73c9057e`），有线 `enp0s31f6` DOWN——USB 网卡满载挂死病史（三次实锤），`net_if` 上屏作信号；插有线不归本项目管

## 4. 数据协议

```
GET /health                → {"ok":true,...}         免鉴权，探活
GET /stats   (X-Token 头)  → 快照 JSON               token 错误 403
```

- 导出器 `exporter/server.py`：1s 后台采样线程，HTTP 即时返回；`nvidia-smi` 缺失/失败 → `gpu.driver=false` 全 null（优雅降级）
- 鉴权：恒定时间比较；token 在 `/etc/rig-stats.env`（root:cui 640）
- 暴露面：仅局域网，ufw 只放行 192.168.1.0/24，路由器无端口映射、无公网隧道

快照 schema（**任何字段可 null，固件缺项显 `--` 不崩**，cdt_json 浮点空指针教训）：

```json
{
  "schema": 1, "host": "…", "ts": 1760000000,
  "net_if": "wlx502b73c9057e", "uptime_s": 302400,
  "gpu":  {"name":"RTX 5060 Ti","temp_c":72,"util_pct":98,
           "vram_used_gb":13.2,"vram_total_gb":16.0,
           "power_w":152.3,"power_limit_w":170,"fan_pct":62,"driver":true},
  "cpu":  {"temp_c":41,"util_pct":23,"cores_pct":[20,31,15,42,8,37]},
  "mem":  {"used_gb":18.2,"total_gb":42.6,"util_pct":43,
           "swap_used_gb":0.4,"swap_total_gb":8.0},
  "sys":  {"load1":1.2,"net_rx_kbps":4200,"net_tx_kbps":830,
           "disks":[{"mnt":"/","free_gb":373,"total_gb":468},
                    {"mnt":"/mnt/model-library","free_gb":129,"total_gb":466}]}
}
```

样例 fixtures：`protocol/stats.normal.json`、`protocol/stats.nodriver.json`（离线/超温为固件侧状态，不由数据表达）。

## 5. 屏幕设计（混合双页定稿，400×300 横屏 1-bit）

**首页·显卡（默认常驻）**
- 左侧超大温度数字（一瞥层）；右侧利用率竖/横条
- VRAM 条（used/16G）、功耗 W（含上限）、风扇 %
- 底部 60 分钟温度走势折线，75°C 刻度线标出
- `net_if` 以 `wl*` 开头时显示 `WiFi` 小标记（提醒链路不稳史）

**次页·CPU/内存/系统**
- CPU 总占用+温度，6 核每核小条
- RAM 条（used/37G）、SWAP
- 盘余量（/ 与 /mnt/model-library）、网速 ↑↓、uptime、load1

**交互与异常态**
- BOOT 翻页，纯手动常驻无自动回跳；警报随时抢占，恢复回原页
- 阈值（初值，P4 后按实测调）：GPU ≥75°C 走势线刻度 / ≥80°C 温度区加重（反白小块）/ ≥85°C 持续 30s（迟滞防抖）→ 整屏反白警报牌；CPU ≥85°C 同款
- 离线态：连续超时 → 「主机离线 · 最后在线 hh:mm」，指数退避重连不轰炸
- 驱动未装态：`gpu.driver=false` → 显卡区显「驱动未装」，CPU/内存照常
- 走势数据：12h @30s/点环形缓冲放 PSRAM（1440 点 ≈3KB），显示层取近 60min 切片；实时值仍 2s 轮询刷新

## 6. 固件技术栈

从 codex-desk-terminal **拷贝**（只读引用源项目，绝不改上游）：ST7305 驱动、LVGL 9.3.0 双端锁定 + PC 模拟器、自研 Noto Sans SC 字体导出器（°/⚠ 等码点缺失则在副本内补）。无 TLS/WSS，RAM 压力远小于信使（其 PSRAM 迁移经验备查：DMA 缓冲留内部 RAM 红线）。

## 7. 里程碑

| 卡 | 内容 | 验收（机械可核对） | 执行方 |
|---|---|---|---|
| P0 | SSH 免密重建 + 环境核查 | Mac 免密 `uname` 成功；核查事实落 PLAN §3 | 主力 ✅ 2026-09-16 |
| P1 | rig-stats 导出器部署 | Mac `curl -H X-Token /stats` 得 sane JSON；无驱动降级不报错；`systemctl is-enabled` = enabled；错 token 得 403 | 主力 |
| P2a | 固件骨架移植 + 模拟器双页四态 | 见 `tasks/P2a-firmware-skeleton.md` | 子代理（重卡，派前查额度） |
| P2b | fixtures 全集 + golden 像素回归 | golden N/N 零差异；golden 永不自动覆盖 | 子代理 |
| P3 | 板子到货、烧录、桌面联调 | 屏显实时跟踪；WiFi RSSI 记录；2s 轮询稳定 | 等快递，不阻塞 P1/P2 |
| P4 | 真实工况验收 + 夜班 soak | 挂渲片任务 30-60min 屏上走势 vs nvidia-smi 对账；soak 一晚（哨兵子代理盯串口落盘只报终态） | 主力验收 + 哨兵 |
| P5 | 打磨：阈值/全刷频率/支架 | 用户肉眼验收 | 按需 |

## 8. 风险与悬置

- **USB WiFi**：仍是现役链路，满载挂死史——屏上「主机离线」本身即有价值信号；插有线属系统恢复线，不归本项目
- **板子未购**：P3 阻塞项，用户下单即可，P1/P2 不受影响
- 阈值初值基于历史正常区间（满载 60-76°C），P4 实测后调
- 温度符号等码点是否在 857 字符集内：P2a 核查
