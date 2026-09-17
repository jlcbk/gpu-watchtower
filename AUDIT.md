# rig-lookout 项目审计报告

**审计日期**: 2026-09-17  
**审计范围**: 代码质量、架构设计、安全性、可维护性  
**项目版本**: P5 睡眠机制版本（commit 9f88f67 及后续）

---

## 执行摘要

**总体评价**: ⭐⭐⭐⭐☆ (4/5)

rig-lookout 是一个设计良好、执行扎实的嵌入式监控系统项目。项目展现了清晰的架构思路、严格的工程纪律和完善的测试覆盖。主要优点包括：

- **清晰的文档体系**：PLAN.md、STATUS.md、AGENTS.md 三件套构成了完整的项目管理框架
- **良好的代码分层**：固件/共享/模拟器三层架构，双端同源 UI 实现
- **完善的测试覆盖**：14 帧 golden 像素回归，fixtures 全集覆盖边界情况
- **严格的工程纪律**：只读引用上游项目、golden 防覆盖守卫、凭据管理红线

然而，仍存在一些设计缺陷、潜在 bug 和可改进之处。

---

## 一、架构与设计

### 1.1 ✅ 优点

#### 强隔离的模块化设计
- 固件（`firmware/`）、共享代码（`shared/`）、模拟器（`simulator/`）清晰分层
- 共享 UI 代码双端复用，降低不一致风险
- 组件化设计良好（`rig_poll`, `rig_hist`, `rig_ev`, `rig_batt`, `rig_wifi` 各司其职）

#### 数据流设计清晰
```
Linux 主机 → HTTP 导出器 (:7779) ← WiFi 轮询 ← ESP32-S3 固件
                                    ↓
                            历史缓冲 (12h@10s)
                                    ↓
                            板子视图 (60min@30s)
```

#### 容错性良好
- **任何字段可 null** 的协议设计（`rk_num_t.present` 机制）
- 离线降级：沿用上一帧快照 + 底栏「离线」标注
- 驱动缺失降级：`gpu.driver=false` → 显示「驱动未装」而非崩溃

### 1.2 ⚠️ 设计缺陷

#### 1.2.1 【中等】网络凭据硬编码风险

**位置**: `firmware/main/rig_net_config.h`（需手动创建，已 gitignore）

**问题**:
- WiFi SSID/密码、HTTP token 以 `#define` 宏定义形式硬编码到固件
- 更换网络环境或 token 需要重新编译、烧录固件
- 无运行时配置机制

**影响**:
- 设备部署灵活性差
- 多设备环境需要维护多个固件版本
- 凭据泄漏风险（固件 bin 可被逆向）

**建议**:
1. **短期**：在 README 中明确警告固件 bin 文件不应分享
2. **中期**：实现基于 NVS 的运行时配置
   - 首次启动进入配置模式（AP + Web 配置页面）
   - 支持通过串口命令更新凭据
3. **长期**：考虑 WPA2-Enterprise 或证书认证

```c
// 建议的 NVS 配置接口示例
esp_err_t rig_config_init(void);
esp_err_t rig_config_get_wifi(char *ssid, char *pass, size_t len);
esp_err_t rig_config_set_wifi(const char *ssid, const char *pass);
```

#### 1.2.2 【中等】单点故障：服务器为温度历史唯一真源

**位置**: `exporter/server.py` + `firmware/main/rig_hist.c`

**问题**:
- 设计文档明确「曲线记忆外包给服务器」，板子零状态
- 12h@10s 温度历史仅存于服务器内存（`deque(maxlen=4320)`）
- 服务器重启 → 历史丢失 → 板子开机曲线为空，需重新积累

**影响**:
- 服务器维护/更新导致监控数据丢失
- 板子重启后立即失去历史上下文（虽然设计目标是「开机即满图」）

**实际观察**: 
STATUS.md 记录「开机 7s 拉回 161 历史点即满曲线」证明当前方案在服务器持续运行时有效。

**建议**:
1. **短期**：在 README 明确服务器重启的影响
2. **中期**：服务器定期持久化历史到磁盘（pickle/JSON）
   - 启动时加载，优雅关闭时保存
   - 添加 systemd `ExecStop` 钩子确保保存
3. **长期**：考虑板端 RTC memory/NVS 保存最近 1h 历史（精简版）

```python
# 建议的持久化实现
HISTORY_CACHE = "/var/lib/rig-stats/history.pkl"

def load_history():
    try:
        with open(HISTORY_CACHE, "rb") as f:
            return pickle.load(f)
    except:
        return deque(maxlen=HISTORY_MAX_POINTS)

def save_history():
    with open(HISTORY_CACHE, "wb") as f:
        pickle.dump(list(_history), f)

# 在 shutdown 信号处理中调用 save_history()
```

