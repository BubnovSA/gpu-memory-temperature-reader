#!/bin/sh
# Install fan-governor daemon, service unit, and default config.
set -eu

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "Installing daemon to /usr/local/bin/"
sudo install -m 755 "$SELF_DIR/fan-governor.py" /usr/local/bin/fan-governor.py

echo "Installing systemd unit"
sudo install -D -m 644 "$SELF_DIR/fan-governor.service" \
    /etc/systemd/system/fan-governor.service

echo "Installing default config (if absent)"
sudo mkdir -p /etc/fan-governor
sudo install -m 644 "$SELF_DIR/config.example" /etc/fan-governor/config.example
if [ ! -f /etc/fan-governor/config.json ]; then
    sudo cp "$SELF_DIR/config.example" /etc/fan-governor/config.json
    echo "  Created /etc/fan-governor/config.json"
else
    echo "  Kept existing /etc/fan-governor/config.json"
fi

echo "Reloading systemd"
sudo systemctl daemon-reload

cat <<EOF

Installed. Next steps:
  sudo systemctl enable --now fan-governor
  journalctl -u fan-governor -f

To uninstall:
  sudo systemctl disable --now fan-governor
  sudo rm /etc/systemd/system/fan-governor.service /usr/local/bin/fan-governor.py
  sudo rm -r /etc/fan-governor
  sudo systemctl daemon-reload
EOF
