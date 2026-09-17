# STATUS — 唯一任务台账

> 规则：每卡记 owner/状态/证据路径。实物验收听磁盘，不听汇报。golden 永不自动覆盖。

| 卡 | 内容 | 状态 | 证据 |
|---|---|---|---|
| P0 | SSH 免密重建 + 环境核查 | ✅ done 2026-09-16 | PLAN.md §3；BatchMode ssh 实测通过 |
| P1 | rig-stats 导出器部署 | ✅ done 2026-09-16 | `reports/p1-verify.txt`（200/403/auth 三连过） |
| P2a | 固件骨架+模拟器四态 | ✅ done 2026-09-16 | `reports/p2a-acceptance.md`（8 PNG+VLM 抽检+上游干净） |
| P2b | fixtures 全集 + golden 回归（任务书已按四轮迭代后的 UI 现状重写） | ✅ done 2026-09-17 | 子代理 4.4M/22min + 主力亲验：14/14 PASS、注入自检检出、上游零脏、golden 防覆盖守卫生效；**UI 定稿冻结**（golden/ + README + tools/golden_run.sh）；顺手根治 P2a PNG writer CRC bug（120KB→15KB 标准 PNG）；7 个边界 fixture 入库（未入矩阵，ad-hoc 命令在 README） |
| P3 | 真机烧录联调 | 🔄 **首刷成功 2026-09-17**，余项见下 | 本行下方「真机事实」+ 服务器侧 ss 抓到 192.168.1.2 轮询 7779 |
| P4 | 工况验收 + 夜班 soak | ⏳ pending | 依赖 P3 余项 |
| P5 | 打磨 | 🔄 **睡眠机制+低压深睡已上线 2026-09-17（用户下单补齐，commit 9f88f67）**：①PM tickless+DFS + st7305 flush 持 APB 锁 + 主循环事件驱动重构（按键 ISR/poll 信号/10s 兜底，去 10ms 轮询）②空闲 WiFi 降 MAX_MODEM（CALM 5min，活动恢复）③**低压深睡：电池 <3.65V×3 确认 → 清屏仅显「休眠中」→ esp_wifi_stop → deep sleep 1h / BOOT(ext0) 唤醒复查 ≥3.75V 迟滞**。**新发现（改写设计假设）**：USB 供电时充电电路回灌电池槽 ~4.1V → 电压法无法区分空槽+USB 与真电池（courier「空座显 USB」在本板不成立；屏幕电池位空槽显 ~4.1V 属物理行为）→ 轻睡门控 armed 为常态，USB console 实测存活（90s 连续读零异常）；低压逻辑不受扰（回灌远高于阈值，边充边低电的边角由 1h 复查自愈）。golden 14/14 保持。**DEVICE_PENDING**：真电池低压深睡路径/轻睡实际入睡电流/休眠屏观感未实测（无电池）。**余项**：2D 窗口标定实验、model.last_online 清理、休眠中大字号（现 unifont16） |

## 真机事实（2026-09-17 首刷，主力亲手）

- **板上只有一块板**（此前「1301/1605 两块在役板」认知有误）：同一块板两个 USB 人格——
  - `usbmodem1605` = TinyUSB CDC 人格（Waveshare 描述符 SN0002/VID 2bdf），esptool 复位序列打不开，不可用于烧录
  - `usbmodem1301` = USB-Serial-JTAG 人格，`--before default_reset` 正常进下载模式可烧录（codex 项目 P4.1c 历史一致）
- **烧录 SOP**：`esptool --chip esp32s3 -p /dev/cu.usbmodem1301 -b 460800 --before default_reset --after hard_reset write_flash @flash_args`（build 目录）。若只见到 1605 人格：按住 BOOT 拔插 USB 可切回 1301 人格
- **控制台在 UART0（引脚 43/44）不在 USB**（sdkconfig 沿用 codex 项目）——USB 只见 bootloader 头几行，应用日志（WiFi/轮询）USB 上不可见；P5 可议改 USB console
- 首刷内容：P2a 骨架 + 主力补齐的网络层（rig_wifi.c 拉起/WiFi 凭据注入；修 X-Token 双重头名 403 bug、改 esp_http_client open→read_response 正统取包、接通指数退避 next_delay_ms——P2a 网络分支此前从未被编译过）
- 板子 IP：192.168.1.2（DHCP）；服务器侧 ss 连续 10s 抓到其对 :7779 的 established 连接（2s 节拍特征），WiFi+轮询+鉴权链路全通
- 被顶掉的旧固件：板上原固件已被覆盖（可从相应仓库刷回）