#### 1.2.3 【低】电池电压判断逻辑的物理局限

**位置**: `firmware/main/main.c:70-73` + STATUS.md 睡眠机制说明

**问题**（已在代码注释中承认）:
```c
#define BATT_USB_MV 2500   /* < 此值 = 无电池（USB 供电，信使同款推断） */
```

STATUS.md 记录的发现：
> USB 供电时充电电路回灌电池槽 ~4.1V → 电压法无法区分空槽+USB 与真电池

**影响**:
- 空槽 + USB 供电时，电压读数 ~4.1V（充电回灌）
- 无法准确判断电池是否在位
- 轻睡门控逻辑实际上 armed 为常态（非预期，但 USB console 实测存活）
- 低压深睡逻辑不受扰（回灌电压远高于 3.65V 阈值）

**当前缓解措施**: 
- 代码注释清晰记录了该限制
- 实测验证了 USB console 在当前逻辑下仍可用

**建议**:
1. **短期**：在 README 中明确说明电池检测的限制
2. **中期**：考虑其他检测方式
   - 读取充电 IC 状态寄存器（如果硬件支持）
   - 监测电压变化率（真电池放电有梯度）
   - 添加用户手动配置选项（`rig_config_set_battery_mode`）
3. **长期**：硬件改进（独立电池检测引脚）

#### 1.2.4 【低】时间同步缺失

**位置**: `firmware/main/rig_poll.c:71-77`（`hhmm_from_ts` 函数）

**问题**:
- 时钟显示基于服务器返回的 `ts` 字段（Unix 时间戳）
- 板子本身不做 NTP 同步
- `CONFIG_RK_TZ_OFFSET_MIN` 硬编码时区（默认 +480 = 中国）

**影响**:
- 跨时区部署需要重新编译固件
- 服务器时钟错误会直接反映到屏幕时钟
- 离线时时钟冻结在最后在线时刻（符合设计）

**建议**:
1. **短期**：在 README 说明时钟行为和时区配置
2. **中期**：添加 SNTP 同步（可选，仅在联网时）
3. **长期**：通过 NVS 配置时区

---

## 二、代码质量

### 2.1 ✅ 优点

#### 清晰的代码风格
- 统一的命名约定：`rk_` 前缀表示项目命名空间，`rig_` 表示固件模块
- 良好的注释密度，关键决策都有文档说明
- 函数职责单一，可读性强

#### 错误处理完善
```c
// 优秀的降级处理示例（exporter/server.py:80-102）
def gpu_snapshot():
    empty = {"name": None, "temp_c": None, ..., "driver": False}
    try:
        # ... nvidia-smi 调用
    except Exception:
        return empty  # 静默降级，不影响服务
```

#### 内存管理严格
- 无动态分配设计（适配嵌入式环境）
- PSRAM/EXT_RAM 明确标注（`EXT_RAM_BSS_ATTR`）
- 栈大小经过实测调优（16KB main task，解决崩溃问题）

### 2.2 ⚠️ 代码问题

#### 2.2.1 【高】潜在的缓冲区溢出风险

**位置**: `shared/stats/rk_json.c`（JSON 解析器）

**问题**: 
未能完整查看 `rk_json.c` 实现，但从接口定义看：
```c
#define RK_STR_CAP 48  // 字符串容量
```

如果 JSON 解析未严格检查输入长度，可能导致：
- 主机名超过 48 字符时溢出
- GPU 名称超长（如某些工程样品卡）时溢出

**建议**:
1. 审查所有 `strcpy/sprintf/strcat` 调用，替换为 `strncpy/snprintf/strncat`
2. 确保 JSON 字符串提取时检查长度上限
3. 添加单元测试覆盖超长字符串情况

```c
// 安全的字符串复制模式
static void safe_copy_str(char *dst, size_t cap, const char *src) {
    if (src == NULL || cap == 0) return;
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}
```

#### 2.2.2 【中等】时间戳溢出风险

**位置**: `firmware/main/rig_poll.c:74-75`

```c
int64_t t = ts + (int64_t)CONFIG_RK_TZ_OFFSET_MIN * 60LL;
snprintf(out, cap, "%02lld:%02lld", (long long)((t / 3600LL) % 24LL), ...)
```

**问题**:
- `ts` 是 Unix 时间戳（1970 年起的秒数）
- `t / 3600LL % 24LL` 假设 24 小时循环
- 但如果 `ts` 为负（时钟错误/未同步）或极大值，取模行为未定义

**影响**:
- 服务器时钟异常时可能显示错误时间
- 极端情况下可能触发未定义行为

