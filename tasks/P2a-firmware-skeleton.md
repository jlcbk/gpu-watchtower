# P2a 任务书：固件骨架移植 + 模拟器双页四态（子代理执行卡）

## 目标（一句话）

在 `/Users/cui/Documents/Projects/rig-lookout/` 下产出可构建的 ESP32-S3 固件骨架与 Mac 端模拟器，用 fixtures 数据渲染「混合双页 × 四态」共 8 帧画面并导出 PNG。

## 必读指针（按序，不抄内容只指向）

1. `/Users/cui/Documents/Projects/rig-lookout/PLAN.md` — §4 数据协议（schema/样例路径）、§5 屏幕设计（双页布局/阈值/警报/异常态/走势线）、§6 技术栈
2. `/Users/cui/Documents/Projects/rig-lookout/protocol/stats.normal.json` 与 `stats.nodriver.json` — 数据样例（离线态=取数失败、超温警报态=温度阈值触发，均为固件侧状态）
3. `/Users/cui/Documents/Projects/codex-desk-terminal/AGENTS.md` + `docs/INTERFACES.md` — 源项目纪律与模块边界（只读参考）
4. `/Users/cui/Documents/Projects/codex-desk-terminal/firmware/`、同级 `simulator/`、`tools/`（字体导出器）、`shared/`、`protocol/` — 拷贝源
5. ESP-IDF 环境：从 codex-desk-terminal 的构建脚本/文档里找本机既有 idf 环境（版本与源项目一致；**不要**自行下载安装新版本）

## 工作范围

- 新建 `rig-lookout/firmware/`：从源项目**拷贝**改造 ST7305 驱动、显示抽象、字体资产；`°`、`⚠` 等码点若缺失，用其 tools 字体导出器在 rig-lookout 副本内补齐
- 新建 `rig-lookout/simulator/`：Mac 端渲染同一套 UI 代码（LVGL 9.3.0 双端锁定，与源项目一致）
- UI 按 PLAN §5：首页（超大温度数字+util/VRAM/功耗/风扇+底部 60min 走势线）、次页（CPU/内存/盘/网/uptime）；四态=正常/离线/驱动未装/超温警报（整屏反白）
- 数据：模拟器直读 fixtures JSON；走势线可用合成历史数据填充展示
- 轮询/HTTP/WiFi 侧只搭骨架不联真机（真机联调属 P3，不在本卡）

## 验收标准（机械可核对）

1. firmware 构建命令跑通（沿用源项目构建入口，target esp32s3），产出 .bin，命令与产物路径写入汇报
2. 模拟器可运行，导出 8 张 PNG（2 页 × 4 态）至 `rig-lookout/artifacts/p2a/`，命名 `page1-normal.png`、`page1-offline.png`、`page1-nodriver.png`、`page1-alarm.png`、`page2-*` 同理
3. 红线核对通过：`git -C /Users/cui/Documents/Projects/codex-desk-terminal status --porcelain` 与 hermes-courier 同理，输出为空
4. 汇报格式：新增/修改路径清单、构建与运行命令、PNG 路径、遗留问题清单

## 红线

- codex-desk-terminal 与 hermes-courier 只读，任何文件不得修改（拷贝出来改副本）
- 不烧录任何板子；不动远程 Ubuntu 机器；不建 golden 基线（P2b 才做，本卡 PNG 只是预览产物）
- 不引入新框架、不换 LVGL 版本
