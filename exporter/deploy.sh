#!/bin/bash
# rig-stats one-shot idempotent deploy. Run as root on the Ubuntu box:
#   ssh cui@192.168.1.12 'echo <pw> | sudo -S bash /tmp/deploy.sh'
# Expects /tmp/rig-stats-server.py, /tmp/rig-stats.service, and optional
# /tmp/rig-stats.extra-env (KEY=VAL lines merged into /etc/rig-stats.env at creation).
set -euo pipefail

DIR=/opt/rig-stats
mkdir -p "$DIR/logs"
install -m 0644 /tmp/rig-stats-server.py "$DIR/server.py"
chown -R cui:cui "$DIR"

# dependency: psutil via apt (no pip/venv needed on a fresh box)
if ! python3 -c "import psutil" 2>/dev/null; then
  apt-get update -qq
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq python3-psutil
fi

ENVF=/etc/rig-stats.env
if [ ! -f "$ENVF" ]; then
  umask 027
  {
    echo "RIG_STATS_TOKEN=$(head -c16 /dev/urandom | od -An -tx1 | tr -d ' \n')"
    cat /tmp/rig-stats.extra-env 2>/dev/null || true
  } > "$ENVF"
fi
chown root:cui "$ENVF"
chmod 640 "$ENVF"

install -m 0644 /tmp/rig-stats.service /etc/systemd/system/rig-stats.service

# LAN-only firewall rule (records even while ufw is inactive; no-op if absent)
if command -v ufw >/dev/null 2>&1; then
  ufw allow from 192.168.1.0/24 to any port 7779 proto tcp comment 'rig-stats LAN' >/dev/null 2>&1 || true
fi

systemctl daemon-reload
systemctl enable --now rig-stats
sleep 1
echo "active=$(systemctl is-active rig-stats) enabled=$(systemctl is-enabled rig-stats)"
echo "--- env ---"
cat "$ENVF"
