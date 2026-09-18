# golden/ — UI 像素基线（P2b 冻结）

- **冻结日期**：2026-09-17（P2b 首冻；同日 env-footer 变更后守卫重冻，见下）
- **重冻记录**：2026-09-17 底栏新增温湿度槽（SHTC3 环境显示）——页码缩至 w88、
  env 槽 x[100..204]、离线小标 152→124。`GOLDEN_OVERWRITE=1` 守卫流程重生成，
  逐帧机械核对（离线帧 env 区去标矩形后墨点=0 + 在线帧字形墨点≈158）+ VLM 目检
  busy 帧底栏四项无重叠，14/14 PASS + 注入自检通过。
- **内容**：400×300 1-bit 灰度 PNG × 14 帧，模拟器确定性渲染
  （`--clock 21:04 --trend-seed 42 --batt 4.12V --env "24.9C 36%"`
  ，stored zlib 无随机因素，逐字节可复现）
- **格式**：标准 PNG（bit depth 1 / grayscale / filter None / 流式 CRC32），
  单帧 15,368 B。P2a 旧 writer（IDAT CRC 恒 0、8-bit 虚胖 120KB）已于 P2b 修复。

## 回归命令

```sh
tools/golden_run.sh              # 渲染 14 帧 → 与本目录逐像素比对 → N/N PASS 或逐帧差异
tools/golden_run.sh --selftest   # 注入自检：篡改本目录副本单像素，必须 FAIL 检出
```

## 重生成命令（红线：永不自动覆盖）

```sh
# ① 渲染候选基线到临时目录，逐帧目检：
tools/golden_run.sh --render-only /tmp/golden-candidate
# ② 确认 UI 变更有据（任务卡/用户拍板）后，显式覆盖：
GOLDEN_OVERWRITE=1 tools/golden_freeze.sh
```

跑挂时的正确动作：修代码，或登记差异裁决。**禁止重生成基线消除失败。**

## 帧清单（14 = 8 基础 + 6 变体）

| 帧 | fixture | state | 页 | 额外参数 | 钉住的行为 |
|---|---|---|---|---|---|
| page1-normal.png | stats.normal.json | normal | 1 | — | GPU 页全量基线 |
| page2-normal.png | stats.normal.json | normal | 2 | — | SYS 页全量基线 |
| page1-offline.png | stats.normal.json | offline | 1 | — | 离线仅底栏「离线」黑标（x124），env 槽让位清空，页面/数据不接管 |
| page2-offline.png | stats.normal.json | offline | 2 | — | 同上（SYS 页） |
| page1-nodriver.png | stats.nodriver.json | nodriver | 1 | — | gpu.driver=false → 右列让位「驱动未装」框、大字 --、走势隐线 |
| page2-nodriver.png | stats.nodriver.json | nodriver | 2 | — | 驱动缺失不影响 SYS 页 |
| page1-alarm.png | stats.normal.json | alarm | 1 | --alarm-temp 87 --alarm-src gpu | 整屏反白警报牌（与 page2-alarm 逐像素相同=设计） |
| page2-alarm.png | stats.normal.json | alarm | 2 | 同上 | 同上 |
| page1-busy.png | stats.normal.json | normal | 1 | --busy | 「渲染中」反白牌（与 normal 差异仅在标题牌区 x[270..329]） |
| page2-busy.png | stats.normal.json | normal | 2 | --busy | 同上 |
| page1-batt-usb.png | stats.normal.json | normal | 1 | --batt USB | 底栏电池位 "USB"（与 normal 差异仅在底栏 y[274..283]） |
| page2-batt-usb.png | stats.normal.json | normal | 2 | --batt USB | 同上 |
| page1-offline-stale.png | stats.live.json | offline | 1 | --last-online 20:58 | 离线沿用 stale 快照照常渲染（live 数据≠normal） |
| page2-offline-stale.png | stats.live.json | offline | 2 | --last-online 20:58 | 同上（SYS 页） |

注：`--last-online` 当前只进 model（rk_ui.h:45），UI 层无渲染路径——真机「离线时钟显
最后成功时刻」是固件宿主策略（firmware/main.c 把 last_online_hhmm 充当 clock_text）。

## 边界 fixtures（protocol/，不在本基线，随取随渲染）

P2b 新增 7 个合成边界变体，均已过模拟器双页烟测（解析+渲染不崩）：
`stats.null-single.json`（gpu.name null→显卡名 "--"）、`stats.null-multi.json`
（fan/load1/swap_used null + 非 wl 网卡→WiFi 标隐藏）、`stats.all-null-temp.json`
（GPU/CPU 温度全 null→大字 "--"）、`stats.vram-full.json`（16/16G 满载条）、
`stats.net-zero.json`（0kbps）、`stats.disk-missing.json`（disks []→两行 DISK --）、
`stats.long-host.json`（host 47 字符 = RK_STR_CAP-1 解析容量上界 + 43 字符显卡名两行换行）。

示例：`SDL_VIDEODRIVER=dummy build/simulator/rig-lookout-sim --fixture protocol/stats.vram-full.json --state normal --page 1 --png /tmp/x.png --clock 21:04 --trend-seed 42 --batt 4.12V`

### 协议层事实（P2b 实测，与任务书措辞的差异）

- **主机名**：任务书预期「超长主机名截断」；实测解析层对超限字符串**越限即拒收**
  （rk_json.c `emit_byte` → RK_PARSE_ERR_SIZE，RK_STR_CAP=48 含 NUL），且 host 字段
  UI 不上屏。故 fixture 取 47 字符容量上界，不取超长值（超长会被拒收进不了渲染）。
- **字符串 null**：数值字段与 `gpu.name` 可 null（→ present=false / 显 "--"）；
  `host`/`net_if` 为 null 会被解析拒收（协议层限制，exporter 侧恒发字符串，不构成风险）。
