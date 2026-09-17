# P2a 验收记录（主力实物验收，2026-09-16 20:3x）

**结论：PASS。**

## 磁盘证据（不听汇报）

- `artifacts/p2a/` 8/8 PNG 在盘；md5 去重后 7 张唯一（`page1-alarm` == `page2-alarm` 为设计如此：警报牌整屏接管两页）
  - 首次疑点：8 张字节数完全相同（120,373B）→ 查校验和证实非同图，系 1-bit 内容灰度 PNG 压缩巧合
- 固件产物 `firmware/build/rk_device.bin`（3,397,296 B，本机既有 ESP-IDF v5.5.5 构建，target esp32s3）
- 模拟器产物 `build/simulator/rig-lookout-sim`（SDL2 headless 可跑）
- 上游核对：codex-desk-terminal `git status --porcelain` = 0 条 ✅

## VLM 抽检（2/2 合规）

- `page1-normal`：五要素全中——72°C 超大字、UTIL 98%/VRAM 13.2/16G/PWR 152/170W/FAN 62% 四组条、TREND 60MIN 走势实线+75°C 阈值虚线、右上反色 WiFi 标签、页脚 F1/2 GPU + 时钟
- `page1-alarm`：整屏黑底白字反白、⚠ 超温警报、GPU 87°C 居中最大字、≥85C 30S RULE、时间戳——一眼警报达标

## 裁决与备注

1. **hermes-courier 基线脏 2 条**（`simulator/out/session.log` mtime Sep 10 早于本卡 6 天 + `vendor/waveshare-rlcd` 嵌套仓自脏）：裁定**保留不动**——非本卡造成，清理属该仓库 owner 事务，动它反而违反只读红线。已在 STATUS 记为已知基线。
2. 模拟器 PNG writer 输出 Read 工具不可直接解码、体积虚胖（120KB/帧）；sips/file 均可正常解析。P2b 建golden 前须统一编码（已写入 P2b 任务书）。
3. 遗留 7 项见子代理汇报（走势合成数据、真机 WiFi/HTTP 未测、° 字形 fallback 16px 等），全部归 P3/P4/P5，不阻塞验收。

## 成本

子代理 12.8M tokens / 36 分钟（重卡预估 25-55M 的下沿）。
