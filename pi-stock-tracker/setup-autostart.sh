#!/bin/bash
# Makes the stock tracker start automatically whenever the Pi boots,
# and restart by itself if it ever crashes.
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
PY="$DIR/venv/bin/python"
[ -x "$PY" ] || PY="$(command -v python3)"

sudo tee /etc/systemd/system/stocktracker.service > /dev/null << EOF
[Unit]
Description=Pi Stock Tracker
After=network-online.target
Wants=network-online.target

[Service]
User=$USER
WorkingDirectory=$DIR
ExecStart=$PY $DIR/server.py
Restart=always
RestartSec=10
Environment=PYTHONUNBUFFERED=1

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable stocktracker
sudo systemctl restart stocktracker
IP="$(hostname -I | awk '{print $1}')"
echo
echo "Done! The stock tracker now runs in the background and starts on boot."
echo "Open:          http://$IP:8000"
echo "See its log:   journalctl -u stocktracker -f"
echo "Stop it:       sudo systemctl stop stocktracker"
echo "Turn off auto: sudo systemctl disable --now stocktracker"