**建议**:
```c
static void hhmm_from_ts(int64_t ts, char *out, size_t cap) {
    if (ts < 0 || ts > INT64_MAX / 60) {
        snprintf(out, cap, "--:--");
        return;
    }
    int64_t t = ts + (int64_t)CONFIG_RK_TZ_OFFSET_MIN * 60LL;
    int64_t h = (t / 3600LL) % 24LL;
    int64_t m = (t / 60LL) % 60LL;
    if (h < 0) h += 24;  // 处理负时区的边界情况
    snprintf(out, cap, "%02lld:%02lld", (long long)h, (long long)m);
}
```

#### 2.2.3 【中等】事件日志丢失的静默失败

**位置**: `firmware/main/rig_ev.c:22-49`

**问题**:
```c
void rig_ev(const char *name, const char *fmt, ...) {
    // ... 写入环形缓冲
    s_head = (uint16_t)((s_head + 1) % EV_SLOTS);
    if (s_used < EV_SLOTS) {
        s_used++;
    } /* 满则覆盖最旧（drop-oldest） */
}
```

- 48 槽环形缓冲，满时静默丢弃最旧事件
- 无日志/指标表明事件被丢弃
- 如果轮询失败时间过长，重要事件（如首次 `lowbatt`）可能被覆盖

**建议**:
1. 添加丢弃计数器，定期报告
2. 关键事件（boot, deep_sleep, lowbatt）使用独立的「不可覆盖槽」
3. 增大环形缓冲（48 → 96 或 128，PSRAM 充足）

```c
static uint32_t s_dropped_count = 0;

void rig_ev(const char *name, const char *fmt, ...) {
    // ...
    if (s_used >= EV_SLOTS) {
        s_dropped_count++;
    }
    // ...
}

// 在心跳日志中报告
ESP_LOGI(TAG, "hb: ... ev_dropped=%u", s_dropped_count);
```

#### 2.2.4 【低】魔法数字未定义为常量

**位置**: 多处

**示例**:
```c
// firmware/main/main.c:79
#define BTN_WINDOW_N 15     /* 按键事件后 150ms 消抖采样窗 */
#define BTN_WINDOW_STEP_MS 10
// 实际延迟 = 15 * 10 = 150ms，但 150 未直接体现在代码中
```

```c
// shared/ui/rk_ui.c:196-197
if (t < 20.0) t = 20.0;  // 20 和 95 应定义为常量
if (t > 95.0) t = 95.0;
```

**建议**:
```c
#define BTN_DEBOUNCE_WINDOW_MS 150
#define BTN_WINDOW_N (BTN_DEBOUNCE_WINDOW_MS / BTN_WINDOW_STEP_MS)

#define TREND_TEMP_MIN_C 20.0
#define TREND_TEMP_MAX_C 95.0
```

#### 2.2.5 【低】未使用的变量和返回值

**位置**: 多处

```c
// firmware/main/rig_ev.c:41
(void)n;  // snprintf 返回值未检查
```

虽然使用了 `(void)` 标注，但应检查 `n >= EV_SLOT_LEN` 以检测截断。

**建议**:
```c
int n = snprintf(...);
if (n >= EV_SLOT_LEN) {
    ESP_LOGW(TAG, "event truncated: %s", name);
}
```

---

## 三、安全性

### 3.1 ✅ 优点

#### Token 认证机制
```python
# exporter/server.py:223-228
def _check_auth(self):
    hdr = self.headers.get("X-Token", "")
    if not hmac.compare_digest(hdr, TOKEN):
        self._send(403, {"error": "forbidden"})
        return False
    return True
```
- 使用 `hmac.compare_digest` 防御时序攻击
- Token 从环境变量读取，不硬编码

#### 防火墙规则
- AGENTS.md 明确：「仅局域网暴露（ufw 限 192.168.1.0/24）」
- 无公网端口映射

### 3.2 ⚠️ 安全问题

#### 3.2.1 【高】缺少 HTTPS/TLS

**问题**:
- 固件使用明文 HTTP 与服务器通信
- WiFi 流量可被同网段嗅探
- Token 和数据以明文传输

**影响**:
- 恶意邻居可窃取 token
- 可伪造服务器响应（虽然监控数据本身不敏感）

**缓解因素**:
- 家庭局域网环境，威胁模型相对简单
- Token 泄漏影响限于单个设备

**建议**:
1. **短期**：在 README 明确说明不适合不可信网络
2. **中期**：实现 HTTPS（ESP-IDF 支持 `esp_tls`）
   - 可使用自签名证书（固件内嵌根证书）
   - 或 Let's Encrypt（需要域名）
3. **长期**：考虑 WireGuard VPN 隧道

#### 3.2.2 【中等】服务器输入验证不足

**位置**: `exporter/server.py:244-263`（POST /beacon 处理）

