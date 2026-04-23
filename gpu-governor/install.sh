#!/bin/sh
# Install gpu-governor daemon, service unit, and default config.
set -eu

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "Installing daemon to /usr/local/bin/"
sudo install -m 755 "$SELF_DIR/gpu-governor.py" /usr/local/bin/gpu-governor.py

echo "Installing systemd unit"
sudo install -D -m 644 "$SELF_DIR/gpu-governor.service" \
    /etc/systemd/system/gpu-governor.service

echo "Installing default config (if absent)"
sudo mkdir -p /etc/gpu-governor
sudo install -m 644 "$SELF_DIR/config.example" /etc/gpu-governor/config.example
if [ ! -f /etc/gpu-governor/config ]; then
    sudo cp "$SELF_DIR/config.example" /etc/gpu-governor/config
    echo "  Created /etc/gpu-governor/config"
else
    echo "  Kept existing /etc/gpu-governor/config"
fi

echo "Reloading systemd"
sudo systemctl daemon-reload

cat <<EOF

Installed. Next steps:
  sudo systemctl enable --now gpu-governor
  journalctl -u gpu-governor -f

To uninstall:
  sudo systemctl disable --now gpu-governor
  sudo rm /etc/systemd/system/gpu-governor.service /usr/local/bin/gpu-governor.py
  sudo rm -r /etc/gpu-governor
  sudo systemctl daemon-reload
EOF
