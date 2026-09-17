#!/usr/bin/env python3
"""rig-stats: read-only system/GPU stats exporter for the RLCD lookout screen.

Serves GET /health (no auth) and GET /stats (X-Token header) on the LAN.
A background thread samples once per second; HTTP handlers return the latest
snapshot instantly. Every field may be null -- the firmware renders '--'.
"""
import hmac
import json
import os
import shutil
import socket
import subprocess
import threading
import time
from collections import deque
from urllib.parse import urlparse, parse_qs

import psutil
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(os.environ.get("RIG_STATS_PORT", "7779"))
TOKEN = os.environ.get("RIG_STATS_TOKEN", "")
MOUNTS = [m.strip() for m in os.environ.get("RIG_STATS_MOUNTS", "/").split(",") if m.strip()]
SENSOR_CHAIN = [s.strip().lower()
                for s in os.environ.get("RIG_STATS_CPU_SENSORS",
                                        "coretemp,k10temp,zenpower,cpu_thermal,acpitz").split(",") if s.strip()]

_state = {"snap": None}
_net_prev = (psutil.net_io_counters(), time.monotonic())

# ---- 温度历史（哨兵节拍卡：服务端为唯一真源，板子按需回补）----
# 12h @10s 环形；仅记录 gpu.temp_c 非 None 的点（无驱动=无曲线，与 UI 隐藏口径一致）。
HISTORY_STEP_S = 10
HISTORY_MAX_POINTS = 4320
_history = deque(maxlen=HISTORY_MAX_POINTS)
_hist_last_ts = 0

# ---- 板子信标日志（板子随每次 /stats 轮询上报 b=电池mV c=节拍 r=RSSI）----
# JSONL 逐条落盘（>4MB 轮转保留一代 .old）；轮询时间戳本身即节拍/离线的证据。
BEACON_DIR = "/opt/rig-stats/logs"
BEACON_PATH = os.path.join(BEACON_DIR, "board.jsonl")
BEACON_MAX_BYTES = 4 * 1024 * 1024
_beacon_lock = threading.Lock()

def beacon_log(ip, b, c, r):
    try:
        with _beacon_lock:
            try:
                if os.path.getsize(BEACON_PATH) > BEACON_MAX_BYTES:
                    os.replace(BEACON_PATH, BEACON_PATH + ".old")
            except OSError:
                pass
            with open(BEACON_PATH, "a") as f:
                f.write(json.dumps({"ts": int(time.time()), "ip": ip,
                                    "b": b, "c": c, "r": r}) + "\n")
    except Exception:
        pass  # 信标日志失败不影响服务

GPU_QUERY = ["nvidia-smi",
             "--query-gpu=name,temperature.gpu,utilization.gpu,memory.used,memory.total,power.draw,power.limit,fan.speed",
             "--format=csv,noheader,nounits"]


def _num(field):
    if field in ("N/A", "[N/A]", ""):
        return None
    try:
        v = float(field)
    except ValueError:
        return None
    return None if v < 0 else v


def gpu_snapshot():
    empty = {"name": None, "temp_c": None, "util_pct": None, "vram_used_gb": None,
             "vram_total_gb": None, "power_w": None, "power_limit_w": None,
             "fan_pct": None, "driver": False}
    try:
        out = subprocess.run(GPU_QUERY, capture_output=True, text=True, timeout=3)
        if out.returncode != 0 or not out.stdout.strip():
            return empty
        p = [x.strip() for x in out.stdout.strip().splitlines()[0].split(",")]
        vram_used, vram_total = _num(p[3]), _num(p[4])  # MiB
        return {
            "name": p[0],
            "temp_c": _num(p[1]),
            "util_pct": _num(p[2]),
            "vram_used_gb": round(vram_used / 1024, 1) if vram_used is not None else None,
            "vram_total_gb": round(vram_total / 1024, 1) if vram_total is not None else None,
            "power_w": _num(p[5]),
            "power_limit_w": _num(p[6]),
            "fan_pct": _num(p[7]),
            "driver": True,
        }
    except Exception:
        return empty


def cpu_temp():
    """Return the hottest reading from the preferred sensor chip (coretemp has
    one entry per core; a monitor wants the hottest, not Core 0)."""
    try:
        temps = psutil.sensors_temperatures()
    except Exception:
        return None
    for key in SENSOR_CHAIN:
        for chip, entries in temps.items():
            if chip.lower() == key:
                vals = [e.current for e in entries if e.current is not None]
                if vals:
                    return round(max(vals))
    for entries in temps.values():  # fallback: first chip with any reading
        vals = [e.current for e in entries if e.current is not None]
        if vals:
            return round(max(vals))
    return None