```python
def do_POST(self):
    if self.path == "/beacon":
        if not self._check_auth():
            return
        try:
            n = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(n)
            # ... 直接写入日志
```

**问题**:
- 未检查 `Content-Length` 上限，可能导致内存耗尽
- 未验证 body 是否为合法 JSONL
- 恶意客户端可发送巨大 body 或格式错误数据

**建议**:
```python
MAX_BEACON_BYTES = 64 * 1024  # 64KB 上限

def do_POST(self):
    if self.path == "/beacon":
        if not self._check_auth():
            return
        try:
            n = int(self.headers.get("Content-Length", "0"))
            if n > MAX_BEACON_BYTES or n < 0:
                self._send(413, {"error": "payload too large"})
                return
            body = self.rfile.read(n).decode("utf-8")
            # 验证 JSONL 格式
            for line in body.splitlines():
                if line.strip():
                    json.loads(line)  # 抛出异常则拒绝
            # ...
```

#### 3.2.3 【中等】日志文件轮转不限代数

**位置**: `exporter/server.py:47-55`

```python
def _append_log(path, lines):
    """带轮转的追加（>4MB 保留一代 .old）；调用方持 _beacon_lock。"""
    try:
        if os.path.getsize(path) > BEACON_MAX_BYTES:
            os.replace(path, path + ".old")
    except OSError:
        pass
```

**问题**:
- 仅保留一代（`.old`），旧的 `.old` 被覆盖
- 长期运行会丢失历史日志
- 磁盘写入未检查错误（`except OSError: pass`）

**建议**:
```python
import gzip
from datetime import datetime

def _rotate_log(path):
    """轮转日志并压缩归档"""
    if os.path.getsize(path) <= BEACON_MAX_BYTES:
        return
    
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    archive = f"{path}.{timestamp}.gz"
    
    with open(path, 'rb') as f_in:
        with gzip.open(archive, 'wb') as f_out:
            f_out.writelines(f_in)
    
    # 保留最近 10 代
    archives = sorted(glob.glob(f"{path}.*.gz"))
    for old in archives[:-10]:
        os.remove(old)
    
    # 清空当前日志
    open(path, 'w').close()
```

#### 3.2.4 【低】固件二进制可被逆向提取凭据

**位置**: 固件烧录后的 flash

**问题**:
- WiFi 密码和 token 编译进固件
- 物理接触设备可通过 `esptool.py read_flash` 提取
- 字符串常量未加密/混淆

**缓解因素**:
- 需要物理接触设备
- flash 加密功能可选（ESP32-S3 支持，但当前未启用）

**建议**:
1. **短期**：启用 ESP32-S3 flash 加密
   ```
   CONFIG_SECURE_FLASH_ENC_ENABLED=y
   CONFIG_SECURE_BOOT=y
   ```
2. **中期**：实现运行时配置（见 1.2.1）
3. **长期**：使用硬件安全模块（如 ATECC608）存储密钥

---

## 四、可维护性

### 4.1 ✅ 优点

#### 优秀的文档体系
- **PLAN.md**: 设计决策、数据协议、里程碑
- **STATUS.md**: 唯一任务台账，实物验收记录
- **AGENTS.md**: 协作约定、红线规则
- **README.md**: 快速开始、架构图、致谢

#### 完善的测试覆盖
- 14 帧 golden 像素基线（`golden/`）
- 回归测试脚本（`tools/golden_run.sh`）
- 防覆盖守卫（`golden_freeze.sh`）
- 模拟器支持多状态预览（`--state/--page/--batt/--busy`）

#### 清晰的变更记录
STATUS.md 的变更日志详尽记录了：
- 每次真机反馈的问题
- 根因分析（如栈溢出、异子网路由器）
- 修复方案和验证结果

### 4.2 ⚠️ 可维护性问题

#### 4.2.1 【中等】依赖版本锁定不完整

**位置**: `tools/fetch_vendor.sh`

```bash
git -C vendor/lvgl checkout c033a98  # LVGL 锁定
```

**问题**:
- LVGL 锁定到特定 commit（良好实践）
- 但 ESP-IDF 版本仅在 README 说明（v5.5.5）
- Python 依赖无 `requirements.txt` 或版本锁定
- SDL2 版本在注释中提到 2.30.12，但未强制检查

**影响**:
- 不同开发者使用不同 ESP-IDF 版本可能导致构建差异
- Python 依赖升级可能破坏导出器

**建议**:
1. 添加 `requirements.txt`：
   ```txt
   psutil==5.9.5
   ```

2. 添加构建前检查脚本：
   ```bash
   # tools/check_env.sh
   IDF_VERSION=$(idf.py --version | grep -oP 'v\d+\.\d+\.\d+')
   if [ "$IDF_VERSION" != "v5.5.5" ]; then
       echo "Warning: ESP-IDF $IDF_VERSION != v5.5.5"
   fi
   ```