## P3 余项（并入 P4 前收尾）

- [ ] 用户目检屏幕实况（温度大字/四条/走势线/BOOT 翻页）——显示产品的最终验收在人眼
- [ ] 走势线现为合成数据；接真实 12h/30s 环形缓冲
- [ ] WiFi RSSI 记录、24h 稳定性观察
- [ ] 屏摄存档 `artifacts/board/`

## 已知基线与裁定

- hermes-courier 2 条先于本项目的脏状态：保留不动（`reports/p2a-acceptance.md` 裁定）
- 模拟器 PNG writer 输出非常规：P2b 建基线前统一编码

## 变更记录

- 2026-09-16 立项 + P0/P1/P2a 三连收（详见各报告）。
- 2026-09-17 凌晨：用户要求真机首刷。排查 1605 人格不可烧录 → 1301 人格 default_reset 一发入魂；网络层三处缺陷主力修复；全链路（WiFi→轮询→渲染）服务器侧证实；等用户屏摄终验。
- 2026-09-17 UI 修复轮（用户真机反馈四条）：①幻影翻页根因=boot_short_press 消抖初值不一致（开机第一循环误触发一次 GPU→SYS，屏「卡」在系统页）→ 重写为首调采样不触发；②BOOT(GPIO0)+KEY(GPIO18，Waveshare 官方表证实低有效) 双键等价翻页；③次页去 6 核小条阵改 CPU/RAM/SWAP 三大条（用户定稿）；④防闪屏双层：rk_ui_refresh 去整屏 invalidate + st7305_flush 同帧 memcmp 跳推（真·部分窗口写入无官方先例，留 P5 实验）；NET 10K 档量化防数字连跳。模拟器真实数据预检双页 VLM 全绿后刷板，轮询恢复证实。**用户复验：SYS 页不闪 ✓。**
- 2026-09-17 UI 修复轮二（用户反馈三条，设计语言升级=「离线不是事件，是角落标注」）：①离线不再整页接管——两页删离线横幅，数据沿用上一份好快照照常渲染，仅底栏亮黑底「离线」小标（时钟同步显示最后成功时刻）；②盘改纯数值行去进度条；③显卡页同规则 + util 5%/pwr 5W 档量化（满载抖动不再连跳推帧）。模拟器三图 VLM 预检（正常×2+离线新行为）全绿后刷板，轮询恢复。**待用户复验。**
- 2026-09-17 **崩溃根因破案（用户反馈「GPU 还是闪/离线丢旧数据/页跳回 GPU」三症一体）**：USB 控制台抓到 `stack overflow in task main` + addr2line 实锤 `app_main→rk_ui_refresh→lv_refr_now→LVGL 渲染链`——**main 任务默认 3584B 栈跑 LVGL 渲染必溢出**（P2a 拷资产漏了 codex/courier 的大栈配置；边缘型溢出解释时好时坏）。修复=CONFIG_ESP_MAIN_TASK_STACK_SIZE=16384，验证 0 重启。顺带：主控制台切 USB-Serial-JTAG（排障日志可见）、poll 任务栈 6144→8192。
- 2026-09-17 **异子网路由器案件**：板子 2.4G 上被同名 SSID「Cui」的第二路由器（192.168.188.x）吸走→够不着服务器=「离线」。修复=rig_wifi 启动扫描同名 AP 按 RSSI 锁 BSSID + GOT_IP 子网不符自动轮换下一 AP（好 AP 回线自愈）；验证=板子拿到 192.168.1.182 恢复轮询，控制台静默（成功不打日志）。
- 2026-09-17 **真·局部矩形刷新上线**：st7305 增脏矩形窗口写入（0x2B 页=2 列宽竖条×0x2A 列单元=12 行高条带的字节算术映射；脏页连段×脏列单元区间→矩形写，>5120B 或出错退回全刷；每 128 次 flush 强制全刷自愈映射错位）。**映射为推导值未经数据手册证实，若用户见渲染错位→自愈机制兜底+可一键退全刷模式；待用户目检确认。**
- 2026-09-17 **撕裂反馈→0x2A 映射证伪，退守页窗口整高竖条模式**：用户照片 VLM 取证=撕裂严格局限在所写条带 x 范围内（右列数据区内容纵向错位/行间串扰），左半屏/顶栏/页脚完好→**0x2B 页映射证实正确，0x2A 列单元映射（3B/单元推导）错误**（正是上条预警的风险）。定稿：0x2A 恒用全范围，仅按脏页连段做整高竖条窗口写（字节序与全帧流完全同构，零几何假设）；内容保证正确，代价=数值变化的刷新范围放大到「变化列的整高条带」（2D 紧凑矩形需一次标定实验后再启用，P4 待办）。已刷板轮询恢复。用户复验：画面正常 ✓。
- 2026-09-17 **UI 细节四连（用户反馈）**：①显卡名自动换行两行（原单行截断 RTX5...）②右列四行整体上移（ys 64/100/136/172→40/76/112/148）③标题行主机名→电池电压（新 rig_batt.c：官方 ESP-IDF ADC 路径 ADC1_CH3/DB_12/curve-fitting/×3 分压/4 读平均；<2.5V 显 "USB" 无电池判定，信使 H02 同款；10s 节律）④**走势线接线**——P2a 以来 trend 恒 NULL 的占位补齐：12h@30s 环形缓冲（1440 点 PSRAM）+ 近 60min=120 点视图，30s 一点渐进生长。模拟器（新增 --batt）+真实数据 VLM 预检五项全绿后刷板；boot 日志 batt adc ready / 单候选 AP / IP 正常 / 0 重启。
- 2026-09-17 **UI 细节五连（用户反馈+方案拍板）**：①电池位迁至底栏与时钟同排（页码·离线·电池·时钟）②标题行改「渲染中」反白状态牌（util≥15% 亮、空载隐身——信使「空闲即隐身」哲学；主力方案）③走势 scale 下限 40→20°C（GPU 双态分布 25-35/70-78 均获分辨率，75°C 刻度含义不变；拒绝动态自适应 scale=丢绝对参考）。模拟器 --busy 预检 VLM 四项全绿后刷板，轮询正常 0 重启。
- 2026-09-17 **哨兵节拍·服务端先行**：导出器 v2 部署（主力亲手）——新增 12h@10s 温度环形历史 + `/history?after=<ts>&limit=<n>` 增量接口（token 鉴权同 /stats；仅记 gpu.temp 非 null 点）；验收=10s 落点节奏/增量拉取/板子 /stats 不受扰全过。固件侧（节拍状态机+曲线换源+睡眠）待 P2b 验收后实施。
- 2026-09-17 **P2b golden 冻结 + 哨兵节拍固件上线（同日双线收官）**：P2b=14 帧 golden 冻结（UI 定稿锁定）+ 注入自检 + PNG writer 根治；哨兵节拍固件（主力亲手，1 次构建通过）=rig_poll 节拍 FSM（CALM 10s/BUSY 2s，触发 util≥15/温度≥75/警报，降档 5 周期迟滞）+ rig_hist 新模块（/history 拉取+解析+stride3 降采样 120 点视图）+ main.c 双缓冲临界区交接+板端 12h 环形删除。console 验证=开机 7s 拉回 161 历史点即满曲线、30s 回补节奏正、空闲 CALM 静默、0 重启 0 溢出。**待真实渲片工况验证 BUSY 升降档**（P4 联测）。
- 2026-09-17 **git 入库 + 节拍哨兵**：项目 git init（commit c1b0511 v0.2，201 文件；沿 codex 惯例 vendor/ 与 third_party/dl 不入库、凭据文件确认未跟踪、空模板入库）；后台挂 tools/cadence_watch.py（6h 串口哨兵，节拍/警报/重启关键行落 reports/cadence-watch.log）——下次真实渲片自动捕获 BUSY 升降档证据。
- 2026-09-17 **板子信标日志上线（commit 2db79de，用户需求=长期可回看的工作日志）**：板子每次 /stats 轮询附 ?b=电池mV&c=节拍&r=RSSI，导出器逐条落 `/opt/rig-stats/logs/board.jsonl`（JSONL，>4MB 轮转一代）+ 固件每 10min 控制台心跳行。**日志体系三层定型**：①信标 JSONL（永久、不依赖 Mac——节拍切换看时间戳间隔、电压趋势、信号质量、离线=断流）②USB 控制台（插线即看，含 panic/深睡/降档细节）③Mac 节拍哨兵（关键行过滤落盘）。已验证：10-11s 节奏可见、b=4128-4131（USB 回灌实锤）、r=-51~-61。查询口径：`ssh cui@192.168.1.12 'tail /opt/rig-stats/logs/board.jsonl'`。
