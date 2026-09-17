#!/usr/bin/env python3
"""cadence_watch — 节拍转换哨兵（P4 工况验收取证）。

盯 /dev/cu.usbmodem1301 控制台，只把关键行落 reports/cadence-watch.log：
节拍升降档 / 警报 / 离线恢复 / 重启 / 栈溢出 / 异子网轮换。
默认跑 6 小时后退出（退出时尾部写 END 标记）。板子重插/刷机自动重连。
"""
import serial
import time

PORT = "/dev/cu.usbmodem1301"
DURATION_S = 6 * 3600
KEYS = ("cadence", "ALARM", "back online", "rst:0x", "stack overflow",
        "wrong subnet", "offline after")

def main():
    out = open("reports/cadence-watch.log", "a", buffering=1)
    out.write("=== watcher start %s ===\n" % time.strftime("%F %T"))
    s = None
    buf = b""
    end = time.time() + DURATION_S
    while time.time() < end:
        if s is None:
            try:
                s = serial.Serial(PORT, 115200, timeout=2)
            except Exception:
                time.sleep(5)
                continue
        try:
            data = s.read(8192)
        except Exception:
            try: s.close()
            except Exception: pass
            s = None
            continue
        if not data:
            continue
        text = (buf + data).decode("utf-8", "replace")
        lines = text.split("\n")
        buf = lines[-1].encode()
        for l in lines[:-1]:
            if any(k in l for k in KEYS):
                out.write("%s %s\n" % (time.strftime("%F %T"), l.strip()))
    out.write("=== watcher end %s ===\n" % time.strftime("%F %T"))

if __name__ == "__main__":
    main()