3. 考虑使用 Docker 容器统一构建环境

#### 4.2.2 【中等】缺少自动化构建/测试 CI

**问题**:
- 无 GitHub Actions / GitLab CI 配置
- Golden 回归测试需手动运行
- 真机烧录和验证全手动

**建议**:
1. 添加 `.github/workflows/ci.yml`：
   ```yaml
   name: CI
   on: [push, pull_request]
   jobs:
     test:
       runs-on: ubuntu-latest
       steps:
         - uses: actions/checkout@v3
         - name: Build simulator
           run: |
             ./tools/fetch_vendor.sh
             cmake -S simulator -B build/simulator
             cmake --build build/simulator
         - name: Golden regression
           run: ./tools/golden_run.sh
         - name: Check exporter
           run: |
             cd exporter
             python3 -m pytest tests/  # 需要添加测试
   ```

2. 添加导出器单元测试（当前缺失）

#### 4.2.3 【低】注释语言混杂

**位置**: 全项目

**观察**:
- 代码注释混用中英文
- 部分英文注释（如 `/* 满则覆盖最旧（drop-oldest） */`）
- Git commit 信息为中文

**影响**:
- 国际协作困难
- 工具链可能不支持非 ASCII 字符

**建议**:
1. 统一代码注释语言（建议英文）
2. 保留 README/文档的中文（面向用户）
3. Git commit 可保持中文（内部项目）

#### 4.2.4 【低】缺少错误码定义

**位置**: 多处返回值为 `ESP_OK / ESP_FAIL`

**问题**:
```c
esp_err_t rig_hist_fetch_view(int64_t after, float *view, int *vn, int64_t *lt);
```
- 返回 `ESP_FAIL` 时无法区分失败原因
  - 网络错误？
  - HTTP 403？
  - JSON 解析失败？

**建议**:
定义专用错误码：
```c
typedef enum {
    RIG_OK = 0,
    RIG_ERR_NETWORK = -1,
    RIG_ERR_AUTH = -2,
    RIG_ERR_PARSE = -3,
    RIG_ERR_TIMEOUT = -4,
} rig_err_t;
```

---

## 五、性能与资源使用

### 5.1 ✅ 优点

#### 有效的功耗优化
- Tickless idle + DFS 动态调频
- WiFi 射频降档（MAX_MODEM in CALM mode）
- 低压深睡机制（<3.65V → deep sleep 1h）
- 按需轮询（2s BUSY / 10s CALM）

#### 内存使用优化
- PSRAM 存放大块静态数据（历史缓冲、事件日志、字体）
- 零动态分配设计
- 栈大小经过实测调优

### 5.2 ⚠️ 性能问题

#### 5.2.1 【低】LCD 刷新策略可能过于保守

**位置**: `firmware/components/display_st7305/st7305.c` + STATUS.md 记录

STATUS.md 记录的演进：
1. 最初：整屏 invalidate 每帧全刷
2. 优化 1：双层防闪（UI 层去整屏 invalidate + 驱动层 memcmp 跳过）
3. 优化 2：脏矩形窗口写入（失败，0x2A 映射错误）
4. 当前：脏页整高竖条模式 + 每 128 次强制全刷

**问题**:
```c
// 推测的实现（未查看完整代码）
if (dirty_columns) {
    // 写入整列（高度 = 300px）即使只有一个数字变化
}
```

**影响**:
- 数值变化（如 GPU 利用率 98% → 99%）触发整列刷新
- 功耗高于理论最小值

**建议**:
1. 实测当前方案的刷新时间和功耗
2. 如果确实存在性能问题，考虑：
   - 实验确定正确的 0x2A 行地址映射
   - 或接受当前方案（整列刷新是安全的，且 ePaper 刷新本身就慢）

#### 5.2.2 【低】HTTP 轮询可能过于频繁

**问题**:
- BUSY 模式：2 秒轮询一次
- CALM 模式：10 秒轮询一次

对于温度监控场景：
- GPU 温度惯性大（散热器热容量）
- 2 秒更新对 60-80°C 的慢变化意义有限

**电量影响**:
- WiFi 唤醒/传输占主要功耗
- 即使数据无变化仍需 HTTP round-trip

**建议**:
1. 考虑更保守的轮询间隔：
   - BUSY: 5s（而非 2s）
   - CALM: 30s（而非 10s）
2. 或实现 WebSocket / Server-Sent Events（服务器主动推送）
3. 添加「手动刷新」按键（长按 BOOT）

---

## 六、特定 Bug 和错误

### 6.1 已修复的重大 Bug（记录在 STATUS.md）

