#!/bin/bash
# Autoadjust Harvester Pi Setup Script
# Run this after flashing Ubuntu 24.04 LTS and SSHing into the Pi
#
# Prerequisites:
# 1. Upload this script to the Pi
# 2. Upload autoadjust_harvester_poll.py and autoadjust_harvester_tcp_server.py to /home/rooted/
#
# Usage: chmod +x setup_harvester_pi.sh && ./setup_harvester_pi.sh

set -e  # Exit on error

# Timeout wrapper function (seconds, command...)
run_with_timeout() {
    local timeout_sec=$1
    shift
    echo "  Running (${timeout_sec}s timeout): $*"
    if ! timeout $timeout_sec "$@"; then
        echo "ERROR: Command timed out or failed after ${timeout_sec}s: $*"
        exit 1
    fi
}

echo "========================================="
echo "Autoadjust Harvester Pi Setup"
echo "========================================="

# Update and install git
echo ""
echo "[1/9] Updating system and installing git..."
run_with_timeout 300 sudo apt update
run_with_timeout 120 sudo apt install -y git

# Clone TE CLI
echo ""
echo "[2/9] Cloning TE CLI repository..."
cd ~
if [ -d "te-cli" ]; then
    echo "te-cli directory already exists, skipping clone"
else
    run_with_timeout 300 git clone https://github.com/Grayhill/te-cli.git
fi

# Install venv and build TE CLI
echo ""
echo "[3/9] Setting up Python virtual environment and installing TE CLI..."
run_with_timeout 120 sudo apt install -y python3.12-venv
cd ~/te-cli
python3 -m venv venv
. venv/bin/activate
run_with_timeout 120 python3 -m pip install --upgrade pip
run_with_timeout 300 python3 -m pip install .
run_with_timeout 300 python3 -m pip install ".[dev]"

# Install HID libraries
echo ""
echo "[4/9] Installing HID libraries..."
run_with_timeout 180 sudo apt-get install -y libhidapi-hidraw0 libhidapi-libusb0 libusb-1.0-0 libusb-1.0-0-dev
run_with_timeout 120 pip install hidapi

# Update udev permissions
echo ""
echo "[5/9] Updating udev permissions..."
cd ~/te-cli
sudo cp udev/* /etc/udev/rules.d
run_with_timeout 30 sudo udevadm control --reload-rules
run_with_timeout 30 sudo udevadm trigger

# Configure network
echo ""
echo "[6/9] Configuring network (static eth0 + Wi-Fi)..."
sudo tee /etc/netplan/50-cloud-init.yaml >/dev/null <<'NETPLAN'
network:
    version: 2
    renderer: networkd

    ethernets:
        eth0:
            dhcp4: no
            addresses:
              - 192.168.10.1/24
            optional: true

    wifis:
        wlan0:
            dhcp4: yes
            access-points:
                "urbanfarms":
                    password: "RoboticFarming"
NETPLAN
run_with_timeout 30 sudo netplan apply
sudo ip link set wlan0 up || true

# Move scripts if they exist in home directory
echo ""
echo "[7/9] Setting up harvester scripts..."
cd ~/te-cli
if [ -f ~/autoadjust_harvester_poll.py ]; then
    mv ~/autoadjust_harvester_poll.py .
    echo "Moved autoadjust_harvester_poll.py to te-cli"
fi
if [ -f ~/autoadjust_harvester_tcp_server.py ]; then
    mv ~/autoadjust_harvester_tcp_server.py .
    echo "Moved autoadjust_harvester_tcp_server.py to te-cli"
fi

# Check scripts exist
if [ ! -f autoadjust_harvester_poll.py ] || [ ! -f autoadjust_harvester_tcp_server.py ]; then
    echo "WARNING: One or both Python scripts not found in ~/te-cli/"
    echo "Please upload them manually before starting services"
fi

chmod +x autoadjust_harvester_poll.py autoadjust_harvester_tcp_server.py 2>/dev/null || true

# Create default JSON
echo ""
echo "[8/9] Creating default JSON file..."
python3 - <<'PY'
import json, os
p="/home/rooted/te-cli/TE_Variable_Values.json"
os.makedirs(os.path.dirname(p), exist_ok=True)
if not os.path.exists(p):
    with open(p,"w") as f: json.dump({"ready_to_run":False,"active_variety":None}, f, indent=4)
print("JSON file ready")
PY

# Create and enable systemd services
echo ""
echo "[9/9] Creating and enabling systemd services..."
sudo tee /etc/systemd/system/autoadjust_harvester_poll.service >/dev/null <<'EOF'
[Unit]
Description=Autoadjust Harvester Poll
After=network-online.target
Wants=network-online.target
[Service]
Type=simple
User=rooted
WorkingDirectory=/home/rooted/te-cli
ExecStart=/home/rooted/te-cli/venv/bin/python /home/rooted/te-cli/autoadjust_harvester_poll.py
Restart=always
RestartSec=2
NoNewPrivileges=true
[Install]
WantedBy=multi-user.target
EOF

sudo tee /etc/systemd/system/autoadjust_harvester_tcp_server.service >/dev/null <<'EOF'
[Unit]
Description=Autoadjust Harvester TCP Server
After=network-online.target
Wants=network-online.target
[Service]
Type=simple
User=rooted
WorkingDirectory=/home/rooted/te-cli
ExecStart=/home/rooted/te-cli/venv/bin/python /home/rooted/te-cli/autoadjust_harvester_tcp_server.py
Restart=always
RestartSec=2
NoNewPrivileges=true
[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable autoadjust_harvester_poll.service
sudo systemctl enable autoadjust_harvester_tcp_server.service
sudo systemctl start autoadjust_harvester_poll.service
sudo systemctl start autoadjust_harvester_tcp_server.service

# Verify
echo ""
echo "========================================="
echo "Setup complete! Verifying services..."
echo "========================================="
echo ""
echo "Poll service: $(sudo systemctl is-active autoadjust_harvester_poll.service)"
echo "TCP service:  $(sudo systemctl is-active autoadjust_harvester_tcp_server.service)"
echo ""
echo "To check logs:"
echo "  sudo journalctl -u autoadjust_harvester_poll.service -f"
echo "  sudo journalctl -u autoadjust_harvester_tcp_server.service -f"
echo ""
echo "To test TCP server:"
echo "  nc 192.168.10.1 8888"
