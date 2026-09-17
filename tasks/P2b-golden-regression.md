# P2b 任务书：fixtures 全集 + golden 像素回归（子代理执行卡）

## 目标（一句话）

为 rig-lookout 当前定稿 UI 建立 golden 基线与回归门禁：fixtures 全集 × 页面 × 状态逐像素对比，一条命令全绿或精确报差异；故意篡改必须能被抓住。

## 冻结对象（UI 现状，2026-09-17 定稿，历经四轮真机反馈收敛）

- 双页：P1 显卡（大温度字/显卡名两行换行/右列四行条 ys=40/76/112/148/走势区 scale 20..95°C 含 75°C 虚线刻度）；P2 系统（CPU 单条+RAM/SWAP 三大条/盘纯数值行/NET 10K 量化）
- 标题行：LOOKOUT - GPU/SYS + 「渲染中」反白牌（model.gpu_busy 时亮）+ WiFi 反白标（net_if 为 wl* 时亮）
- 底栏：页码 · 「离线」黑标（仅 OFFLINE 态亮，页面不接管、数据沿用快照） · 电池位（model.batt_text）· 时钟
- 模拟器入口：`simulator/main.c`，flags：`--fixture <json> --state normal|offline|nodriver|alarm --page 1|2 --batt <TEXT> --busy --trend-seed <N> --clock <HH:MM> --last-online --png <out>`
- 已有导出脚本：`tools/export_p2a.sh`（旧 8 帧，可参考/改造）

## 必读指针

1. `/Users/cui/Documents/Projects/rig-lookout/PLAN.md` §4/§5
2. `/Users/cui/Documents/Projects/rig-lookout/tasks/P2a-firmware-skeleton.md`（上一卡背景）
3. `/Users/cui/Documents/Projects/rig-lookout/reports/p2a-acceptance.md` — 已知坑（PNG 编码问题）
4. `/Users/cui/Documents/Projects/rig-lookout/shared/ui/rk_ui.c` — UI 真源（布局常量/模型字段）

## 工作范围

1. **PNG 编码规范化**：当前模拟器 PNG writer 输出非常规（macOS Read 工具/部分解码器拒收、单帧 120KB 虚胖）。优先修 writer 产出标准 PNG；若 writer 修复代价过大，回归管线内统一重编码（macOS `sips` 可用）后落盘比对，保证逐字节稳定
2. **fixtures 全集**（`protocol/` 下，合成数据、数值固定；现已有 normal/nodriver/live 样例）：补边界变体 ≥6 个——null 单字段/多字段、全 null 温度、VRAM 满载、网络 0kbps、磁盘缺失、超长主机名截断
3. **golden 基线**：矩阵 ≥14 帧（页面×四态 8 帧基础 + 变体：busy 亮×2 页、batt=USB×2 页、offline 沿用 stale 快照×2 页），确定性参数（--clock 21:04 --trend-seed 42 --batt 4.12V），导出后**人工质检再冻结**
4. **回归 runner**：单条命令（脚本落 `tools/golden_run.sh`）跑全集 → 与 golden 逐像素对比 → `N/N PASS` 或逐帧差异报告（差异像素数+首个差异坐标）
5. **注入自检**：篡改一帧 golden 的一个像素 → runner 必须 FAIL 检出（防「假通过」）；把篡改/复测输出摘录写进汇报

## 验收标准（机械可核对）

1. `tools/golden_run.sh` 对当前代码输出 `N/N PASS`（N=实际帧数）
2. 注入测试证据：篡改前后输出对比
3. `golden/README.md` 含冻结日期、重生成命令、帧清单；帧文件在 `golden/`
4. 红线核对：codex-desk-terminal 与 hermes-courier 无新增脏（hermes 已知 2 条基线脏除外，见 STATUS.md）
5. 汇报：路径清单、命令、注入测试摘录、遗留问题

## 红线

- **golden 永不自动覆盖**：跑挂了就修代码或登记差异，「重生成 golden 让它变绿」即失败
- 不改 `shared/ui/`（本卡冻结 UI，不改 UI；发现 UI 疑似 bug 报告给主力裁决，不擅动）
- 上游两 repo 只读；不烧录；不动 192.168.1.12；不动 `exporter/`（主力正并行改导出器）
- 不引入新框架；LVGL 锁 9.3.0