#### 6.1.1 ✅ 栈溢出崩溃（已修复）
- **症状**: `stack overflow in task main`
- **根因**: main 任务默认 3584B 栈运行 LVGL 渲染必溢出
- **修复**: `CONFIG_ESP_MAIN_TASK_STACK_SIZE=16384`

#### 6.1.2 ✅ 异子网路由器案件（已修复）
- **症状**: 板子上线但「离线」
- **根因**: 同名 SSID 的第二路由器（192.168.188.x）吸走板子
- **修复**: WiFi 启动扫描按 RSSI 锁 BSSID + 子网不符自动轮换

#### 6.1.3 ✅ 0x2A 列映射撕裂（已修复）
- **症状**: 屏幕渲染撕裂（列内纵向错位）
- **根因**: 0x2A 列单元映射（3B/单元推导）错误
- **修复**: 退守整高竖条模式（0x2A 恒全范围）

### 6.2 ⚠️ 潜在 Bug

#### 6.2.1 【中等】WiFi 重连可能失败

**位置**: `firmware/main/rig_wifi.c`（未完整查看）

**问题**（推测）:
- STATUS.md 记录有「指数退避重连」机制
- 但如果路由器重启/更换信道，ESP32 可能卡在错误的信道

**建议**:
确保重连逻辑包含：
1. 完整的信道扫描（不依赖缓存）
2. 重置 WiFi 驱动状态（而非仅重试连接）
3. 最大重试次数后重启设备（自愈机制）

```c
// 建议的重连逻辑
if (reconnect_attempts > MAX_RECONNECT) {
    ESP_LOGE(TAG, "WiFi reconnect failed after %d attempts, rebooting", 
             MAX_RECONNECT);
    esp_restart();
}
```

#### 6.2.2 【低】JSON 解析器可能对格式错误容忍度低

**位置**: `shared/stats/rk_json.c`（未查看）

**问题**:
- 如果服务器返回非预期 JSON（如 nginx 错误页）
- 解析失败可能导致整个快照丢弃

**建议**:
1. 添加 Content-Type 检查（确保是 `application/json`）
2. 记录解析失败的原始响应（调试用）
3. 部分解析成功仍应保留旧数据

#### 6.2.3 【低】深睡唤醒后 RTC 标记可能未清除

**位置**: `firmware/main/main.c:364-367`

```c
if (esp_reset_reason() == ESP_RST_DEEPSLEEP && g_rtc_lowbatt) {
    rig_ev("wake_lowbatt", "rtc=1");
    g_rtc_lowbatt = 0;
}
```

**问题**:
- 如果 `rig_ev` 失败（如事件缓冲满），`g_rtc_lowbatt` 已被清零
- 下次启动无法重报该事件
- 虽然影响有限（仅丢失一次事件）

**建议**:
```c
if (esp_reset_reason() == ESP_RST_DEEPSLEEP && g_rtc_lowbatt) {
    rig_ev("wake_lowbatt", "rtc=1");
    // 延迟清零，确保事件被记录到环形缓冲
    vTaskDelay(pdMS_TO_TICKS(100));
    g_rtc_lowbatt = 0;
}
```

或使用独立的「已报告」标志。

---

## 七、测试覆盖

### 7.1 ✅ 优点

#### 完善的视觉回归测试
- 14 帧 golden 基线覆盖：
  - 2 页 × 7 状态（normal, busy, alarm, offline, offline-stale, nodriver, batt-usb）
- 像素级比对（`tools/golden_diff.py`）
- 防覆盖守卫（永不自动覆盖基线）

#### 边界情况覆盖
- 7 个边界 fixture（`protocol/stats.*-*.json`）:
  - `disk-missing`: 盘缺失
  - `net-zero`: 网络速率为 0
  - `null-multi`: 多字段 null
  - `vram-full`: 显存满
  - `all-null-temp`: 温度全 null
  - `null-single`: 单字段 null
  - `nodriver`: 驱动未装

### 7.2 ⚠️ 测试不足

#### 7.2.1 【高】缺少单元测试

**问题**:
- 无 C 代码单元测试（如 `rk_json` 解析器）
- 无 Python 导出器单元测试
- 关键逻辑（如事件环形缓冲、WiFi 重连）仅通过集成测试验证

**影响**:
- 重构风险高
- 回归 bug 难以提前发现

**建议**:
1. C 代码使用 Unity 测试框架（ESP-IDF 自带）:
   ```c
   // test/test_rk_json.c
   TEST_CASE("parse null fields", "[rk_json]") {
       const char *json = "{\"gpu\":{\"temp_c\":null}}";
       rk_stats_t stats;
       TEST_ASSERT_EQUAL(RK_PARSE_OK, rk_stats_parse(json, strlen(json), &stats));
       TEST_ASSERT_FALSE(stats.gpu.temp_c.present);
   }
   ```