def default_if():
    try:
        with open("/proc/net/route") as f:
            for line in f.readlines()[1:]:
                cols = line.split()
                if len(cols) > 2 and cols[1] == "00000000":
                    return cols[0]
    except Exception:
        pass
    return None


def disks_snapshot():
    out = []
    for m in MOUNTS:
        try:
            u = shutil.disk_usage(m)
            out.append({"mnt": m, "free_gb": round(u.free / 2**30), "total_gb": round(u.total / 2**30)})
        except Exception:
            pass
    return out


def sample():
    global _net_prev
    now = time.monotonic()
    counters = psutil.net_io_counters()
    prev, prev_t = _net_prev
    _net_prev = (counters, now)
    dt = max(now - prev_t, 1e-6)

    cores = psutil.cpu_percent(percpu=True)
    vm, sm = psutil.virtual_memory(), psutil.swap_memory()
    return {
        "schema": 1,
        "host": socket.gethostname(),
        "ts": int(time.time()),
        "net_if": default_if(),
        "uptime_s": int(time.time() - psutil.boot_time()),
        "gpu": gpu_snapshot(),
        "cpu": {"temp_c": cpu_temp(),
                "util_pct": round(sum(cores) / len(cores)) if cores else None,
                "cores_pct": [round(c) for c in cores]},
        "mem": {"used_gb": round(vm.used / 2**30, 1), "total_gb": round(vm.total / 2**30, 1),
                "util_pct": round(vm.percent),
                "swap_used_gb": round(sm.used / 2**30, 1), "swap_total_gb": round(sm.total / 2**30, 1)},
        "sys": {"load1": round(os.getloadavg()[0], 2),
                "net_rx_kbps": round((counters.bytes_recv - prev.bytes_recv) * 8 / dt / 1000),
                "net_tx_kbps": round((counters.bytes_sent - prev.bytes_sent) * 8 / dt / 1000),
                "disks": disks_snapshot()},
    }


def sampler_loop():
    global _hist_last_ts
    psutil.cpu_percent(percpu=True)  # prime the delta baseline
    while True:
        t0 = time.monotonic()
        try:
            snap = sample()
            _state["snap"] = snap
            gtemp = snap.get("gpu", {}).get("temp_c")
            if gtemp is not None and snap["ts"] - _hist_last_ts >= HISTORY_STEP_S:
                _hist_last_ts = snap["ts"]
                _history.append([snap["ts"], gtemp, snap.get("gpu", {}).get("util_pct")])
        except Exception:
            pass  # keep the last good snapshot; HTTP still answers
        time.sleep(max(0.0, 1.0 - (time.monotonic() - t0)))


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _send(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/health":
            self._send(200, {"ok": True, "service": "rig-stats",
                             "schema": 1, "ts": int(time.time())})
        elif path == "/stats":
            if not TOKEN or not hmac.compare_digest(self.headers.get("X-Token", ""), TOKEN):
                self._send(403, {"error": "forbidden"})
                return
            qs = parse_qs(urlparse(self.path).query)
            b = qs.get("b", [None])[0]
            if b is not None:  # 板子信标：记录即日志（轮询节奏/电压/节拍档/信号）
                beacon_log(self.client_address[0], b,
                           qs.get("c", [None])[0], qs.get("r", [None])[0])
            self._send(200, _state["snap"] or {"schema": 1, "warming_up": True})
        elif path == "/history":
            if not TOKEN or not hmac.compare_digest(self.headers.get("X-Token", ""), TOKEN):
                self._send(403, {"error": "forbidden"})
                return
            qs = parse_qs(urlparse(self.path).query)
            try:
                after = int(qs.get("after", ["0"])[0] or 0)
                limit = min(max(int(qs.get("limit", ["720"])[0] or 720), 1), HISTORY_MAX_POINTS)
            except ValueError:
                self._send(400, {"error": "bad query"})
                return
            pts = [p for p in _history if p[0] > after][-limit:]
            latest = _history[-1][0] if _history else int(time.time())
            self._send(200, {"schema": 1, "step_s": HISTORY_STEP_S,
                             "latest_ts": latest, "points": pts})
        else:
            self._send(404, {"error": "not found"})

    def log_message(self, *args):
        pass  # keep journald quiet at 0.5 rps from the ESP32


def main():
    threading.Thread(target=sampler_loop, daemon=True).start()
    print(f"rig-stats listening on 0.0.0.0:{PORT}, auth={'on' if TOKEN else 'OFF'}")
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()


if __name__ == "__main__":
    main()
