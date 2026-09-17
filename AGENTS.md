# rig-lookout 开发 Agent 协作约定

- 方案真源：`PLAN.md`。任务台账唯一真源：`STATUS.md`，不另建 backlog。
- 执行通道：ZCode 内置子代理单通道（2026-09-04 用户定规）；重卡派发前先查额度账本（~/.zcode/cli/db/db.sqlite model_usage，5h 窗口）；派后健康检查 = TaskOutput + `find -mmin` 双证。

## 红线

1. `codex-desk-terminal/` 与 `hermes-courier/` 两在役项目**只读引用**，资产拷贝出来改副本，上游任何文件不得修改（验收时 `git status --porcelain` 必须为空）。
2. golden 基线永不自动覆盖；测试截图不能消除失败。
3. 远程机 192.168.1.12 只做只读探测与 rig-stats 部署运维；**不装驱动、不删模型、不重启**（驱动/ComfyUI 恢复属 NVFP4 线）。
4. 导出器只读系统信息、无写能力；仅局域网暴露（ufw 限 192.168.1.0/24），路由器无端口映射、不加公网隧道。
5. 凭证（SSH 密码、token）不进 fixtures/日志/公开文档；token 本地存 `config/token.txt`（600）。

## 远程机速查

- `ssh cui@192.168.1.12`（免密已配；sudo 密码同登录密码）
- rig-stats：`systemctl status rig-stats`；env（含 token）：`/etc/rig-stats.env`（root:cui 640）；代码 `/opt/rig-stats/server.py`