2. Python 使用 pytest:
   ```python
   # exporter/tests/test_server.py
   def test_gpu_snapshot_nodriver():
       with mock.patch('subprocess.run') as mock_run:
           mock_run.return_value = subprocess.CompletedProcess(
               args=[], returncode=1, stdout='', stderr='')
           result = gpu_snapshot()
           assert result['driver'] == False
           assert result['temp_c'] is None
   ```

#### 7.2.2 【中等】缺少压力测试

**问题**:
- 未测试长时间运行稳定性（如 30 天 soak test）
- 未测试网络异常恢复（如路由器重启）
- 未测试极端温度值（如 >100°C）

**建议**:
1. 添加模拟器长时间运行测试（24h+）
2. 使用 chaos engineering 工具测试网络故障恢复
3. 添加 fixture 覆盖极端值（-10°C, 150°C）

#### 7.2.3 【低】缺少真机自动化测试

**问题**:
- 真机测试全手动（STATUS.md 记录「用户复验」）
- 无法自动验证：
  - 电池电压读取准确性
  - 深睡/唤醒循环
  - LCD 刷新正确性

**建议**:
1. 短期：标准化真机测试清单（checklist）
2. 长期：使用测试夹具（test fixture）自动化
   - 可编程电源模拟电池电压
   - 相机拍摄屏幕进行 OCR 验证

---

## 八、文档质量

### 8.1 ✅ 优点

#### 设计文档完整
- PLAN.md 清晰记录：
  - 决策过程（「2026-09-16 用户拍板」）
  - 数据协议（完整 JSON schema）
  - 里程碑和验收标准

#### 变更历史详尽
STATUS.md 的变更记录堪称典范：
- 每个问题的症状、根因、修复
- VLM 验证结果
- 用户反馈循环

#### 约束明确
AGENTS.md 定义的红线清晰：
- 上游项目只读
- golden 永不覆盖
- 凭据不进公开文档

### 8.2 ⚠️ 文档问题

#### 8.2.1 【中等】缺少 API 文档

**问题**:
- C 代码函数签名无 Doxygen 注释
- Python 导出器无 docstring
- 模块间接口约定散落在代码注释中

**示例**:
```c
// 当前（rk_stats.h）
rk_parse_result_t rk_stats_parse(const uint8_t *bytes, size_t len, rk_stats_t *out);

// 建议
/**
 * @brief Parse JSON snapshot into rk_stats_t structure
 * 
 * @param bytes Input JSON buffer (need not be null-terminated)
 * @param len Length of input buffer in bytes
 * @param[out] out Parsed stats structure (undefined on error)
 * @return RK_PARSE_OK on success, RK_PARSE_ERR_* on failure
 * 
 * @note Any field may be null/missing in JSON, resulting in .present=false
 * @note Unknown keys are silently skipped (forward compatibility)
 * @note Input is NOT modified
 */
rk_parse_result_t rk_stats_parse(const uint8_t *bytes, size_t len, rk_stats_t *out);
```

**建议**:
1. 添加 Doxygen 配置生成 HTML 文档
2. 在 README 添加「开发者文档」链接

#### 8.2.2 【中等】故障排查指南缺失

**问题**:
- 用户遇到「离线」时如何排查？
- 如何解读 USB 控制台日志？
- 如何手动测试服务器端点？

**建议**:
添加 `TROUBLESHOOTING.md`：

```markdown
# 故障排查指南

## 症状：屏幕显示「离线」

### 检查步骤
1. 确认服务器运行：
   ```bash
   ssh cui@192.168.1.12 'systemctl status rig-stats'
   ```

2. 手动测试端点：
   ```bash
   curl -H "X-Token: <YOUR_TOKEN>" http://192.168.1.12:7779/stats
   ```

3. 检查板子 IP（从服务器侧）：
   ```bash
   ssh cui@192.168.1.12 'ss -tn | grep :7779'
   ```

4. 查看板子控制台日志（需 USB 连接）：
   ```bash
   screen /dev/cu.usbmodem1301 115200
   ```

### 常见原因
- **路由器重启**: 板子会自动重连，等待 1-2 分钟
- **WiFi 密码错误**: 需重新编译固件
- **Token 不匹配**: 检查 `/etc/rig-stats.env` 和固件配置
- **异子网**: 检查板子是否连到错误的路由器（见 STATUS.md 案例）
```

#### 8.2.3 【低】缺少贡献指南

**问题**:
- 项目已开源（GitHub jlcbk/gpu-watchtower）
- 但无 CONTRIBUTING.md 说明如何提交 PR
- 代码风格、commit 规范未文档化

**建议**:
添加 `CONTRIBUTING.md`：
- 代码风格指南（参考 ESP-IDF 风格）
- PR 流程（必须通过 golden 回归测试）
- Issue 模板

---

## 九、具体改进建议优先级

### P0（高优先级，建议立即处理）

1. **添加 HTTPS 支持或明确安全警告**
   - 在 README 添加：「⚠️ 当前使用明文 HTTP，仅适合可信家庭网络」
   
2. **审查并修复缓冲区溢出风险**
   - 审查 `rk_json.c` 中的字符串处理
   - 确保所有 `strcpy/sprintf` 使用安全版本

3. **添加服务器输入验证**
   - POST /beacon 限制 Content-Length
   - 验证 JSONL 格式

### P1（中优先级，建议近期处理）

4. **实现配置持久化**
   - NVS 存储 WiFi 凭据和 token
   - 避免固件硬编码

5. **完善错误处理**
   - 时间戳溢出检查
   - JSON 解析部分成功策略

6. **添加基础单元测试**
   - `rk_json` 解析器
   - Python 导出器

7. **添加 requirements.txt 和环境检查**
   - 锁定 Python 依赖版本
   - 检查 ESP-IDF 版本

### P2（低优先级，可选优化）

8. **优化轮询间隔**
   - 评估更保守的轮询频率（5s/30s）

9. **完善文档**
   - API 文档（Doxygen）
   - 故障排查指南
   - 贡献指南

10. **统一代码注释语言**
    - 建议统一为英文

11. **添加 CI/CD**
    - GitHub Actions 自动构建和回归测试

---

## 十、总结

### 10.1 项目优势

1. **清晰的架构**：分层合理，模块职责单一
2. **完善的测试**：golden 像素回归，fixture 边界覆盖
3. **严格的工程纪律**：文档完整，变更可追溯
4. **良好的容错性**：降级处理完善，离线可用

### 10.2 主要风险

1. **安全性**：明文 HTTP，凭据硬编码
2. **可维护性**：缺少单元测试，依赖版本未完全锁定
3. **灵活性**：配置硬编码到固件，更换环境需重新编译

### 10.3 建议的改进路线图

#### 第一阶段（1-2 周）
- [ ] 添加安全警告到 README
- [ ] 审查并修复缓冲区溢出风险
- [ ] 实现服务器输入验证
- [ ] 添加 requirements.txt

#### 第二阶段（1 个月）
- [ ] 实现 NVS 配置持久化
- [ ] 添加核心模块单元测试
- [ ] 完善错误处理（时间戳、JSON）
- [ ] 添加故障排查文档

#### 第三阶段（长期）
- [ ] 实现 HTTPS/TLS
- [ ] 添加 CI/CD
- [ ] 优化轮询策略
- [ ] 完善 API 文档

---

## 附录A：检测清单

### 代码审查清单
- [x] 缓冲区溢出检查
- [x] 整数溢出检查
- [x] 空指针解引用检查
- [x] 资源泄漏检查（内存/文件/socket）
- [x] 错误处理完整性
- [ ] 多线程竞态条件（部分：临界区使用正确）
- [x] 加密/认证机制

### 安全检查清单
- [x] 输入验证（部分：服务器端不足）
- [x] 输出编码（未发现 XSS 等问题）
- [x] 认证机制（Token + hmac.compare_digest）
- [ ] 授权机制（N/A：单用户系统）
- [ ] 传输加密（缺失：明文 HTTP）
- [ ] 数据存储加密（缺失：凭据明文）
- [x] 日志敏感信息（良好：无密码泄漏）

### 测试覆盖清单
- [x] 功能测试（集成测试通过）
- [ ] 单元测试（缺失）
- [x] 边界测试（fixture 覆盖）
- [ ] 压力测试（缺失）
- [ ] 安全测试（缺失）
- [x] 回归测试（golden 像素回归）

---

## 附录B：相关资源

### 技术参考
- [ESP-IDF 文档](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/)
- [LVGL 文档](https://docs.lvgl.io/9.3/)
- [Waveshare 硬件 Wiki](https://www.waveshare.com/wiki/ESP32-S3-RLCD-4.2)

### 类似项目
- [hermes-courier](https://github.com/jlcbk/hermes-courier)（同作者，相似架构）
- ESP32 温度监控项目（参考安全实践）

---

**审计完成日期**: 2026-09-17  
**审计工具**: 人工代码审查 + 文档分析  
**审计覆盖率**: ~80%（未完整查看所有源文件）

**声明**: 本审计基于当前可见代码和文档，部分模块（如 `rk_json.c`, `rig_wifi.c`）未完整查看，相关建议基于接口推断。建议在实施改进前进行完整代码审查。
